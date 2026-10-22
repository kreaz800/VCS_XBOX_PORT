#include "display_window.hpp"

#include <hal/video.h>
#include <usbh_lib.h>
#include <xid_driver.h>
#include <usb/libusbohci/inc/hub.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <span>
#include <vector>

namespace vcs {

namespace {

constexpr std::uint32_t kXboxWidth  = 640u;
constexpr std::uint32_t kXboxHeight = 480u;

bool g_started = false;
bool g_aspect_lock = false;

constexpr std::uint32_t kPspSelect   = 0x000001u;
constexpr std::uint32_t kPspStart    = 0x000008u;
constexpr std::uint32_t kPspUp       = 0x000010u;
constexpr std::uint32_t kPspRight    = 0x000020u;
constexpr std::uint32_t kPspDown     = 0x000040u;
constexpr std::uint32_t kPspLeft     = 0x000080u;
constexpr std::uint32_t kPspLTrigger = 0x000100u;
constexpr std::uint32_t kPspRTrigger = 0x000200u;
constexpr std::uint32_t kPspTriangle = 0x001000u;
constexpr std::uint32_t kPspCircle   = 0x002000u;
constexpr std::uint32_t kPspCross    = 0x004000u;
constexpr std::uint32_t kPspSquare   = 0x008000u;

constexpr std::size_t kPadPacketSize = 32u;
constexpr std::uint8_t kButtonDeadzone = 0x20u;
constexpr std::int32_t kStickDeadzone = 4096;

bool g_usb_initialized = false;
xid_dev_t *g_pad = nullptr;
std::uint8_t g_pad_raw[kPadPacketSize]{};

void xbox_pad_read_callback(UTR_T *utr)
{
    if (utr == nullptr)
        return;

    xid_dev_t *device =
        reinterpret_cast<xid_dev_t *>(utr->context);

    if (utr->status < 0 || device == nullptr || device != g_pad)
        return;

    std::size_t length = static_cast<std::size_t>(utr->xfer_len);
    if (length > kPadPacketSize)
        length = kPadPacketSize;

    if (length != 0u)
        std::memcpy(g_pad_raw, utr->buff, length);

    // Keep the Xbox interrupt endpoint continuously reading.
    utr->xfer_len = 0;
    utr->bIsTransferDone = 0;
    usbh_int_xfer(utr);
}

xid_dev_t *find_first_gamepad()
{
    xid_dev_t *device = usbh_xid_get_device_list();

    while (device != nullptr) {
        if (device->xid_desc.bType == XID_TYPE_GAMECONTROLLER)
            return device;

        device = device->next;
    }

    return nullptr;
}

void xbox_pad_pump()
{
    if (!g_usb_initialized) {
        usbh_core_init();
        usbh_xid_init();
        g_usb_initialized = true;
    }

    // Drives Xbox USB enumeration/hotplug.
    usbh_pooling_hubs();

    xid_dev_t *device = find_first_gamepad();

    if (device == g_pad)
        return;

    g_pad = device;
    std::memset(g_pad_raw, 0, sizeof(g_pad_raw));

    if (g_pad != nullptr)
        usbh_xid_read(g_pad, 0, xbox_pad_read_callback);
}

std::int16_t load_s16(const std::uint8_t *p)
{
    std::int16_t value{};
    std::memcpy(&value, p, sizeof(value));
    return value;
}

std::uint8_t xbox_stick_to_psp(std::int16_t value, bool invert)
{
    std::int32_t axis = value;

    if (axis > -kStickDeadzone && axis < kStickDeadzone)
        axis = 0;

    std::int32_t converted =
        invert ? (128 - axis / 256)
               : (128 + axis / 256);

    converted = std::clamp<std::int32_t>(converted, 0, 255);
    return static_cast<std::uint8_t>(converted);
}

int xbox_stick_to_camera(std::int16_t value, bool invert)
{
    std::int32_t axis = value;

    if (axis > -kStickDeadzone && axis < kStickDeadzone)
        axis = 0;

    std::int32_t converted = axis / 256;
    if (invert)
        converted = -converted;

    return static_cast<int>(
        std::clamp<std::int32_t>(converted, -127, 127));
}

void ensure_started()
{
    if (g_started)
        return;

    XVideoSetMode(
        static_cast<int>(kXboxWidth),
        static_cast<int>(kXboxHeight),
        32,
        REFRESH_DEFAULT);

    volatile std::uint32_t *fb =
        reinterpret_cast<volatile std::uint32_t *>(XVideoGetFB());

    if (fb != nullptr) {
        for (std::uint32_t i = 0; i < kXboxWidth * kXboxHeight; ++i)
            fb[i] = 0u;

        XVideoFlushFB();
    }

    g_started = true;
}

struct ScaledRect {
    std::uint32_t x{};
    std::uint32_t y{};
    std::uint32_t width{};
    std::uint32_t height{};
};

ScaledRect fit_to_screen(std::uint32_t source_width,
                         std::uint32_t source_height)
{
    if (source_width == 0u || source_height == 0u)
        return {};

    std::uint32_t width = kXboxWidth;
    std::uint32_t height =
        static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(source_height) * kXboxWidth) /
            source_width);

    if (height > kXboxHeight) {
        height = kXboxHeight;
        width =
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(source_width) * kXboxHeight) /
                source_height);
    }

    width = std::max(1u, width);
    height = std::max(1u, height);

    return {
        (kXboxWidth - width) / 2u,
        (kXboxHeight - height) / 2u,
        width,
        height
    };
}

template <typename PixelReader>
void present_scaled(std::uint32_t source_width,
                    std::uint32_t source_height,
                    PixelReader &&read_pixel)
{
    ensure_started();

    volatile std::uint32_t *fb =
        reinterpret_cast<volatile std::uint32_t *>(XVideoGetFB());

    if (fb == nullptr || source_width == 0u || source_height == 0u)
        return;

    const ScaledRect destination =
        fit_to_screen(source_width, source_height);

    if (destination.width == 0u || destination.height == 0u)
        return;

    // Clear the output first so unused areas become black letter/pillar bars.
    for (std::uint32_t i = 0; i < kXboxWidth * kXboxHeight; ++i)
        fb[i] = 0u;

    // Only 640 entries maximum, avoiding an expensive divide for every pixel.
    std::array<std::uint32_t, kXboxWidth> source_x{};

    for (std::uint32_t x = 0u; x < destination.width; ++x) {
        source_x[x] =
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * source_width) /
                destination.width);

        if (source_x[x] >= source_width)
            source_x[x] = source_width - 1u;
    }

    for (std::uint32_t y = 0u; y < destination.height; ++y) {
        std::uint32_t sy =
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(y) * source_height) /
                destination.height);

        if (sy >= source_height)
            sy = source_height - 1u;

        volatile std::uint32_t *row =
            fb + (destination.y + y) * kXboxWidth + destination.x;

        for (std::uint32_t x = 0u; x < destination.width; ++x)
            row[x] = read_pixel(source_x[x], sy);
    }

    XVideoFlushFB();
}

} // namespace


bool display_window_enabled()
{
    return true;
}


void display_window_start()
{
    ensure_started();
    xbox_pad_pump();
}


void display_window_set_status(const char *)
{
    // No desktop title bar on Xbox.
}


void display_window_set_aspect_lock(bool locked) noexcept
{
    g_aspect_lock = locked;
    (void)g_aspect_lock;
}


void display_window_present(const psprecomp::GuestMemory &memory,
                            const FramebufferDescription &description)
{
    if (description.address == 0u ||
        description.width == 0u ||
        description.height == 0u ||
        description.stride == 0u) {
        return;
    }

    try {
        const std::vector<std::uint8_t> rgb =
            decode_framebuffer_rgb(memory, description);

        const std::size_t required =
            static_cast<std::size_t>(description.width) *
            description.height * 3u;

        if (rgb.size() < required)
            return;

        present_scaled(
            description.width,
            description.height,
            [&rgb, &description](std::uint32_t x,
                                std::uint32_t y) -> std::uint32_t {
                const std::size_t offset =
                    (static_cast<std::size_t>(y) * description.width + x) * 3u;

                const std::uint32_t red   = rgb[offset + 0u];
                const std::uint32_t green = rgb[offset + 1u];
                const std::uint32_t blue  = rgb[offset + 2u];

                return (red << 16u) | (green << 8u) | blue;
            });

    } catch (const std::exception &) {
        // Invalid/transient guest framebuffer: retain the previous frame.
    }
}


void display_window_present_rgba(std::span<const std::byte> rgba,
                                 std::uint32_t width,
                                 std::uint32_t height)
{
    if (width == 0u || height == 0u)
        return;

    const std::size_t required =
        static_cast<std::size_t>(width) * height * 4u;

    if (rgba.size() < required)
        return;

    present_scaled(
        width,
        height,
        [rgba, width](std::uint32_t x,
                      std::uint32_t y) -> std::uint32_t {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + x) * 4u;

            const std::uint32_t red =
                std::to_integer<std::uint8_t>(rgba[offset + 0u]);
            const std::uint32_t green =
                std::to_integer<std::uint8_t>(rgba[offset + 1u]);
            const std::uint32_t blue =
                std::to_integer<std::uint8_t>(rgba[offset + 2u]);

            return (red << 16u) | (green << 8u) | blue;
        });
}


DisplayWindowSurface display_window_surface()
{
    // There is no desktop/native HWND-style surface on Xbox.
    return {nullptr, nullptr, kXboxWidth, kXboxHeight};
}


HostInputState display_window_input()
{
    xbox_pad_pump();

    HostInputState input{};
    input.analog_x = 128u;
    input.analog_y = 128u;

    if (g_pad == nullptr)
        return input;

    std::uint8_t raw[kPadPacketSize]{};
    std::memcpy(raw, g_pad_raw, sizeof(raw));

    std::uint16_t digital{};
    std::memcpy(&digital, raw + 2u, sizeof(digital));

    // Digital controls.
    if (digital & (1u << 0u)) input.buttons |= kPspUp;
    if (digital & (1u << 1u)) input.buttons |= kPspDown;
    if (digital & (1u << 2u)) input.buttons |= kPspLeft;
    if (digital & (1u << 3u)) input.buttons |= kPspRight;
    if (digital & (1u << 4u)) input.buttons |= kPspStart;
    if (digital & (1u << 5u)) input.buttons |= kPspSelect;

    // Original Xbox face buttons are pressure-sensitive.
    if (raw[4] > kButtonDeadzone) input.buttons |= kPspCross;     // A
    if (raw[5] > kButtonDeadzone) input.buttons |= kPspCircle;    // B
    if (raw[6] > kButtonDeadzone) input.buttons |= kPspSquare;    // X
    if (raw[7] > kButtonDeadzone) input.buttons |= kPspTriangle;  // Y

    // Black / White act as the PSP shoulder buttons.
    if (raw[8] > kButtonDeadzone) input.buttons |= kPspRTrigger;  // Black
    if (raw[9] > kButtonDeadzone) input.buttons |= kPspLTrigger;  // White

    // Xbox triggers provide native vehicle throttle/brake.
    input.brake      = raw[10] > kButtonDeadzone;
    input.accelerate = raw[11] > kButtonDeadzone;

    const std::int16_t lx = load_s16(raw + 12u);
    const std::int16_t ly = load_s16(raw + 14u);
    const std::int16_t rx = load_s16(raw + 16u);
    const std::int16_t ry = load_s16(raw + 18u);

    input.analog_x = xbox_stick_to_psp(lx, false);
    input.analog_y = xbox_stick_to_psp(ly, true);

    input.camera_x = xbox_stick_to_camera(rx, false);
    input.camera_y = xbox_stick_to_camera(ry, true);

    return input;
}


std::uint32_t display_window_buttons()
{
    return display_window_input().buttons;
}


void display_window_analog(std::uint8_t &x, std::uint8_t &y)
{
    const HostInputState input = display_window_input();
    x = input.analog_x;
    y = input.analog_y;
}


bool display_window_close_requested()
{
    return false;
}


void display_window_shutdown()
{
    // Keep the Xbox video mode alive for the process lifetime.
}

} // namespace vcs
