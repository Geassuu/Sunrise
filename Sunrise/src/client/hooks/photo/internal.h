#pragma once

#include <atomic>
#include <cstdint>

namespace sunrise::client::hooks::photo {

/** The value the game's controlled-handle getter writes while the player controls nothing. */
inline constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFFU;

/** The local player's object handle, refreshed once a frame on the camera thread. */
extern std::atomic<std::uint32_t> g_playerHandle;
/** The hide-player detour is attached. */
extern std::atomic_bool g_hideInstalled;
/** One capture request latched by the shutter or the menu, waiting for the presentation thread. */
extern std::atomic_bool g_capturePending;

} // namespace sunrise::client::hooks::photo
