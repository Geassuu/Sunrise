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
 * Read-only probe: resolves every collected player model handle and logs the model record
 * layout, so the part table that carries the 0x40 hidden-part flag can be located. Logs each
 * model once. Runs on the camera thread, so its state needs no lock.
 */
void probe_player_models() noexcept;

} // namespace sunrise::client::hooks::photo
