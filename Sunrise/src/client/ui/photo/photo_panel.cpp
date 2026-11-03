/**
 * The photo module's interface. Every control saves to disk at once, so a change made here
 * survives the next launch with no settings edit. The folder field commits on Enter or when it
 * loses focus, like the other pages' number fields.
 */

#include "photo_panel.h"

#include <Windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <imgui.h>

#include "../../../core/settings/settings.h"
#include "../../../core/ui/components/label/ui_label_component.h"
#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../../core/ui/notice/ui_notice_overlay.h"
#include "../../photo/photo_settings_store.h"
#include "../../hooks/photo/photo.h"

namespace sunrise::client::ui::photo {
namespace {

namespace label = core::ui::components::label;

/** Lowest and highest virtual keys the picker scans. Zero is not a key. */
constexpr int kFirstVirtualKey = 1;
constexpr int kLastVirtualKey = 254;
/** Mouse buttons are skipped so a click on the picker cannot bind itself. */
constexpr int kLastMouseKey = 6;
/** Longest key name Windows returns, plus the null. */
constexpr std::size_t kKeyNameCapacity = 64;
/** One UTF-8 folder plus the "Saves to: " prefix and the null. */
constexpr std::size_t kCaptionCapacity = client::photo::kPathCapacity + 16;

enum class CaptureTarget {
    none,
    toggle,
    shutter,
};

CaptureTarget g_capturing{CaptureTarget::none};
/** The folder field's text, loaded from the store once the page first draws. */
std::array<char, client::photo::kPathCapacity> g_folder{};
bool g_folderLoaded{false};

/**
 * Names one virtual key for display.
 * @param virtualKey Key to name, or zero for no binding.
 * @param output Receives the name.
 */
void key_name(std::uint32_t virtualKey, std::array<char, kKeyNameCapacity>& output) noexcept {
    if (virtualKey == client::photo::kNoKey) {
        (void)std::snprintf(output.data(), output.size(), "None");
        return;
    }
    const UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    std::array<wchar_t, kKeyNameCapacity> wide{};
    const int written = scanCode != 0 ? GetKeyNameTextW(static_cast<LONG>(scanCode << 16),
                                                         wide.data(),
                                                         static_cast<int>(wide.size()))
                                       : 0;
    if (written <= 0
        || WideCharToMultiByte(CP_UTF8,
                               0,
                               wide.data(),
                               written,
                               output.data(),
                               static_cast<int>(output.size() - 1),
                               nullptr,
                               nullptr)
               <= 0) {
        (void)std::snprintf(
            output.data(), output.size(), "Key 0x%02X", static_cast<unsigned>(virtualKey));
    }
}

/**
 * Takes the first key held while the picker is armed.
 * @param picked Receives the key, or zero when Escape clears the binding.
 * @return True when this frame ended the capture.
 */
[[nodiscard]] bool capture_key(std::uint32_t& picked) noexcept {
    if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
        picked = client::photo::kNoKey;
        return true;
    }
    for (int key = kFirstVirtualKey; key <= kLastVirtualKey; ++key) {
        if (key <= kLastMouseKey) {
            continue;
        }
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            picked = static_cast<std::uint32_t>(key);
            return true;
        }
    }
    return false;
}

/**
 * Draws one key picker while keeping capture ownership exclusive.
 * @param id ImGui identity for the button.
 * @param target Binding this picker captures.
 * @param virtualKey Binding value to display and update.
 * @param width Button width.
 * @return True when a new binding was captured.
 */
[[nodiscard]] bool
key_picker(const char* id, CaptureTarget target, std::uint32_t& virtualKey, float width) noexcept {
    ImGui::PushID(id);
    if (g_capturing == target) {
        if (ImGui::Button("...", ImVec2(width, 0.0F))) {
            g_capturing = CaptureTarget::none;
        }
        ImGui::PopID();
        std::uint32_t picked = client::photo::kNoKey;
        if (capture_key(picked)) {
            virtualKey = picked;
            g_capturing = CaptureTarget::none;
            return true;
        }
        return false;
    }
    std::array<char, kKeyNameCapacity> name{};
    key_name(virtualKey, name);
    const bool clicked = ImGui::Button(name.data(), ImVec2(width, 0.0F));
    ImGui::PopID();
    if (clicked) {
        g_capturing = target;
    }
    return false;
}

/** Publishes the folder field's current text, or restores the stored value when invalid. */
void commit_folder() noexcept {
    client::photo::Settings settings = client::photo::get();
    // The field holds UTF-8; a plain copy is the whole parse, and the store validates the rest.
    std::array<char, client::photo::kPathCapacity> path{};
    (void)std::snprintf(path.data(), path.size(), "%s", g_folder.data());
    settings.outputPath = path;
    if (!client::photo::publish(settings)) {
        core::ui::notice::raise("Photo folder was not saved");
        std::memcpy(g_folder.data(), client::photo::get().outputPath.data(), g_folder.size());
    }
}

} // namespace

/** Draws the photo module inside the active Core UI frame. */
void draw() noexcept {
    if (!core::settings::get().client.cheats) {
        ImGui::TextDisabled("Locked (cheats off)");
        ImGui::SetItemTooltip("Set client.cheats to true in settings.json and restart. "
                              "Progress is not saved while cheats are on.");
        return;
    }
    if (!g_folderLoaded) {
        std::memcpy(
            g_folder.data(), client::photo::get().outputPath.data(), g_folder.size());
        g_folderLoaded = true;
    }
    client::photo::Settings settings = client::photo::get();
    bool changed = false;

    ImGui::TextUnformatted("Free camera");
    ImGui::Separator();
    ImGui::TextWrapped("A free camera for photography. Move with your movement keys; the player "
                       "character is hidden while the mode is on.");
    ImGui::Spacing();

    changed = core::ui::components::toggle::control("Enabled##photo", settings.enabled) || changed;

    ImGui::Spacing();
    // One label column and one control column, so the slider and key buttons share both edges.
    const float labelWidth =
        label::inset() + ImGui::CalcTextSize("Toggle key").x + ImGui::GetStyle().ItemSpacing.x * 2;
    const float controlWidth = ImGui::GetContentRegionAvail().x - labelWidth;

    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Toggle key");
    ImGui::SameLine(labelWidth);
    changed =
        key_picker("photo_toggle_key", CaptureTarget::toggle, settings.toggleKey, controlWidth)
        || changed;

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Speed");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    float speed = settings.speed;
    if (ImGui::SliderFloat("##photo_speed",
                           &speed,
                           client::photo::kMinimumSpeed,
                           client::photo::kMaximumSpeed,
                           "%.1f units/s")) {
        settings.speed = speed;
        changed = true;
    }

    ImGui::Spacing();
    changed =
        core::ui::components::toggle::control("Hide player##photo", settings.hidePlayer) || changed;

    ImGui::Spacing();
    changed =
        core::ui::components::toggle::control("Detached camera##photo", settings.detached) || changed;
    if (settings.detached) {
        ImGui::TextDisabled("The camera flies free; the player stands still.");
    }

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Photos");
    ImGui::Separator();
    ImGui::TextWrapped("Take a photo with the key below, or with the button while this page is "
                       "open. Photos save as JPEG into the folder below.");
    ImGui::Spacing();

    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Photo key");
    ImGui::SameLine(labelWidth);
    changed =
        key_picker("photo_shutter_key", CaptureTarget::shutter, settings.shutterKey, controlWidth)
        || changed;

    ImGui::Spacing();
    ImGui::AlignTextToFramePadding();
    label::align();
    ImGui::TextUnformatted("Folder");
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(controlWidth);
    const bool folderChanged = ImGui::InputText("##photo_folder",
                                                g_folder.data(),
                                                g_folder.size(),
                                                ImGuiInputTextFlags_EnterReturnsTrue)
                               || ImGui::IsItemDeactivatedAfterEdit();
    if (folderChanged) {
        commit_folder();
        settings = client::photo::get();
    }

    ImGui::Spacing();
    std::array<char, kCaptionCapacity> caption{};
    std::string_view folder = settings.outputPath[0] != '\0' ? std::string_view(settings.outputPath.data())
                                                             : std::string_view("<Sunrise>\\photos");
    (void)std::snprintf(caption.data(), caption.size(), "Saves to: %.*s",
                        static_cast<int>(folder.size()), folder.data());
    ImGui::TextUnformatted(caption.data());

    ImGui::Spacing();
    if (ImGui::Button("Take photo")) {
        hooks::photo::request_capture();
    }
    ImGui::SameLine();
    if (ImGui::Button("Default folder")) {
        std::memcpy(g_folder.data(), client::photo::Settings{}.outputPath.data(), g_folder.size());
        commit_folder();
        settings = client::photo::get();
    }
    ImGui::SameLine();
    if (ImGui::Button("Open folder")) {
        const std::string_view path = settings.outputPath[0] != '\0'
                                          ? std::string_view(settings.outputPath.data())
                                          : std::string_view();
        hooks::photo::request_open_folder(path);
    }

    if (changed && !client::photo::publish(settings)) {
        ImGui::Spacing();
        ImGui::TextUnformatted("value out of range, not saved");
    }
}

} // namespace sunrise::client::ui::photo
