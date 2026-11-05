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
#include "../world_objects/dynamic_object_registry.h"
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
std::atomic<std::uint32_t> g_carrierHandle{0};

/**
 * Appends one photo event to `Sunrise\logs\photo.log` beside the in-memory log, so the trace
 * lands on disk even when the game's own log is not flushed. The file is UTF-8 and appended
 * per event; it is a debug aid and never truncated by the photo module itself.
 */
void trace(const char* stage, const char* text) noexcept {
    std::array<char, 320> line{};
    const int used =
        std::snprintf(line.data(), line.size(), "ev=photo stage=%s %s\n", stage, text);
    if (used <= 0) {
        return;
    }
    std::array<wchar_t, 512> wide{};
    const int wideCount = GetModuleFileNameW(nullptr, wide.data(), static_cast<DWORD>(wide.size()));
    if (wideCount > 0 && wideCount < static_cast<int>(wide.size())) {
        std::wstring_view path(wide.data(), static_cast<std::size_t>(wideCount));
        const std::size_t slash = path.find_last_of(L'\\');
        if (slash != std::wstring_view::npos) {
            std::array<wchar_t, 512> logPath{};
            const int built = std::swprintf(logPath.data(),
                                            logPath.size(),
                                            L"%.*s\\bin\\x64\\Sunrise\\logs\\photo.log",
                                            static_cast<int>(slash),
                                            wide.data());
            if (built > 0) {
                std::array<char, 512> narrow{};
                const int converted = WideCharToMultiByte(CP_UTF8,
                                                          0,
                                                          logPath.data(),
                                                          built,
                                                          narrow.data(),
                                                          static_cast<int>(narrow.size()),
                                                          nullptr,
                                                          nullptr);
                if (converted > 0) {
                    FILE* file = nullptr;
                    if (fopen_s(&file, narrow.data(), "a") == 0 && file != nullptr) {
                        std::fwrite(line.data(), 1, static_cast<std::size_t>(used), file);
                        std::fclose(file);
                    }
                }
            }
        }
    }
}

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
/** Identity-slot state; owned by the camera thread. */
bool g_identityPatched{false};
std::uint32_t g_identityOriginal{kInvalidHandle};
/** Carrier-capture state: the dynamic handles present before the spawn, and the wait. */
std::atomic_bool g_carrierCapturePending{false};
std::size_t g_carrierCaptureFrames{0};
std::array<std::uint32_t, hooks::world_objects::kDynamicHandleCapacity> g_carrierBeforeHandles{};
/** Whether the photo switch was on last frame; edges report the collected model tags. */
bool g_reportWasOn{false};

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

/** Writes the carrier handle into the identity slot and restores it on exit. */
void apply_identity() noexcept {
    const std::uint32_t carrier = g_carrierHandle.load(std::memory_order_relaxed);
    if (carrier == 0 || carrier == kInvalidHandle) {
        return;
    }
    std::uintptr_t slot = 0;
    if (!teleport::identity_slot_address(slot)) {
        return;
    }
    if (!g_identityPatched) {
        g_identityOriginal = *reinterpret_cast<volatile std::uint32_t*>(slot);
        g_identityPatched = true;
        std::array<char, 96> line{};
        const int written = std::snprintf(line.data(),
                                          line.size(),
                                          "ev=photo stage=identity result=applied handle=0x%X",
                                          static_cast<unsigned>(carrier));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(written)});
            // After "ev=photo stage=identity ".
            trace("identity", line.data() + 23);
        }
    }
    // Re-written every frame: the game may refresh the slot itself.
    *reinterpret_cast<volatile std::uint32_t*>(slot) = carrier;
}

/** Restores the original identity when the detached camera turns off. */
void restore_identity() noexcept {
    if (!g_identityPatched) {
        return;
    }
    std::uintptr_t slot = 0;
    if (teleport::identity_slot_address(slot)) {
        *reinterpret_cast<volatile std::uint32_t*>(slot) = g_identityOriginal;
    }
    g_identityPatched = false;
    trace("identity", "result=restored");
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
        restore_identity();
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
    // The identity redirect is dormant: the game camera is bound to the real player character,
    // not to the identity handle, so redirecting it froze the view while the input drove the
    // carrier. The carrier machinery stays for a future camera-anchor redirect.
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
    } else {
        g_playerHandle.store(kInvalidHandle, std::memory_order_release);
        g_playerParent.store(kInvalidHandle, std::memory_order_relaxed);
    }
    // Report the collected model tags once per photo switch-on, for the effects patcher.
    const bool reportNow = settings.enabled && settings.hidePlayer && !g_reportWasOn;
    if (reportNow) {
        std::array<char, 320> line{};
        std::size_t used = 0;
        used += static_cast<std::size_t>(std::snprintf(line.data(), line.size(), "models="));
        for (std::atomic<std::uint32_t>& slot : g_playerModels) {
            const std::uint32_t model = slot.load(std::memory_order_relaxed);
            if (model == 0) {
                continue;
            }
            used += static_cast<std::size_t>(std::snprintf(line.data() + used,
                                                           line.size() - used,
                                                           " 0x%X",
                                                           static_cast<unsigned>(model)));
        }
        if (used > 7) {
            trace("models", line.data() + 7);
        }
    }
    g_reportWasOn = settings.enabled && settings.hidePlayer;
    // The carrier capture waits for the spawned actor to appear among the dynamic handles.
    if (g_carrierCapturePending.load(std::memory_order_relaxed)) {
        ++g_carrierCaptureFrames;
        if (g_carrierCaptureFrames > 1800) {
            g_carrierCapturePending.store(false, std::memory_order_relaxed);
            // Fallback: capture an existing dynamic object parented to the player, so the
            // carrier does not depend on a delivered squad spawn. The ghost or the weapon is
            // the usual candidate; both have bodies and hang off the player's chain.
            std::array<objects::DynamicInstance, objects::kDynamicHandleCapacity> dynamic{};
            const auto snapshot = objects::try_dynamic_snapshot(dynamic);
            std::uint32_t fallback = 0;
            std::uint32_t fallbackClass = 0;
            if (snapshot.status == objects::SnapshotStatus::ready) {
                const std::uint32_t player =
                    g_playerHandle.load(std::memory_order_acquire);
                for (std::size_t i = 0; i < snapshot.written; ++i) {
                    const std::uint32_t candidate = dynamic[i].identity.handle;
                    if (candidate == 0 || candidate == kInvalidHandle || candidate == player) {
                        continue;
                    }
                    // The chain walk, like the hide match: the weapon and the ghost reach the
                    // player within a few datum parents.
                    std::uint32_t cursor = candidate;
                    bool reaches = false;
                    for (std::size_t depth = 0; depth < 8; ++depth) {
                        const std::uint32_t parent = datum_parent(cursor);
                        if (parent == kInvalidHandle) {
                            break;
                        }
                        if (parent == player) {
                            reaches = true;
                            break;
                        }
                        cursor = parent;
                    }
                    if (reaches) {
                        fallback = candidate;
                        fallbackClass = dynamic[i].classTag;
                        break;
                    }
                }
            }
            std::array<char, 320> line{};
            int written = 0;
            if (fallback != 0) {
                g_carrierHandle.store(fallback, std::memory_order_release);
                written = std::snprintf(line.data(),
                                        line.size(),
                                        "result=fallback_captured handle=0x%X class=0x%X",
                                        static_cast<unsigned>(fallback),
                                        static_cast<unsigned>(fallbackClass));
            } else {
                // Report what the dynamic registry held, so the candidate set is visible.
                std::size_t used = 0;
                used += static_cast<std::size_t>(std::snprintf(
                    line.data() + used,
                    line.size() - used,
                    "result=fail reason=capture_timeout snap=%u written=%u",
                    static_cast<unsigned>(snapshot.status),
                    static_cast<unsigned>(snapshot.written)));
                for (std::size_t i = 0; i < 6 && i < snapshot.written && used + 24 < line.size();
                     ++i) {
                    used += static_cast<std::size_t>(std::snprintf(
                        line.data() + used,
                        line.size() - used,
                        " h=0x%X:c=0x%X",
                        static_cast<unsigned>(dynamic[i].identity.handle),
                        static_cast<unsigned>(dynamic[i].classTag)));
                }
                written = static_cast<int>(used);
            }
            if (written > 0) {
                core::log::write(core::log::Channel::client,
                                 core::log::Level::warn,
                                 {line.data(), static_cast<std::size_t>(written)});
                trace("carrier", line.data());
            }
        } else if (TryAcquireSRWLockShared(&objects::g_lock)) {
            for (std::size_t i = 0; i < objects::kDynamicHandleCapacity; ++i) {
                const std::uint32_t candidate = objects::g_dynamicHandles[i];
                if (candidate == 0 || candidate == kInvalidHandle) {
                    continue;
                }
                bool known = false;
                for (std::size_t b = 0; b < objects::kDynamicHandleCapacity
                                        && g_carrierBeforeHandles[b] != 0;
                     ++b) {
                    known = known || g_carrierBeforeHandles[b] == candidate;
                }
                if (!known) {
                    g_carrierHandle.store(candidate, std::memory_order_release);
                    g_carrierCapturePending.store(false, std::memory_order_relaxed);
                    std::array<char, 96> line{};
                    const int written = std::snprintf(line.data(),
                                                      line.size(),
                                                      "ev=photo stage=carrier result=captured handle=0x%X",
                                                      static_cast<unsigned>(candidate));
                    if (written > 0) {
                        core::log::write(core::log::Channel::client,
                                         core::log::Level::warn,
                                         {line.data(), static_cast<std::size_t>(written)});
                        // After "ev=photo stage=carrier ".
                        trace("carrier", line.data() + 22);
                    }
                    break;
                }
            }
            ReleaseSRWLockShared(&objects::g_lock);
        }
    }
}

/** Snapshots the dynamic handles and opens the carrier-capture wait. */
void begin_carrier_capture() noexcept {
    namespace objects = hooks::world_objects;
    g_carrierBeforeHandles = {};
    if (TryAcquireSRWLockShared(&objects::g_lock)) {
        for (std::size_t i = 0; i < objects::kDynamicHandleCapacity; ++i) {
            g_carrierBeforeHandles[i] = objects::g_dynamicHandles[i];
        }
        ReleaseSRWLockShared(&objects::g_lock);
    }
    g_carrierCaptureFrames = 0;
    g_carrierCapturePending.store(true, std::memory_order_release);
}

/** @return True while a captured carrier handle still names a live object. */
bool carrier_live() noexcept {
    namespace objects = hooks::world_objects;
    const std::uint32_t carrier = g_carrierHandle.load(std::memory_order_relaxed);
    if (carrier == 0 || carrier == kInvalidHandle) {
        return false;
    }
    objects::DatumIdentity identity{};
    return objects::read_datum_identity(carrier, identity)
           && identity.selfHandle == carrier;
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
    // Hand the identity back before the hooks leave, in case the game is not shutting down.
    restore_identity();
    // The game is shutting down, so the HUD state does not need handing back.
    g_hudWanted.store(false, std::memory_order_relaxed);
    g_hudApplied.store(false, std::memory_order_relaxed);
}

} // namespace sunrise::client::hooks::photo
