#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace sunrise::client::photo {

/** Default free-camera speed, in world units per second. */
inline constexpr float kDefaultSpeed = 10.0F;
/** Slowest offered free-camera speed, for fine framing. */
inline constexpr float kMinimumSpeed = 0.5F;
/** Fastest offered free-camera speed. Past this the player outruns what the map streams in. */
inline constexpr float kMaximumSpeed = 100.0F;
/** No key is bound until one is picked, so a fresh install cannot fire a photo feature. */
inline constexpr std::uint32_t kNoKey = 0;
/** Longest output folder accepted from the file, in UTF-8 bytes. */
inline constexpr std::size_t kPathCapacity = 256;

/** Runtime photo configuration. This module owns it; Core settings do not carry it. */
struct Settings {
    bool enabled{false};
    std::uint32_t toggleKey{kNoKey};
    /** The key that takes a photo; it only fires while the mode is on. */
    std::uint32_t shutterKey{kNoKey};
    /** World units per second while a direction is pressed. */
    float speed{kDefaultSpeed};
    /** Skips the local player's skinned draws while the mode is on. */
    bool hidePlayer{true};
    /** Detaches the camera from the player; the keys fly the view while the player stands. */
    bool detached{false};
    /** Output folder, UTF-8. Empty means the default `Sunrise\photos` folder. */
    std::array<char, kPathCapacity> outputPath{};
};

/**
 * Resolves the configuration file and loads it when one exists.
 * @param module Loaded DLL used to resolve the owned artifact directory.
 */
void initialize(void* module) noexcept;

/** Drops the runtime configuration and the resolved paths. */
void shutdown() noexcept;

/** @return One lock-consistent copy of the current configuration. */
[[nodiscard]] Settings get() noexcept;

/**
 * Publishes one configuration and writes it straight to disk.
 * @param settings Candidate configuration, refused when a field is out of range.
 * @return True when the value was published. A failed write is logged, not returned.
 */
bool publish(const Settings& settings) noexcept;

/**
 * @return The default photo folder, `<Sunrise>\photos`. Resolved once at startup.
 */
[[nodiscard]] const std::wstring& default_output_directory() noexcept;

} // namespace sunrise::client::photo
