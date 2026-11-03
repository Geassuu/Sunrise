/**
 * The photo-mode HUD policy. The game hides its HUD for activity lifetime states 1 and 2, so
 * the photo switch drives the type-17 lifetime sensor through the same path the Activity Host
 * window's Lifetime page uses: bind the SDK view against the live host instance, then set the
 * state. State 3 is the live default the switch hands back on exit.
 */

#include "photo_hud.h"

#include <array>
#include <cstdint>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../server/activity/activity_sdk_lifetime_runtime.h"
#include "../../server/activity/host_runtime.h"
#include "../../server/bap/runtime.h"
#include "../../state/activity_sdk/runtime.h"

namespace sunrise::client::photo {
namespace {

namespace host = server::activity::host;
namespace lifetimes = server::activity::activity_sdk_lifetime;
namespace sdk = state::activity_sdk;

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
    const sdk::Snapshot catalog = sdk::snapshot();
    if (!catalog) {
        report_fail("catalog");
        return;
    }
    host::DiagnosticsSnapshot diagnostics{};
    host::snapshot(diagnostics);
    if (diagnostics.instanceCount == 0) {
        report_fail("instance");
        return;
    }
    // The local session is the only live host instance offline.
    const host::InstanceSnapshot& instance = diagnostics.instances[0];
    server::bap::ActivityLinkView link{};
    (void)server::bap::activity_link_view(instance.binding, link);
    const sdk::Selection selection{
        instance.binding, link.matchingLinks, link.activityClientGeneration};
    sdk::BoundView view{};
    if (sdk::resolve(catalog, selection, view) != sdk::Status::ready) {
        report_fail("resolve");
        return;
    }
    server::bap::ActivityLinkView current{};
    (void)server::bap::activity_link_view(instance.binding, current);
    if (sdk::revalidate(view, instance.binding, current.matchingLinks,
                        current.activityClientGeneration)
        != sdk::Status::ready) {
        report_fail("revalidate");
        return;
    }
    const std::uint8_t state = hidden ? kHiddenLifetimeState : host::kDefaultLifetimeState;
    const auto status = lifetimes::set(view, state);
    if (status != server::activity::activity_sdk_lifetime::Status::ready) {
        report_fail("set");
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
