/**
 * Photo mode: a free camera that moves the local player. The movement machinery mirrors the fly
 * hook: the keys set the player's velocity before the simulation step, and the noclip step hook
 * carries the position collision-free. On top of that the module holds the per-frame state the
 * hide-player detour and the shutter share: the player's object handle and one pending capture.
 */

#include "photo.h"

#include <Windows.h>

#include <Shellapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../../core/ui/runtime/ui_visibility_runtime.h"
#include "../../../state/account/account_state.h"
#include "../../../state/runtime/runtime.h"
#include "../../input/window_focus.h"
#include "../../photo/photo_settings_store.h"
#include "../../photo/photo_hud.h"
#include "../noclip/runtime.h"
#include "../teleport/runtime.h"
#include "../world_objects/internal.h"
#include "internal.h"

namespace sunrise::client::hooks::photo {

/** Shared runtime state; defined once here, read across the three photo translation units. */
std::atomic<std::uint32_t> g_playerHandle{kInvalidHandle};
std::atomic<std::uint32_t> g_playerParent{kInvalidHandle};
std::atomic_bool g_hideInstalled{false};
std::atomic_bool g_capturePending{false};
std::atomic<std::uintptr_t> g_poolBaseSlot{0};
std::atomic<std::uintptr_t> g_poolStrideSlot{0};

namespace {

namespace bindings = state::account::settings::bindings;

/** The high bit of a polled key state marks it held. */
constexpr SHORT kKeyHeldBit = static_cast<SHORT>(0x8000);
/** Below this squared length a direction counts as none. */
constexpr float kMinimumLengthSquared = 0.000001F;
/** The horizontal lanes. The basis is X forward, Z up. */
constexpr std::size_t kLaneX = 0;
constexpr std::size_t kLaneY = 1;

/** One movement direction in the player's own frame. */
enum class Direction : std::size_t {
    forward,
    backward,
    left,
    right,
    up,
    down,
    count,
};

/** Sizes the per-direction flag array. */
constexpr std::size_t kDirectionCount = static_cast<std::size_t>(Direction::count);

/** One polled action and the direction it moves. */
struct ActionDirection {
    bindings::Action action;
    Direction direction;
};

/** Crouch has two actions and either one descends. Both are read as a hold, to pair with jump. */
constexpr std::array<ActionDirection, 7> kActions{{
    {bindings::Action::moveForward, Direction::forward},
    {bindings::Action::moveBackward, Direction::backward},
    {bindings::Action::moveLeft, Direction::left},
    {bindings::Action::moveRight, Direction::right},
    {bindings::Action::jump, Direction::up},
    {bindings::Action::toggleCrouch, Direction::down},
    {bindings::Action::holdCrouch, Direction::down},
}};

/** Both binding halves of each polled action, in the order of the table above. */
std::array<bindings::Binding, kActions.size()> g_bindings{};
/** Set once the bindings are read, so the costly account snapshot runs once. */
bool g_bindingsRead{false};
/** The toggle key on the previous frame, so the switch only flips on the press. */
std::atomic_bool g_toggleDown{false};
/** The shutter key on the previous frame, so one press takes one photo. */
std::atomic_bool g_shutterDown{false};
/** One folder queued by the menu for the shell to open; empty opens the default folder. */
std::array<char, client::photo::kPathCapacity> g_openFolder{};
std::atomic_bool g_openPending{false};
/** Cached per frame on the camera thread, so the per-draw hide test takes no store lock. */
std::atomic_bool g_hideActive{false};
/** Detached-camera state; written by the camera hook, read by the physics hooks. */
std::atomic_bool g_detached{false};
bool g_freecamInitialized{false};
teleport::Vector g_freecamPosition{};
std::uint64_t g_lastDriveTick{0};
/** The staged pose the renderer thread writes before the frame draws. */
SRWLOCK g_freecamPublishLock{SRWLOCK_INIT};
teleport::Vector g_freecamPublishPosition{};
std::uint32_t g_freecamPublishPlayerIndex{kInvalidHandle};
bool g_freecamPublishValid{false};
/** The HUD state the camera thread wants; the renderer thread applies it. */
std::atomic_bool g_hudWanted{false};
/** The HUD state already applied, so the push happens once per switch. */
std::atomic_bool g_hudApplied{false};

/**
 * Takes the account's movement bindings once they are loaded.
 * @return True when they have been read.
 */
[[nodiscard]] bool read_bindings() noexcept {
    if (g_bindingsRead) {
        return true;
    }
    // The snapshot copies the whole account, so it stops once the bindings arrive.
    const state::AccountState account = state::account_snapshot();
    if (!account.settings.keyBindings.configured) {
        return false;
    }
    for (std::size_t index = 0; index < kActions.size(); ++index) {
        const std::size_t action = static_cast<std::size_t>(kActions[index].action);
        g_bindings[index] = account.settings.keyBindings.values[action];
    }
    g_bindingsRead = true;
    return true;
}

/**
 * A half is an input code, not a virtual key. A half bound to a mouse button gives no key.
 * @param half One binding half, empty when unbound.
 * @return True while its key is down.
 */
[[nodiscard]] bool half_down(const std::optional<std::uint16_t>& half) noexcept {
    if (!half.has_value()) {
        return false;
    }
    const std::uint32_t key = teleport::action_key(*half);
    return key != 0 && (GetAsyncKeyState(static_cast<int>(key)) & kKeyHeldBit) != 0;
}

/** @return One flag per direction, true while any key bound to it is down. */
[[nodiscard]] std::array<bool, kDirectionCount> pressed_directions() noexcept {
    std::array<bool, kDirectionCount> pressed{};
    for (std::size_t index = 0; index < kActions.size(); ++index) {
        const bindings::Binding& binding = g_bindings[index];
        if (half_down(binding.primary) || half_down(binding.secondary)) {
            pressed[static_cast<std::size_t>(kActions[index].direction)] = true;
        }
    }
    return pressed;
}

/**
 * Forward turned about the up axis. This turn is the player's right, confirmed in flight.
 * @return The strafe axis, or zeroes when the camera looks straight up or down.
 */
[[nodiscard]] teleport::Vector right_of(const teleport::Vector& forward) noexcept {
    teleport::Vector right{forward[kLaneY], -forward[kLaneX], 0.0F};
    const float lengthSquared = right[kLaneX] * right[kLaneX] + right[kLaneY] * right[kLaneY];
    if (lengthSquared <= kMinimumLengthSquared) {
        return teleport::Vector{};
    }
    const float length = std::sqrt(lengthSquared);
    right[kLaneX] /= length;
    right[kLaneY] /= length;
    return right;
}

/**
 * Composes the pressed directions into one unit vector.
 * @param pressed One flag per direction.
 * @param forward Camera forward vector.
 * @return The direction to fly, or all zeroes when nothing is pressed.
 */
[[nodiscard]] teleport::Vector travel(const std::array<bool, kDirectionCount>& pressed,
                                      const teleport::Vector& forward) noexcept {
    const teleport::Vector right = right_of(forward);
    teleport::Vector move{};
    const auto add = [&move](const teleport::Vector& axis, float scale) noexcept {
        for (std::size_t lane = 0; lane < teleport::kVectorLanes; ++lane) {
            move[lane] += axis[lane] * scale;
        }
    };
    if (pressed[static_cast<std::size_t>(Direction::forward)]) {
        add(forward, 1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::backward)]) {
        add(forward, -1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::right)]) {
        add(right, 1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::left)]) {
        add(right, -1.0F);
    }
    if (pressed[static_cast<std::size_t>(Direction::up)]) {
        move[teleport::kVerticalLane] += 1.0F;
    }
    if (pressed[static_cast<std::size_t>(Direction::down)]) {
        move[teleport::kVerticalLane] -= 1.0F;
    }
    float lengthSquared = 0.0F;
    for (const float lane : move) {
        lengthSquared += lane * lane;
    }
    if (lengthSquared <= kMinimumLengthSquared) {
        return teleport::Vector{};
    }
    // Normalised, so a diagonal is not faster than a straight line.
    const float length = std::sqrt(lengthSquared);
    for (float& lane : move) {
        lane /= length;
    }
    return move;
}

/**
 * Shortens a velocity to a speed limit, keeping its direction.
 * @param velocity Velocity to limit in place.
 * @param limit Highest speed to leave.
 */
void cap_speed(teleport::Vector& velocity, float limit) noexcept {
    float speedSquared = 0.0F;
    for (const float lane : velocity) {
        speedSquared += lane * lane;
    }
    if (speedSquared <= limit * limit) {
        return;
    }
    const float scale = limit / std::sqrt(speedSquared);
    for (float& lane : velocity) {
        lane *= scale;
    }
}

/**
 * Works out the velocity the keys ask for.
 * @param speed Configured free-camera speed.
 * @return Velocity in world units per second.
 */
[[nodiscard]] teleport::Vector desired_velocity(float speed) noexcept {
    // The interface or another application owns the keyboard. Their presses must not steer.
    std::array<bool, kDirectionCount> pressed{};
    if (!core::ui::runtime::snapshot().visible && input::game_focused()) {
        pressed = pressed_directions();
    }
    teleport::Vector forward{};
    const teleport::Vector move =
        teleport::camera_forward(forward) ? travel(pressed, forward) : teleport::Vector{};
    teleport::Vector velocity{};
    for (std::size_t lane = 0; lane < teleport::kVectorLanes; ++lane) {
        velocity[lane] = move[lane] * speed;
    }
    return velocity;
}

} // namespace

/** Reads the toggle key once a frame and flips the switch on the press. */
void poll_toggle() noexcept {
    const client::photo::Settings settings = client::photo::get();
    if (settings.toggleKey == client::photo::kNoKey) {
        g_toggleDown.store(false, std::memory_order_relaxed);
        return;
    }
    const bool down =
        input::game_focused()
        && (GetAsyncKeyState(static_cast<int>(settings.toggleKey)) & kKeyHeldBit) != 0;
    // The interface owns the keyboard, so the key tracks the press but never flips the switch.
    if (core::ui::runtime::snapshot().visible) {
        g_toggleDown.store(down, std::memory_order_relaxed);
        return;
    }
    if (down && !g_toggleDown.exchange(true, std::memory_order_acq_rel)) {
        client::photo::Settings updated = settings;
        updated.enabled = !settings.enabled;
        if (!client::photo::publish(updated)) {
            return;
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         updated.enabled ? "ev=photo stage=toggle enabled=1"
                                         : "ev=photo stage=toggle enabled=0");
        return;
    }
    if (!down) {
        g_toggleDown.store(false, std::memory_order_release);
    }
}

/** Reads the shutter key once a frame and latches one capture on the press. */
void poll_shutter() noexcept {
    const client::photo::Settings settings = client::photo::get();
    if (!settings.enabled || settings.shutterKey == client::photo::kNoKey) {
        g_shutterDown.store(false, std::memory_order_relaxed);
        return;
    }
    const bool down =
        input::game_focused()
        && (GetAsyncKeyState(static_cast<int>(settings.shutterKey)) & kKeyHeldBit) != 0;
    // The interface owns the keyboard, so the key tracks the press but never fires. The menu
    // has its own take-photo button for that.
    if (core::ui::runtime::snapshot().visible) {
        g_shutterDown.store(down, std::memory_order_relaxed);
        return;
    }
    if (down && !g_shutterDown.exchange(true, std::memory_order_acq_rel)) {
        request_capture();
        return;
    }
    if (!down) {
        g_shutterDown.store(false, std::memory_order_release);
    }
}

/** Latches one capture request directly; the menu's take-photo button uses this. */
void request_capture() noexcept {
    g_capturePending.store(true, std::memory_order_release);
}

/** Queues one folder-open request for the next camera-thread poll. */
void request_open_folder(std::string_view folder) noexcept {
    const std::size_t length = (std::min)(folder.size(), g_openFolder.size() - 1);
    std::memcpy(g_openFolder.data(), folder.data(), length);
    g_openFolder[length] = '\0';
    g_openPending.store(true, std::memory_order_release);
}

/** Runs one queued folder-open request through the Windows shell. */
void dispatch_open_folder() noexcept {
    if (!g_openPending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    std::wstring directory;
    if (g_openFolder[0] != '\0') {
        const int length =
            MultiByteToWideChar(CP_UTF8, 0, g_openFolder.data(), -1, nullptr, 0);
        if (length > 0) {
            directory.resize(static_cast<std::size_t>(length) - 1);
            if (MultiByteToWideChar(CP_UTF8,
                                    0,
                                    g_openFolder.data(),
                                    -1,
                                    directory.data(),
                                    length)
                != length) {
                directory.clear();
            }
        }
    }
    if (directory.empty()) {
        directory = client::photo::default_output_directory();
    }
    if (directory.empty()) {
        return;
    }
    // ShellExecuteW reserves values through 32 for failure details, as the credits badge notes.
    (void)ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

/** Writes the player's velocity on the physics sync, which publishes it. */
void apply(void* component) noexcept {
    const client::photo::Settings settings = client::photo::get();
    if (!settings.enabled || settings.detached) {
        return;
    }
    if (component == nullptr || !teleport::owns_local_player(component)) {
        return;
    }
    if (!read_bindings()) {
        return;
    }
    // Capped, because this is the field the game reads to decide the player hit something too
    // hard. The step has the real speed; this is only what the sync publishes.
    teleport::Vector velocity = desired_velocity(settings.speed);
    cap_speed(velocity, kPublishedSpeedCap);
    (void)teleport::write_velocity(component, velocity);
}

/** Reports whether the free camera is on. */
bool enabled() noexcept {
    return client::photo::get().enabled;
}

/** @return True while the detached camera owns the view instead of the player position. */
bool detached_active() noexcept {
    return g_detached.load(std::memory_order_relaxed);
}

/** Drives the detached camera from the camera-frame hook. */
void drive_camera(std::uint32_t playerIndex) noexcept {
    const client::photo::Settings settings = client::photo::get();
    const bool detached = settings.enabled && settings.detached;
    if (!detached) {
        g_detached.store(false, std::memory_order_relaxed);
        g_freecamInitialized = false;
        AcquireSRWLockExclusive(&g_freecamPublishLock);
        g_freecamPublishValid = false;
        ReleaseSRWLockExclusive(&g_freecamPublishLock);
        return;
    }
    g_detached.store(true, std::memory_order_relaxed);
    teleport::CameraPose pose{};
    if (!teleport::camera_pose(pose)) {
        return;
    }
    if (!g_freecamInitialized) {
        g_freecamPosition = pose.position;
        g_freecamInitialized = true;
    }
    const std::uint64_t now = GetTickCount64();
    float delta = 0.016F;
    if (g_lastDriveTick != 0) {
        delta = (std::min)(0.1F, static_cast<float>(now - g_lastDriveTick) / 1000.0F);
    }
    g_lastDriveTick = now;
    // The keys move the camera in the frame the game just computed; mouse look stays the
    // game's own, so the detached camera flies and turns while the player stands still.
    const teleport::Vector velocity = desired_velocity(settings.speed);
    for (std::size_t lane = 0; lane < teleport::kVectorLanes; ++lane) {
        g_freecamPosition[lane] += velocity[lane] * delta;
    }
    // Stage the position the physics pin carries the body to: the invisible body becomes the
    // empty character the game's own camera rides on.
    AcquireSRWLockExclusive(&g_freecamPublishLock);
    g_freecamPublishPosition = g_freecamPosition;
    g_freecamPublishPlayerIndex = playerIndex;
    g_freecamPublishValid = true;
    ReleaseSRWLockExclusive(&g_freecamPublishLock);
}

/** Reads the staged free-camera position the physics pin carries the body to. */
bool freecam_body_position(teleport::Vector& position) noexcept {
    AcquireSRWLockShared(&g_freecamPublishLock);
    const bool valid = g_freecamPublishValid;
    position = g_freecamPublishPosition;
    ReleaseSRWLockShared(&g_freecamPublishLock);
    return valid;
}

/** Sets the velocity the coming simulation step integrates. */
void before_step(void* body) noexcept {
    if (body == nullptr || !read_bindings()) {
        return;
    }
    const client::photo::Settings settings = client::photo::get();
    if (!settings.enabled || settings.detached) {
        return;
    }
    noclip::write_body_velocity(body, desired_velocity(settings.speed));
}

/** Refreshes the cached local-player object handle and the hide switch. */
void refresh_player_handle() noexcept {
    const client::photo::Settings settings = client::photo::get();
    g_hideActive.store(settings.enabled && settings.hidePlayer, std::memory_order_relaxed);
    // The object pool carries the parent links the hide-player detour walks. The slots are
    // published by the world-object hooks, so they are cached only while those hooks accept.
    namespace objects = hooks::world_objects;
    if (TryAcquireSRWLockShared(&objects::g_lock)) {
        if (objects::g_accepting.load(std::memory_order_acquire)) {
            g_poolBaseSlot.store(reinterpret_cast<std::uintptr_t>(objects::g_datumBaseStorage),
                                 std::memory_order_relaxed);
            g_poolStrideSlot.store(reinterpret_cast<std::uintptr_t>(objects::g_datumStrideStorage),
                                   std::memory_order_relaxed);
        } else {
            g_poolBaseSlot.store(0, std::memory_order_relaxed);
            g_poolStrideSlot.store(0, std::memory_order_relaxed);
        }
        ReleaseSRWLockShared(&objects::g_lock);
    }
    std::uint32_t handle = kInvalidHandle;
    if (teleport::controlled_handle(handle)) {
        g_playerHandle.store(handle, std::memory_order_release);
        g_playerParent.store(datum_parent(handle), std::memory_order_relaxed);
        update_part_flags();
        // The HUD policy follows the photo switch; the renderer thread pushes it each frame.
        g_hudWanted.store(settings.enabled, std::memory_order_relaxed);
        return;
    }
    g_playerHandle.store(kInvalidHandle, std::memory_order_release);
    g_playerParent.store(kInvalidHandle, std::memory_order_relaxed);
}

/** Applies a pending HUD state from the renderer thread. */
void sync_hud() noexcept {
    const bool wanted = g_hudWanted.load(std::memory_order_relaxed);
    if (g_hudApplied.load(std::memory_order_relaxed) == wanted) {
        return;
    }
    client::photo::set_hud_hidden(wanted);
    g_hudApplied.store(wanted, std::memory_order_relaxed);
}

/** @return True when the local player's skinned draws must be skipped. */
bool hide_player_active() noexcept {
    return g_hideInstalled.load(std::memory_order_relaxed) && g_hideActive.load(std::memory_order_relaxed);
}

/** @param owner Draw owner handle from one skinned draw record. */
bool owns_player_object(std::uint32_t owner) noexcept {
    const std::uint32_t player = g_playerHandle.load(std::memory_order_acquire);
    return player != kInvalidHandle && owner == player;
}

/** @return True when one capture request is waiting for the presentation thread. */
bool screenshot_pending() noexcept {
    return g_capturePending.load(std::memory_order_acquire);
}

/** Clears the key states and any waiting capture. The switches are stored and survive. */
void reset() noexcept {
    g_toggleDown.store(false, std::memory_order_release);
    g_shutterDown.store(false, std::memory_order_release);
    g_capturePending.store(false, std::memory_order_release);
    g_openPending.store(false, std::memory_order_release);
    g_hideActive.store(false, std::memory_order_relaxed);
    g_poolBaseSlot.store(0, std::memory_order_relaxed);
    g_poolStrideSlot.store(0, std::memory_order_relaxed);
    g_playerHandle.store(kInvalidHandle, std::memory_order_release);
    g_playerParent.store(kInvalidHandle, std::memory_order_relaxed);
    g_detached.store(false, std::memory_order_relaxed);
    g_freecamInitialized = false;
    g_lastDriveTick = 0;
    for (std::atomic<std::uint32_t>& slot : g_playerModels) {
        slot.store(0, std::memory_order_relaxed);
    }
    // The hide switch is now off, so this restores every patched part flag.
    update_part_flags();
    // The game is shutting down, so the HUD state does not need handing back.
    g_hudWanted.store(false, std::memory_order_relaxed);
    g_hudApplied.store(false, std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::photo
