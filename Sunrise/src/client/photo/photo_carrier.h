#pragma once

namespace sunrise::client::photo {

/**
 * Arms the camera-carrier spawn. The per-frame poll runs the placement once the activity
 * link settles; call from the panel.
 */
void spawn_carrier() noexcept;

/**
 * Runs one pending spawn attempt. Called every frame from the renderer thread; transient
 * link-state refusals retry silently, definitive refusals log once.
 */
void poll_carrier_spawn() noexcept;

} // namespace sunrise::client::photo
