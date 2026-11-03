/**
 * The photo configuration store. Like the movement store it is separate from Core settings:
 * the interface changes these values while the game runs and saves each change at once.
 * One extra field, the output folder, is a string, so the parser handles quoted scalars too.
 */

#include "photo_settings_store.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"

namespace sunrise::client::photo {
namespace {

/** The module-owned configuration file, beside the generated settings and logs. */
constexpr std::wstring_view kFileSuffix = L"\\photo.json";
/** The document is a few scalars and one path, so one small buffer covers both ways. */
constexpr std::size_t kFileCapacity = 1024;
/** Longest scalar accepted from the file. Anything longer is malformed rather than large. */
constexpr std::size_t kScalarCapacity = 32;
/** Highest Windows virtual-key code, so a stored binding cannot name a key that cannot exist. */
constexpr std::uint32_t kMaximumVirtualKey = 254;
/** Default photo subfolder under the Sunrise artifact directory. */
constexpr std::wstring_view kDefaultSuffix = L"\\photos";

SRWLOCK g_lock{SRWLOCK_INIT};
Settings g_settings{};
core::path::Buffer g_path{};
std::wstring g_defaultDirectory{};
bool g_pathResolved{};

/** @param settings Candidate configuration. @return True when every field is in range. */
[[nodiscard]] bool valid(const Settings& settings) noexcept {
    return settings.toggleKey <= kMaximumVirtualKey && settings.shutterKey <= kMaximumVirtualKey
           && settings.speed >= kMinimumSpeed && settings.speed <= kMaximumSpeed
           && settings.outputPath[settings.outputPath.size() - 1] == '\0';
}

/** @param reason Key naming the step that failed. */
void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=photo stage=store result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Finds one key's raw scalar text.
 * @param text Whole document.
 * @param key Quoted key to locate.
 * @param output Receives the text between the colon and the next separator.
 * @return True when the key exists and carries a non-empty value.
 */
[[nodiscard]] bool
scalar_for(std::string_view text, std::string_view key, std::string_view& output) noexcept {
    const std::size_t at = text.find(key);
    if (at == std::string_view::npos) {
        return false;
    }
    const std::size_t colon = text.find(':', at + key.size());
    if (colon == std::string_view::npos) {
        return false;
    }
    std::size_t begin = colon + 1;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = begin;
    while (end < text.size() && text[end] != ',' && text[end] != '}' && text[end] != '\n'
           && text[end] != '\r') {
        ++end;
    }
    output = text.substr(begin, end - begin);
    return !output.empty();
}

/**
 * Copies one scalar into null-terminated storage the C conversions require.
 * @param value Scalar text taken from the document.
 * @param output Receives the terminated copy.
 * @return True when the scalar fits.
 */
[[nodiscard]] bool terminated(std::string_view value,
                              std::array<char, kScalarCapacity>& output) noexcept {
    if (value.size() >= output.size()) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        output[index] = value[index];
    }
    output[value.size()] = '\0';
    return true;
}

/**
 * Decodes a JSON string into UTF-8. Only the escapes a Windows path needs are understood:
 * a backslash escape, a quote, and the letter escapes a normal JSON writer produces.
 * @param value Raw text including the surrounding quotes.
 * @param output Receives the decoded text, terminated.
 * @return True when the string is quoted, fits, and holds no unknown escape.
 */
[[nodiscard]] bool
decode_string(std::string_view value, std::array<char, kPathCapacity>& output) noexcept {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
        return false;
    }
    std::size_t at = 1;
    std::size_t written = 0;
    const std::size_t last = value.size() - 1;
    while (at < last && written + 1 < output.size()) {
        char next = value[at++];
        if (next != '\\') {
            output[written++] = next;
            continue;
        }
        if (at >= last) {
            return false;
        }
        switch (value[at++]) {
        case '"':
        case '\\':
        case '/':
            output[written++] = value[at - 1];
            break;
        case 'b':
            output[written++] = '\b';
            break;
        case 'f':
            output[written++] = '\f';
            break;
        case 'n':
            output[written++] = '\n';
            break;
        case 'r':
            output[written++] = '\r';
            break;
        case 't':
            output[written++] = '\t';
            break;
        default:
            return false;
        }
    }
    if (at != last) {
        return false;
    }
    output[written] = '\0';
    return true;
}

/**
 * Layers one document over the current defaults. A missing or malformed key keeps its default,
 * so a hand-edited file cannot stop the module loading.
 * @param text Whole document.
 * @param output Receives the parsed configuration.
 */
void parse(std::string_view text, Settings& output) noexcept {
    std::string_view scalar;
    if (scalar_for(text, "\"enabled\"", scalar)) {
        output.enabled = scalar.starts_with("true");
    }
    std::array<char, kScalarCapacity> buffer{};
    if (scalar_for(text, "\"toggle_key\"", scalar) && terminated(scalar, buffer)) {
        output.toggleKey = static_cast<std::uint32_t>(std::strtoul(buffer.data(), nullptr, 0));
    }
    if (scalar_for(text, "\"shutter_key\"", scalar) && terminated(scalar, buffer)) {
        output.shutterKey = static_cast<std::uint32_t>(std::strtoul(buffer.data(), nullptr, 0));
    }
    if (scalar_for(text, "\"speed\"", scalar) && terminated(scalar, buffer)) {
        // Clamped, not refused, so an old value past a new limit keeps the rest of the file.
        output.speed = std::clamp(std::strtof(buffer.data(), nullptr), kMinimumSpeed, kMaximumSpeed);
    }
    if (scalar_for(text, "\"hide_player\"", scalar)) {
        output.hidePlayer = scalar.starts_with("true");
    }
    if (scalar_for(text, "\"detached\"", scalar)) {
        output.detached = scalar.starts_with("true");
    }
    if (scalar_for(text, "\"output_path\"", scalar)) {
        // A malformed string keeps the default folder rather than refusing the whole file.
        std::array<char, kPathCapacity> path{};
        if (decode_string(scalar, path)) {
            output.outputPath = path;
        }
    }
}

/**
 * Escapes one UTF-8 path for the stored JSON string.
 * @param path Null-terminated path to escape.
 * @param output Receives the quoted and escaped text.
 * @return True when everything fit.
 */
[[nodiscard]] bool
encode_string(const char* path, std::array<char, kFileCapacity>& output) noexcept {
    std::size_t written = 0;
    output[written++] = '"';
    for (const char* at = path; *at != '\0' && written + 2 < output.size(); ++at) {
        const char next = *at;
        if (next == '\\' || next == '"') {
            output[written++] = '\\';
        }
        output[written++] = next;
    }
    output[written++] = '"';
    output[written] = '\0';
    return written < output.size();
}

/**
 * Writes the whole document. It is small enough that a complete rewrite is the simplest
 * correct save, which the shared settings file is not.
 * @param settings Configuration to store.
 * @return True when every byte reached the file.
 */
[[nodiscard]] bool store(const Settings& settings) noexcept {
    if (!g_pathResolved) {
        return false;
    }
    std::array<char, kFileCapacity> escaped{};
    if (!encode_string(settings.outputPath.data(), escaped)) {
        return false;
    }
    std::array<char, kFileCapacity> document{};
    const int size = std::snprintf(document.data(),
                                   document.size(),
                                   "{\n  \"enabled\": %s,\n  \"toggle_key\": %u,\n"
                                   "  \"shutter_key\": %u,\n  \"speed\": %.3f,\n"
                                   "  \"hide_player\": %s,\n  \"detached\": %s,\n"
                                   "  \"output_path\": %s\n}\n",
                                   settings.enabled ? "true" : "false",
                                   static_cast<unsigned>(settings.toggleKey),
                                   static_cast<unsigned>(settings.shutterKey),
                                   static_cast<double>(settings.speed),
                                   settings.hidePlayer ? "true" : "false",
                                   settings.detached ? "true" : "false",
                                   escaped.data());
    if (size <= 0) {
        return false;
    }
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    bool complete =
        WriteFile(file, document.data(), static_cast<DWORD>(size), &written, nullptr) != FALSE
        && written == static_cast<DWORD>(size);
    complete = CloseHandle(file) != FALSE && complete;
    return complete;
}

/** Reads the configuration file into the active settings when one exists. */
void load() noexcept {
    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    std::array<char, kFileCapacity> buffer{};
    DWORD read = 0;
    const bool readOk =
        ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size() - 1), &read, nullptr) != FALSE;
    (void)CloseHandle(file);
    if (!readOk || read == 0) {
        return;
    }
    Settings parsed{};
    parse(std::string_view(buffer.data(), read), parsed);
    if (!valid(parsed)) {
        report_fail("range");
        return;
    }
    g_settings = parsed;
}

} // namespace

/** Resolves the configuration file and the default folder, then loads the file when it exists. */
void initialize(void* module) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_settings = Settings{};
    g_defaultDirectory.clear();
    g_pathResolved =
        core::path::artifact_directory(module, g_path) && core::path::append(g_path, kFileSuffix);
    core::path::Buffer directory{};
    if (core::path::artifact_directory(module, directory)
        && core::path::append(directory, kDefaultSuffix)) {
        g_defaultDirectory.assign(directory.chars.data(), directory.length);
    } else {
        report_fail("directory");
    }
    if (g_pathResolved) {
        load();
    } else {
        report_fail("path");
    }
    ReleaseSRWLockExclusive(&g_lock);
}

/** Drops the runtime configuration and the resolved paths. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_settings = Settings{};
    g_defaultDirectory.clear();
    g_path = core::path::Buffer{};
    g_pathResolved = false;
    ReleaseSRWLockExclusive(&g_lock);
}

/** @return One lock-consistent copy; with cheats locked the photo tools read as off and unbound. */
Settings get() noexcept {
    AcquireSRWLockShared(&g_lock);
    Settings snapshot = g_settings;
    ReleaseSRWLockShared(&g_lock);
    // The free camera moves the player, so it is a cheat like the movement tools. Unbound keys
    // also stop the mode toggling itself on while it is locked.
    if (!core::settings::get().client.cheats) {
        snapshot.enabled = false;
        snapshot.toggleKey = kNoKey;
        snapshot.shutterKey = kNoKey;
    }
    return snapshot;
}

/** Publishes one configuration and writes it straight to disk. */
bool publish(const Settings& settings) noexcept {
    if (!valid(settings)) {
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_settings = settings;
    const bool stored = store(settings);
    ReleaseSRWLockExclusive(&g_lock);
    if (!stored) {
        report_fail("write");
    }
    return true;
}

/** @return The default photo folder, `<Sunrise>\photos`. */
const std::wstring& default_output_directory() noexcept {
    AcquireSRWLockShared(&g_lock);
    // The string is written once at startup and never again, so this reference stays valid.
    ReleaseSRWLockShared(&g_lock);
    return g_defaultDirectory;
}

} // namespace sunrise::client::photo
