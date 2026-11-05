/**
 * The camera carrier spawn. One squad member is placed through the same generated-SDK route
 * the Activity Host window's squad page uses, and the dynamic-handle capture is opened so the
 * per-frame refresh picks the spawned actor's object handle up as the carrier. The carrier is
 * an ordinary character: the identity redirect makes the game treat it as the local player,
 * while the hide detour skips its draws so it stays invisible.
 */

#include "photo_carrier.h"

#include <array>
#include <cstdint>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../middleware/bap/activity_message/squad_auth_body.h"
#include "../../server/activity/activity_sdk_squad_runtime.h"
#include "../../server/activity/host_runtime.h"
#include "../../server/bap/runtime.h"
#include "../../state/activity_sdk/runtime.h"
#include "../hooks/photo/photo.h"

namespace sunrise::client::photo {
namespace {

namespace host = server::activity::host;
namespace sdk = state::activity_sdk;
namespace squads = server::activity::activity_sdk_squads;

/** @param reason Key naming the step that failed. */
void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=photo stage=carrier result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Spawns one squad member as the camera carrier. */
void spawn_carrier() noexcept {
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
    if (view.catalog == nullptr || view.catalog->squads().empty()) {
        report_fail("squads");
        return;
    }
    // The first authored squad, one member, ordinary placement.
    const std::array<std::int32_t, 1> counts{1};
    const auto status = squads::place(
        view,
        0,
        counts,
        middleware::bap::activity_message::squad_auth::Mode::mode0);
    if (status != squads::Status::queued && status != squads::Status::ready) {
        report_fail("place");
        return;
    }
    hooks::photo::begin_carrier_capture();
}

} // namespace sunrise::client::photo
