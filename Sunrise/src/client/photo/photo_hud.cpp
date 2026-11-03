/**
 * The photo-mode HUD policy. The game hides its HUD for activity lifetime states 1 and 2, so
 * the photo switch drives the type-17 lifetime sensor through the message-5 path itself: the
 * current activity link for the local slice supplies the exact region and generation the
 * lifetime override queues against. State 3 is the live default the switch hands back on exit.
 */

#include "photo_hud.h"

#include <array>
#include <cstdint>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../server/bap/runtime.h"
#include "../../server/activity/host_runtime.h"

namespace sunrise::client::photo {
namespace {

namespace host = server::activity::host;

/** Lifetime state the game reads as "HUD hidden". */
inline constexpr std::uint8_t kHiddenLifetimeState = 1;

/** @param reason Key naming the step that failed. */
void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=photo stage=hud result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Applies the photo-mode HUD policy through the activity lifetime state. */
void set_hud_hidden(bool hidden) noexcept {
    // The exact ActivityClient the local slice renders from; no unique-link requirement, so it
    // works while several activity links are still arriving.
    server::bap::CurrentActivityLinkView link{};
    if (!server::bap::current_activity_link_view(-1, link)) {
        report_fail("link");
        return;
    }
    if (link.effectiveRegion < 0) {
        report_fail("region");
        return;
    }
    const std::uint8_t state = hidden ? kHiddenLifetimeState : host::kDefaultLifetimeState;
    if (!server::bap::request_activity_lifetime_override(link.binding,
                                                         state,
                                                         link.effectiveRegion,
                                                         link.activityClientGeneration)) {
        report_fail("queue");
        return;
    }
    std::array<char, 64> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=photo stage=hud state=%u",
                                      static_cast<unsigned>(state));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace sunrise::client::photo
