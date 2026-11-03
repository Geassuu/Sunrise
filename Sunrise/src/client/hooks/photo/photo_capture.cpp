/**
 * The photo shutter: captures the swap chain's back buffer into a JPEG file. The capture runs
 * on the presentation thread, before the overlay draws, so the frame holds only the game image.
 * The back buffer is copied to a staging texture, converted to 8-bit RGB, encoded by
 * stb_image_write and written into the folder the Photo page configures.
 */

#include "photo.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../core/ui/notice/ui_notice_overlay.h"
#include "../../../../vendor/stb/stb_image_write.h"
#include "../../photo/photo_settings_store.h"
#include "internal.h"

namespace sunrise::client::hooks::photo {
namespace {

/** JPEG quality the encoder writes. 95 keeps the file small and the picture clean. */
constexpr int kJpegQuality = 95;
/** One JPEG file takes this many bytes per 8-bit RGB pixel at most; the header is counted in. */
constexpr std::size_t kPixelBytes = 3;

/**
 * Converts one half float to a float. Bit math only, so no FPU mode assumptions.
 * @param value Half-float bits.
 * @return The decoded float.
 */
[[nodiscard]] float half_to_float(std::uint16_t value) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000U) << 16;
    std::uint32_t exponent = (value >> 10) & 0x1FU;
    std::uint32_t mantissa = value & 0x3FFU;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: normalize the mantissa and keep the exponent in range.
            int shift = -14;
            while ((mantissa & 0x400U) == 0) {
                mantissa <<= 1;
                --shift;
            }
            mantissa &= 0x3FFU;
            exponent = static_cast<std::uint32_t>(shift + 127);
            bits = sign | (exponent << 23) | (mantissa << 13);
        }
    } else if (exponent == 31) {
        // Infinities and NaNs clamp at the conversion below.
        bits = sign | 0x7F800000U | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
    float output = 0.0F;
    static_assert(sizeof(output) == sizeof(bits));
    std::memcpy(&output, &bits, sizeof(output));
    return output;
}

/** @param component Float color component. @return Its clamped 8-bit value. */
[[nodiscard]] std::uint8_t to_byte(float component) noexcept {
    if (component <= 0.0F) {
        return 0;
    }
    if (component >= 1.0F) {
        return 255;
    }
    return static_cast<std::uint8_t>(component * 255.0F + 0.5F);
}

/**
 * Copies one pixel into the RGB buffer.
 * @param output Receives three bytes.
 * @param red Red component.
 * @param green Green component.
 * @param blue Blue component.
 */
void store_pixel(std::uint8_t* output,
                 std::uint8_t red,
                 std::uint8_t green,
                 std::uint8_t blue) noexcept {
    output[0] = red;
    output[1] = green;
    output[2] = blue;
}

/**
 * Converts one mapped row into RGB. The format picks the lane order and bit depth.
 * @param format Back-buffer format.
 * @param width Row width in pixels.
 * @param row Source bytes; the caller already stepped past the row pitch.
 * @param output Receives width * 3 bytes.
 * @return True when the format is one the converter reads.
 */
[[nodiscard]] bool convert_row(DXGI_FORMAT format,
                               std::size_t width,
                               const std::uint8_t* row,
                               std::uint8_t* output) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM: {
        for (std::size_t x = 0; x < width; ++x) {
            store_pixel(output + x * kPixelBytes, row[0], row[1], row[2]);
            row += 4;
        }
        return true;
    }
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM: {
        for (std::size_t x = 0; x < width; ++x) {
            store_pixel(output + x * kPixelBytes, row[2], row[1], row[0]);
            row += 4;
        }
        return true;
    }
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint32_t pixel = *reinterpret_cast<const std::uint32_t*>(row);
            const auto convert = [](std::uint32_t lane) noexcept {
                return static_cast<std::uint8_t>((lane * 255U + 511U) / 1023U);
            };
            store_pixel(output + x * kPixelBytes,
                        convert(pixel & 0x3FFU),
                        convert((pixel >> 10) & 0x3FFU),
                        convert((pixel >> 20) & 0x3FFU));
            row += 4;
        }
        return true;
    }
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint16_t* lanes = reinterpret_cast<const std::uint16_t*>(row);
            store_pixel(output + x * kPixelBytes,
                        to_byte(half_to_float(lanes[0])),
                        to_byte(half_to_float(lanes[1])),
                        to_byte(half_to_float(lanes[2])));
            row += 8;
        }
        return true;
    }
    default:
        return false;
    }
}

/**
 * Converts one UTF-8 path into a wide string.
 * @param text Null-terminated UTF-8 path.
 * @param output Receives the converted path.
 * @return True when every byte converted.
 */
[[nodiscard]] bool widen(const char* text, std::wstring& output) noexcept {
    if (text[0] == '\0') {
        output.clear();
        return true;
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 0) {
        return false;
    }
    std::vector<wchar_t> buffer(static_cast<std::size_t>(length));
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, buffer.data(), length) != length) {
        return false;
    }
    output.assign(buffer.data());
    return true;
}

/**
 * Creates one directory and every directory on its path. Already-existing components are fine.
 * @param directory Directory to create, without a trailing separator.
 * @return True when the directory exists afterwards.
 */
[[nodiscard]] bool ensure_directory(const std::wstring& directory) noexcept {
    std::wstring extended = core::path::extended(directory.c_str());
    if (extended.empty()) {
        return false;
    }
    // The extended prefix ends at index 4; the drive root sits right after it. Every later
    // separator closes one component to try.
    std::size_t cursor = 4;
    if (extended.size() > 6 && extended[5] == L':') {
        cursor = 6;
    }
    while (cursor < extended.size()) {
        const std::size_t separator = extended.find(L'\\', cursor + 1);
        if (separator == std::wstring::npos) {
            break;
        }
        cursor = separator;
        std::wstring prefix = extended.substr(0, cursor);
        if (CreateDirectoryW(prefix.c_str(), nullptr) == FALSE
            && GetLastError() != ERROR_ALREADY_EXISTS) {
            // A file on the way makes the walk fail, which the caller reports.
            return false;
        }
    }
    const bool exists =
        CreateDirectoryW(extended.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
    return exists;
}

/** @param reason Key naming the step that failed. */
void report_fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written =
        std::snprintf(line.data(), line.size(), "ev=photo stage=capture result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    // The shutter press deserves an answer even when the file could not be written.
    core::ui::notice::raise("Photo save failed");
}

/** @param settings Current photo configuration. @return The folder photos are written into. */
[[nodiscard]] std::wstring output_directory(const client::photo::Settings& settings) noexcept {
    std::wstring directory;
    if (settings.outputPath[0] != '\0' && widen(settings.outputPath.data(), directory)) {
        return directory;
    }
    if (settings.outputPath[0] != '\0') {
        return L"";
    }
    return client::photo::default_output_directory();
}

/** Builds the next file name from the local clock: sunrise_photo_YYYYMMDD_HHMMSS_mmm.jpg. */
[[nodiscard]] std::wstring next_file_name() noexcept {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    std::array<wchar_t, 64> name{};
    const int written = _snwprintf_s(name.data(),
                                     name.size(),
                                     _TRUNCATE,
                                     L"sunrise_photo_%04u%02u%02u_%02u%02u%02u_%03u.jpg",
                                     static_cast<unsigned>(time.wYear),
                                     static_cast<unsigned>(time.wMonth),
                                     static_cast<unsigned>(time.wDay),
                                     static_cast<unsigned>(time.wHour),
                                     static_cast<unsigned>(time.wMinute),
                                     static_cast<unsigned>(time.wSecond),
                                     static_cast<unsigned>(time.wMilliseconds));
    return written > 0 ? std::wstring(name.data()) : std::wstring();
}

} // namespace

/** Captures the back buffer into a JPEG in the configured folder. */
void try_capture(IDXGISwapChain* swapChain,
                 ID3D11Device* device,
                 ID3D11DeviceContext* context) noexcept {
    if (!g_capturePending.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    if (swapChain == nullptr || device == nullptr || context == nullptr) {
        report_fail("renderer");
        return;
    }
    ID3D11Texture2D* backBuffer = nullptr;
    const HRESULT bufferResult = swapChain->GetBuffer(
        0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (FAILED(bufferResult) || backBuffer == nullptr) {
        report_fail("back_buffer");
        return;
    }
    D3D11_TEXTURE2D_DESC description{};
    backBuffer->GetDesc(&description);
    if (description.Width == 0 || description.Height == 0 || description.SampleDesc.Count > 1) {
        backBuffer->Release();
        report_fail("surface");
        return;
    }
    D3D11_TEXTURE2D_DESC stagingDescription{};
    stagingDescription.Width = description.Width;
    stagingDescription.Height = description.Height;
    stagingDescription.MipLevels = 1;
    stagingDescription.ArraySize = 1;
    stagingDescription.Format = description.Format;
    stagingDescription.SampleDesc.Count = 1;
    stagingDescription.Usage = D3D11_USAGE_STAGING;
    stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&stagingDescription, nullptr, &staging)) || staging == nullptr) {
        backBuffer->Release();
        report_fail("staging");
        return;
    }
    context->CopyResource(staging, backBuffer);
    backBuffer->Release();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        staging->Release();
        report_fail("map");
        return;
    }
    const std::size_t width = description.Width;
    const std::size_t height = description.Height;
    std::vector<std::uint8_t> rgb(width * height * kPixelBytes);
    bool converted = true;
    const std::uint8_t* row = static_cast<const std::uint8_t*>(mapped.pData);
    for (std::size_t y = 0; y < height && converted; ++y) {
        converted = convert_row(
            description.Format, width, row, rgb.data() + y * width * kPixelBytes);
        row += mapped.RowPitch;
    }
    context->Unmap(staging, 0);
    staging->Release();
    if (!converted) {
        report_fail("format");
        return;
    }

    const client::photo::Settings settings = client::photo::get();
    const std::wstring directory = output_directory(settings);
    if (directory.empty() || !ensure_directory(directory)) {
        report_fail("directory");
        return;
    }
    const std::wstring name = next_file_name();
    if (name.empty()) {
        report_fail("name");
        return;
    }
    const std::wstring full = directory + L"\\" + name;
    const std::wstring extended = core::path::extended(full.c_str());
    if (extended.empty()) {
        report_fail("path");
        return;
    }
    FILE* file = _wfopen(extended.c_str(), L"wb");
    if (file == nullptr) {
        report_fail("open");
        return;
    }
    const int written = stbi_write_jpg_to_func(
        [](void* context, void* data, int size) noexcept {
            (void)std::fwrite(data, 1, static_cast<std::size_t>(size), static_cast<FILE*>(context));
        },
        file,
        static_cast<int>(width),
        static_cast<int>(height),
        static_cast<int>(kPixelBytes),
        rgb.data(),
        kJpegQuality);
    const bool closed = std::fclose(file) == 0;
    if (written == 0 || !closed) {
        report_fail("write");
        return;
    }

    std::array<char, 128> notice{};
    const int noticeLength = std::snprintf(
        notice.data(), notice.size(), "Photo saved: %ls", name.c_str());
    if (noticeLength > 0) {
        core::ui::notice::raise(
            {notice.data(), static_cast<std::size_t>(noticeLength)});
    }
    const int fullLength =
        WideCharToMultiByte(CP_UTF8, 0, full.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (fullLength > 0) {
        std::vector<char> utf8(static_cast<std::size_t>(fullLength));
        if (WideCharToMultiByte(
                CP_UTF8, 0, full.c_str(), -1, utf8.data(), fullLength, nullptr, nullptr)
            > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             {utf8.data(), utf8.size() - 1});
        }
    }
}

} // namespace sunrise::client::hooks::photo
