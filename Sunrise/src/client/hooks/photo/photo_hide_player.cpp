/**
 * Hide-player: skips the local player's skinned draws while photo mode is on. The detour wraps
 * the skinned pose producer the Sunrise Studio link also observes, one call per skinned draw.
 * A draw whose owner is the local player's object is answered with zero, which the caller reads
 * as "nothing was produced for this draw" and drops from the frame. The player's arms, weapon
 * and body disappear, leaving a clean camera. If a future build changes that contract, the
 * Photo page's hide-player toggle turns the skip off without a rebuild.
 */

#include "photo.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

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

using Producer = char(__fastcall*)(std::uintptr_t, void*, std::uintptr_t, void*, void*, int);

std::atomic_bool g_installed{false};
hooking::detour::Handle g_hideHandle{};

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
        if (memory::copy_current_process(
                nullptr,
                draw + kDrawOwnerOffset,
                std::as_writable_bytes(std::span(&owner, 1)))
            && owns_player_object(owner)) {
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
