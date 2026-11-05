/**
 * The camera carrier spawn. One squad member is placed through the same generated-SDK route
 * the Activity Host window's squad page uses, and the dynamic-handle capture is opened so the
 * per-frame refresh picks the spawned actor's object handle up as the carrier. The carrier is
 * an ordinary character: the identity redirect makes the game treat it as the local player,
 * while the hide detour skips its draws so it stays invisible.
 */

#include "photo_carrier.h"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../middleware/bap/activity_message/squad_auth_body.h"
#include "../../server/activity/activity_sdk_squad_runtime.h"
#include "../../server/activity/host_runtime.h"
#include "../../server/bap/runtime.h"
#include "../../state/activity_sdk/runtime.h"
#include "../hooks/photo/photo.h"
#include "../hooks/photo/internal.h"

namespace sunrise::client::photo {
namespace {

namespace host = server::activity::host;
namespace sdk = state::activity_sdk;
namespace format = state::activity_sdk::format;
namespace squads = server::activity::activity_sdk_squads;
namespace squad_auth = middleware::bap::activity_message::squad_auth;

/** @param reason Key naming the step that failed. */
void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=photo stage=carrier result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
        hooks::photo::trace("carrier", line.data() + 22);
    }
}

} // namespace

/** Whether a spawn attempt is waiting for the per-frame poll. */
std::atomic_bool g_spawnPending{false};
/** Frames the current attempt has retried transient failures. */
std::size_t g_spawnAttempts{0};

/** Spawns one squad member as the camera carrier. */
void spawn_carrier() noexcept {
    g_spawnAttempts = 0;
    g_spawnPending.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::warn,
                     "ev=photo stage=carrier result=pending");
    hooks::photo::trace("carrier", "result=pending");
}

/**
 * Retries the spawn until the activity link settles. The resolve needs exactly one
 * authenticated link, which loading and transitions do not always provide; every other
 * refusal is definitive and logged once.
 */
void poll_carrier_spawn() noexcept {
    if (!g_spawnPending.load(std::memory_order_relaxed)) {
        return;
    }
    ++g_spawnAttempts;
    if (g_spawnAttempts > 600) {
        g_spawnPending.store(false, std::memory_order_relaxed);
        report_fail("timeout");
        return;
    }
    const sdk::Snapshot catalog = sdk::snapshot();
    if (!catalog) {
        report_fail("catalog");
        g_spawnPending.store(false, std::memory_order_relaxed);
        return;
    }
    host::DiagnosticsSnapshot diagnostics{};
    host::snapshot(diagnostics);
    if (diagnostics.instanceCount == 0) {
        report_fail("instance");
        g_spawnPending.store(false, std::memory_order_relaxed);
        return;
    }
    const host::InstanceSnapshot& instance = diagnostics.instances[0];
    server::bap::ActivityLinkView link{};
    (void)server::bap::activity_link_view(instance.binding, link);
    const sdk::Selection selection{
        instance.binding, link.matchingLinks, link.activityClientGeneration};
    sdk::BoundView view{};
    // Transient: retry silently until the link count settles.
    if (sdk::resolve(catalog, selection, view) != sdk::Status::ready) {
        return;
    }
    server::bap::ActivityLinkView current{};
    (void)server::bap::activity_link_view(instance.binding, current);
    if (sdk::revalidate(view, instance.binding, current.matchingLinks,
                        current.activityClientGeneration)
        != sdk::Status::ready) {
        return;
    }
    g_spawnPending.store(false, std::memory_order_relaxed);
    const format::Scenario* const scenario = sdk::bound_scenario(view);
    if (scenario == nullptr) {
        report_fail("scenario");
        return;
    }
    const auto scenarioSquads = sdk::scenario_squads(*view.catalog, *scenario);
    if (scenarioSquads.empty()) {
        report_fail("squads");
        return;
    }
    // The first squad whose authored flags allow a placement, the same rule the host UI uses
    // to enable its place button.
    std::optional<std::uint32_t> chosen{};
    std::size_t chosenMemberCount = 0;
    for (const auto& candidate : scenarioSquads) {
        const std::uint32_t row =
            static_cast<std::uint32_t>(&candidate - view.catalog->squads().data());
        if (!format::squad_runnable(candidate.flags)) {
            continue;
        }
        const auto members = sdk::squad_members(*view.catalog, candidate);
        if (members.empty()) {
            continue;
        }
        std::array<std::int32_t, squad_auth::kMaximumRequestedCountLength> requested{};
        const std::size_t count = (std::min)(members.size(), requested.size());
        requested[0] = 1;
        const std::span<const std::int32_t> counts(requested.data(), count);
        constexpr auto mode = squad_auth::Mode::mode0;
        if (squads::availability(view, row, counts, mode) == squads::Status::ready) {
            chosen = row;
            chosenMemberCount = count;
            break;
        }
    }
    if (!chosen.has_value()) {
        report_fail("runnable");
        return;
    }
    const std::uint32_t squadRow = *chosen;
    // One actor of the first member; the vector length must match the authored members.
    std::array<std::int32_t, squad_auth::kMaximumRequestedCountLength> requested{};
    requested[0] = 1;
    const std::span<const std::int32_t> counts(requested.data(), chosenMemberCount);
    constexpr auto mode = squad_auth::Mode::mode0;
    const auto status = squads::place(view, squadRow, counts, mode);
    if (status != squads::Status::queued && status != squads::Status::ready) {
        std::array<char, 96> line{};
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=photo stage=carrier result=fail reason=place status=%u",
                          static_cast<unsigned>(status));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
        }
        return;
    }
    std::array<char, 96> placed{};
    const int placedWritten = std::snprintf(
        placed.data(), placed.size(), "ev=photo stage=carrier result=placed row=%u", squadRow);
    if (placedWritten > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {placed.data(), static_cast<std::size_t>(placedWritten)});
        hooks::photo::trace("carrier", placed.data() + 22);
    }
    hooks::photo::begin_carrier_capture();
}

} // namespace sunrise::client::photo
