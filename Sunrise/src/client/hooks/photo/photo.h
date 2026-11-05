#pragma once

#include <cstdint>
#include <string_view>

#include "../teleport/runtime.h"

// Forward-declared so this header stays free of the D3D headers, whose `interface` macro
// corrupts any account header included after them. The capture implementation includes the
// full definitions in its own translation unit.
struct IDXGISwapChain;
struct ID3D11Device;
struct ID3D11DeviceContext;

namespace sunrise::client::hooks::photo {

/**
 * Fastest speed the game is shown while the free camera drives the position itself. Contact with
 * geometry damages the player above roughly this, and the real speed is not needed for movement.
 */
inline constexpr float kPublishedSpeedCap = 8.0F;

/** Reads the toggle key once a frame and flips the switch on the press. */
void poll_toggle() noexcept;

/** Reads the shutter key once a frame and latches one capture request on the press. */
void poll_shutter() noexcept;

/** Latches one capture request directly; the menu's take-photo button uses this. */
void request_capture() noexcept;

/**
 * Queues one folder-open request for the next camera-thread poll. Shell work never runs inside
 * the synchronous present call, the same rule the credits badge follows.
 * @param folder UTF-8 folder to open; empty opens the default photo folder.
 */
void request_open_folder(std::string_view folder) noexcept;

/** Runs one queued folder-open request through the Windows shell. */
void dispatch_open_folder() noexcept;

/**
 * Writes the player's velocity on the physics sync, which publishes it.
 * @param component Physics component being synced. Tested for player ownership here.
 */
void apply(void* component) noexcept;

/** @return True while the free camera is on. */
[[nodiscard]] bool enabled() noexcept;

/** @return True while the detached camera owns the view instead of the player position. */
[[nodiscard]] bool detached_active() noexcept;

/**
 * Drives the detached camera. Called from the camera-frame hook after the game computes its
 * own pose. The invisible player body is carried to the accumulated free-camera position, so
 * the game's own camera follows it: the body becomes an empty character the camera rides on.
 * @param playerIndex Player whose camera block is written.
 */
void drive_camera(std::uint32_t playerIndex) noexcept;

/**
 * Reads the staged free-camera position the physics pin carries the player body to.
 * @param position Receives the position.
 * @return True while a position is staged.
 */
[[nodiscard]] bool freecam_body_position(teleport::Vector& position) noexcept;

/**
 * Applies a pending HUD state. Called every frame from the renderer thread, the same thread
 * the Activity Host window uses, so the lifetime push shares its thread contract.
 */
void sync_hud() noexcept;

/**
 * Snapshots the dynamic handles and opens the carrier-capture wait; the per-frame refresh
 * finishes it when the spawned actor appears.
 */
void begin_carrier_capture() noexcept;

/**
 * Sets the velocity the coming simulation step integrates.
 * @param body Character rigid body. Live only inside the step hook.
 */
void before_step(void* body) noexcept;

/** Refreshes the cached local-player object handle. Call once a frame on the camera thread. */
void refresh_player_handle() noexcept;

/** @return True when the local player's skinned draws must be skipped. */
[[nodiscard]] bool hide_player_active() noexcept;

/**
 * @param owner Draw owner handle from one skinned draw record.
 * @return True when the cached local player owns that object.
 */
[[nodiscard]] bool owns_player_object(std::uint32_t owner) noexcept;

/** @return True when one capture request is waiting for the presentation thread. */
[[nodiscard]] bool screenshot_pending() noexcept;

/**
 * Captures the swap chain's back buffer into a JPEG in the configured folder. Runs on the
 * presentation thread before the overlay draws, so the frame holds only the game image.
 * @param swapChain Swap chain whose back buffer is captured.
 * @param device Device the staging texture is made on.
 * @param context Context used for the copy and read-back.
 */
void try_capture(IDXGISwapChain* swapChain,
                 ID3D11Device* device,
                 ID3D11DeviceContext* context) noexcept;

/**
 * Attaches the hide-player detour. Installed with the other cheat hooks, because the free
 * camera it serves is a cheat tool. A failed attach is logged, not fatal.
 * @return True when the detour is attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the hide-player detour and clears the runtime state. */
void uninstall() noexcept;

/** Clears the key states and any waiting capture. The switches are stored and survive. */
void reset() noexcept;

} // namespace sunrise::client::hooks::photo
