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

/**
 * Walks one object's parent chain and tests it against the local player.
 * The weapon and the class-item cloth are child objects of the player's entity, so their draws
 * carry a different owner handle than the body's and need the ancestor test.
 * @param owner Draw owner handle to test.
 * @return True when the owner is the player's object or has it as an ancestor.
 */
[[nodiscard]] bool belongs_to_local_player(std::uint32_t owner) noexcept {
    if (owns_player_object(owner)) {
        return true;
    }
    const std::uint32_t player = g_playerHandle.load(std::memory_order_acquire);
    if (player == kInvalidHandle) {
        return false;
    }
    const std::uintptr_t baseSlot = g_poolBaseSlot.load(std::memory_order_relaxed);
    const std::uintptr_t strideSlot = g_poolStrideSlot.load(std::memory_order_relaxed);
    if (baseSlot == 0 || strideSlot == 0) {
        return false;
    }
    // The slots hold the game's current pool values; reading them per call keeps a teardown from
    // publishing a stale base through the cached slot addresses.
    std::uintptr_t base = 0;
    std::uint32_t stride = 0;
    if (!read(baseSlot, base) || base == 0 || !read(strideSlot, stride)
        || stride < kDatumLinkBytes) {
        return false;
    }
    std::uint32_t cursor = owner;
    for (std::size_t depth = 0; depth < kMaximumAncestors; ++depth) {
        const std::uint64_t offset =
            static_cast<std::uint64_t>(stride) * (cursor & kSlotMask);
        const std::uintptr_t object = base + static_cast<std::uintptr_t>(offset);
        std::uint32_t self = kInvalidHandle;
        std::uint32_t parent = kInvalidHandle;
        if (!read(object + kObjectSelfOffset, self) || self != cursor
            || !read(object + kObjectParentOffset, parent) || parent == kInvalidHandle) {
            return false;
        }
        if (parent == player) {
            return true;
        }
        cursor = parent;
    }
    return false;
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
        if (read(draw + kDrawOwnerOffset, owner) && belongs_to_local_player(owner)) {
            // Zero tells the caller nothing was produced, which drops the draw from the frame.
            return 0;
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
