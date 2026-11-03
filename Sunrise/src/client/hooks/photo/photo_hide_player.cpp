/**
 * Hide-player: skips the local player's skinned draws while photo mode is on. The detour wraps
 * the skinned pose producer the Sunrise Studio link also observes, one call per skinned draw.
 * A draw belongs to the player when its owner is the player's object, or any object whose
 * parent chain reaches the player: the first-person weapon and the class-item cloth hang off
 * child objects the game creates under the player's entity. Such draws are answered with zero,
 * which the caller reads as "nothing was produced for this draw" and drops from the frame.
 * If a future build changes that contract, the Photo page's hide-player toggle turns the skip
 * off without a rebuild.
 */

#include "photo.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>
#include <type_traits>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../memory/current_process_memory.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/signature_text.h"
#include "internal.h"

namespace sunrise::client::hooks::photo {
namespace {

/** The producer pattern the Studio pose link resolves; it matches one function per build. */
constexpr std::string_view kProducerText =
    "4C 8B DC 55 41 54 41 56 49 8D AB C8 FC FF FF 48 81 EC 20 04 00 00 "
    "48 8B 05 ? ? ? ? 48 33 C4 48 89 85 B0 02 00 00 0F 10 41 18";
/** Compiled pattern bytes of the producer signature. */
constexpr auto kProducer =
    patterns::signature<patterns::signature_length(kProducerText)>(kProducerText);

/** The draw record's owner handle sits four bytes in, the same read the Studio observer makes. */
constexpr std::size_t kDrawOwnerOffset = sizeof(std::uint32_t);

/** Object memory offsets the Studio datum readers also use. */
constexpr std::size_t kObjectSelfOffset = 0x0C;
constexpr std::size_t kObjectParentOffset = 60;
/** The datum head that carries the parent link; a shorter stride cannot store it. */
constexpr std::uint32_t kDatumLinkBytes = 64;
/** Handle slot bits, the same mask the Studio datum reader applies. */
constexpr std::uint32_t kSlotMask = 0x1FFFU;
/** A parent chain longer than this is treated as unreadable; it guards against a cycle. */
constexpr std::size_t kMaximumAncestors = 8;

using Producer = char(__fastcall*)(std::uintptr_t, void*, std::uintptr_t, void*, void*, int);

std::atomic_bool g_installed{false};
hooking::detour::Handle g_hideHandle{};

/** Diagnostic counters, reset by each report line. */
std::atomic<std::uint64_t> g_seen{0};
std::atomic<std::uint64_t> g_hidden{0};
std::atomic<std::uint64_t> g_lastReport{0};
/** Distinct owners matched by the parent rule, so the report stays one line per object. */
std::array<std::atomic<std::uint64_t>, 16> g_parentMatches{};
/** How often the counter line repeats while the hide is on. */
constexpr std::uint64_t kReportIntervalMs = 5000;

/** What a draw owner matched, in increasing distance from the player. */
enum class Match : std::uint32_t {
    none = 0,
    player = 1,
    parent = 2,
};

/**
 * Reads one scalar from this process under a fault guard.
 * @param address Source address.
 * @param output Receives the value.
 * @return True when every byte arrived.
 */
template <typename Value> [[nodiscard]] bool read(std::uintptr_t address, Value& output) noexcept {
    static_assert(std::is_trivially_copyable_v<Value>);
    return memory::copy_current_process(
        nullptr, address, std::as_writable_bytes(std::span(&output, 1)));
}

/** @return The object pool base and stride values, or false while they cannot be read. */
[[nodiscard]] bool pool(std::uintptr_t& base, std::uint32_t& stride) noexcept {
    const std::uintptr_t baseSlot = g_poolBaseSlot.load(std::memory_order_relaxed);
    const std::uintptr_t strideSlot = g_poolStrideSlot.load(std::memory_order_relaxed);
    if (baseSlot == 0 || strideSlot == 0) {
        return false;
    }
    // The slots hold the game's current pool values; reading them per call keeps a teardown from
    // publishing a stale base through the cached slot addresses.
    return read(baseSlot, base) && base != 0 && read(strideSlot, stride)
           && stride >= kDatumLinkBytes;
}

/** @return The parent handle of one object, or kInvalidHandle when it cannot be read. */
[[nodiscard]] std::uint32_t parent_of(std::uint32_t handle, std::uintptr_t base,
                                      std::uint32_t stride) noexcept {
    const std::uint64_t offset = static_cast<std::uint64_t>(stride) * (handle & kSlotMask);
    const std::uintptr_t object = base + static_cast<std::uintptr_t>(offset);
    std::uint32_t self = kInvalidHandle;
    std::uint32_t parent = kInvalidHandle;
    if (!read(object + kObjectSelfOffset, self) || self != handle
        || !read(object + kObjectParentOffset, parent)) {
        return kInvalidHandle;
    }
    return parent;
}

/**
 * Tests one draw owner against the local player. The match set is the player object and its
 * parent: the first-person weapon hangs off the player's chain, while entity-level attachments
 * like the class-item cloth can hang off the player's parent as a sibling object.
 * @param owner Draw owner handle to test.
 * @return What the owner matched, or Match::none.
 */
[[nodiscard]] Match match_kind(std::uint32_t owner) noexcept {
    if (owns_player_object(owner)) {
        return Match::player;
    }
    const std::uint32_t player = g_playerHandle.load(std::memory_order_acquire);
    if (player == kInvalidHandle) {
        return Match::none;
    }
    const std::uint32_t playerParent = g_playerParent.load(std::memory_order_relaxed);
    std::uintptr_t base = 0;
    std::uint32_t stride = 0;
    if (!pool(base, stride)) {
        return Match::none;
    }
    std::uint32_t cursor = owner;
    for (std::size_t depth = 0; depth < kMaximumAncestors; ++depth) {
        const std::uint32_t parent = parent_of(cursor, base, stride);
        if (parent == kInvalidHandle) {
            return Match::none;
        }
        if (parent == player) {
            return Match::player;
        }
        if (parent == playerParent) {
            return Match::parent;
        }
        cursor = parent;
    }
    return Match::none;
}

/** Logs the owner once, the first time the parent rule hides it. */
void report_parent_match(std::uint32_t owner) noexcept {
    const std::uint64_t entry =
        (static_cast<std::uint64_t>(owner) << 32) | static_cast<std::uint64_t>(Match::parent);
    for (std::atomic<std::uint64_t>& slot : g_parentMatches) {
        std::uint64_t expected = 0;
        if (slot.compare_exchange_strong(expected, entry, std::memory_order_relaxed)) {
            std::array<char, 96> line{};
            const int written = std::snprintf(
                line.data(), line.size(), "ev=photo stage=hide_parent owner=0x%08X",
                static_cast<unsigned>(owner));
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
            }
            return;
        }
        if (expected == entry) {
            return;
        }
    }
}

/** Emits one counter line every few seconds, so a skipped draw set is visible in the log. */
void report_counters() noexcept {
    const std::uint64_t now = GetTickCount64();
    std::uint64_t last = g_lastReport.load(std::memory_order_relaxed);
    if (now - last < kReportIntervalMs
        || !g_lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
        return;
    }
    const std::uint64_t seen = g_seen.exchange(0, std::memory_order_relaxed);
    const std::uint64_t hidden = g_hidden.exchange(0, std::memory_order_relaxed);
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=photo stage=hide seen=%llu hidden=%llu",
                                      static_cast<unsigned long long>(seen),
                                      static_cast<unsigned long long>(hidden));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * One skinned draw, called by the game while the render frame is built.
 * A hidden player draw never reaches the original producer.
 */
char __fastcall hide_body(std::uintptr_t draw,
                          void* flags,
                          std::uintptr_t pose,
                          void* allocator,
                          void* corrections,
                          int pass) noexcept {
    if (hide_player_active() && draw != 0) {
        std::uint32_t owner = kInvalidHandle;
        if (read(draw + kDrawOwnerOffset, owner)) {
            g_seen.fetch_add(1, std::memory_order_relaxed);
            const Match kind = match_kind(owner);
            if (kind != Match::none) {
                g_hidden.fetch_add(1, std::memory_order_relaxed);
                if (kind == Match::parent) {
                    report_parent_match(owner);
                }
                // Zero tells the caller nothing was produced, which drops the draw from the frame.
                return 0;
            }
            report_counters();
        }
    }
    const Producer original = reinterpret_cast<Producer>(g_hideHandle.original);
    return original != nullptr ? original(draw, flags, pose, allocator, corrections, pass) : 0;
}

/** @param reason Key naming the step that failed. */
void report_install_failure(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=photo stage=install result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Reads one object's parent handle from the datum pool. */
std::uint32_t datum_parent(std::uint32_t handle) noexcept {
    if (handle == kInvalidHandle) {
        return kInvalidHandle;
    }
    std::uintptr_t base = 0;
    std::uint32_t stride = 0;
    if (!pool(base, stride)) {
        return kInvalidHandle;
    }
    return parent_of(handle, base, stride);
}

/** Resolves the producer and attaches the hide-player detour. */
bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    std::byte* const producer = patterns::scan_main_image_unique(kProducer, "photo_hide_player");
    if (producer == nullptr) {
        report_install_failure("producer");
        return false;
    }
    if (!hooking::detour::install(
            hooking::detour::Spec{producer, reinterpret_cast<void*>(&hide_body)}, g_hideHandle)) {
        report_install_failure("attach");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    g_hideInstalled.store(true, std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=photo stage=install result=ok");
    return true;
}

/** Disarms the skip first, then detaches the producer detour. */
void uninstall() noexcept {
    g_hideInstalled.store(false, std::memory_order_release);
    if (!g_installed.load(std::memory_order_acquire)) {
        return;
    }
    // A failed detach keeps the handle, so the detour still reaches the original. The skip is
    // already off, which makes the replacement inert.
    if (!hooking::detour::uninstall(g_hideHandle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=photo stage=uninstall result=fail");
        return;
    }
    g_installed.store(false, std::memory_order_release);
}

} // namespace sunrise::client::hooks::photo
