#pragma once

namespace sunrise::client::photo {

/**
 * Spawns one squad member as the camera carrier and opens the dynamic-handle capture. Runs on
 * the frame thread; the capture finishes on the camera thread's refresh.
 */
void spawn_carrier() noexcept;

} // namespace sunrise::client::photo
