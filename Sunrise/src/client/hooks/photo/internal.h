#pragma once

#include <array>
#include <atomic>
#include <cstdint>

namespace sunrise::client::hooks::photo {

/** The value the game's controlled-handle getter writes while the player controls nothing. */
inline constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFFU;

/** The local player's object handle, refreshed once a frame on the camera thread. */
extern std::atomic<std::uint32_t> g_playerHandle;
/** The player object's parent, refreshed per frame; entity-level attachments hang off it. */
extern std::atomic<std::uint32_t> g_playerParent;
/** The hide-player detour is attached. */
extern std::atomic_bool g_hideInstalled;
/** One capture request latched by the shutter or the menu, waiting for the presentation thread. */
extern std::atomic_bool g_capturePending;
/** Object-pool storage-slot addresses, cached while the world-object hooks accept reads. */
extern std::atomic<std::uintptr_t> g_poolBaseSlot;
extern std::atomic<std::uintptr_t> g_poolStrideSlot;
/** Distinct model handles of the player's skinned draws, collected for the read-only probe. */
extern std::array<std::atomic<std::uint32_t>, 16> g_playerModels;

/**
 * Reads one object's parent handle from the datum pool. The slot addresses must be cached
 * already, which the per-frame refresh does first.
 * @param handle Native object handle.
 * @return The parent handle, or kInvalidHandle when the pool or the object cannot be read.
 */
[[nodiscard]] std::uint32_t datum_parent(std::uint32_t handle) noexcept;

/**
 * Applies or restores the hidden-part flags while the hide is on. Runs on the camera thread
 * from the per-frame refresh, so its state needs no lock. Restores everything when the hide
 * turns off or the module resets.
 */
void update_part_flags() noexcept;

/** The camera carrier's object handle; zero until a carrier is captured. */
extern std::atomic<std::uint32_t> g_carrierHandle;

/**
 * Appends one photo event to `Sunrise\logs\photo.log`, so the trace lands on disk even when
 * the game's own log is not flushed. Debug aid; appended per event, never truncated here.
 */
void trace(const char* stage, const char* text) noexcept;

} // namespace sunrise::client::hooks::photo
