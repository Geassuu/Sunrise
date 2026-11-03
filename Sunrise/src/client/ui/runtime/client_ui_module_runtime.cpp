#include "client_ui_module_runtime.h"

#include <string_view>

#include "../../../core/ui/modules/registry/ui_module_registry.h"
#include "../../../core/ui/modules/ui_module_descriptor.h"
#include "../mission_launch/mission_launch_panel.h"
#include "../movement/movement_panel.h"
#include "../photo/photo_panel.h"
#include "../player/player_panel.h"

namespace sunrise::client::ui::runtime {
namespace {

/** Namespaced stable IDs prevent Client modules from colliding with Server modules. */
constexpr std::string_view kMovementStableId = "client.movement";
constexpr std::string_view kPlayerStableId = "client.player";
constexpr std::string_view kPhotoStableId = "client.photo";
/** Short menu label for the shared teleport and noclip page. */
constexpr std::string_view kMovementDisplayName = "Movement";
/** Short menu label for the player page. */
constexpr std::string_view kPlayerDisplayName = "Player";
/** Short menu label for the photo page. */
constexpr std::string_view kPhotoDisplayName = "Photo Mode";

core::ui::modules::registry::PageRegistration g_movementPage;
core::ui::modules::registry::PageRegistration g_playerPage;
core::ui::modules::registry::PageRegistration g_photoPage;
core::ui::modules::registry::PageRegistration g_activityLauncherPage;

} // namespace

/** @return True when the Client modules own their Core UI registry slots. */
bool initialize() noexcept {
    // Registered after movement, which is the order the menu lists them in.
    const bool movementOwned = g_movementPage.acquire(
        core::ui::modules::Owner::client, kMovementStableId, kMovementDisplayName, &movement::draw);
    const bool playerOwned = g_playerPage.acquire(
        core::ui::modules::Owner::client, kPlayerStableId, kPlayerDisplayName, &player::draw);
    const bool photoOwned = g_photoPage.acquire(
        core::ui::modules::Owner::client, kPhotoStableId, kPhotoDisplayName, &photo::draw);
    const bool launcherOwned = g_activityLauncherPage.acquire(core::ui::modules::Owner::client,
                                                              "client.mission_launch",
                                                              "Activity Launcher",
                                                              &mission_launch::draw);
    return movementOwned && playerOwned && photoOwned && launcherOwned;
}

/** Removes the Client modules from the Core UI registry. */
void shutdown() noexcept {
    g_activityLauncherPage.release();
    g_photoPage.release();
    g_playerPage.release();
    g_movementPage.release();
}

} // namespace sunrise::client::ui::runtime
