#pragma once

namespace sunrise::client::photo {

/**
 * Applies the photo-mode HUD policy through the game's own activity lifetime state: state 1
 * hides the game HUD, state 3 is the live default. Mirrors the Activity Host window's
 * lifetime action, so it runs on the frame thread.
 * @param hidden True while the photo switch is on.
 */
void set_hud_hidden(bool hidden) noexcept;

} // namespace sunrise::client::photo
