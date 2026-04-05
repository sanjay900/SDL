/*
  Simple DirectMedia Layer
  Copyright (C) 2025 Mitchell Cairns <mitch.cairns@handheldlegend.com>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "SDL_internal.h"

#ifdef SDL_JOYSTICK_HIDAPI

#include "../../SDL_hints_c.h"
#include "../SDL_sysjoystick.h"

#include "SDL_hidapi_rumble.h"
#include "SDL_hidapi_santroller.h"
#include "SDL_hidapijoystick_c.h"
#include "SDL_report_descriptor.h"

#ifdef SDL_JOYSTICK_HIDAPI_SANTROLLER

// Define this if you want to log all packets from the controller
#if 0
#define DEBUG_SANTROLLER_PROTOCOL
#endif

#if 0
#define DEBUG_SANTROLLER_INIT
#endif

enum
{
    SDL_SANTROLLER_SUB_TYPE_GAMEPAD = 1,
    SDL_SANTROLLER_SUB_TYPE_DANCEPAD = 2,
    SDL_SANTROLLER_SUB_TYPE_GUITAR_HERO_GUITAR = 3,
    SDL_SANTROLLER_SUB_TYPE_ROCK_BAND_GUITAR = 4,
    SDL_SANTROLLER_SUB_TYPE_GUITAR_HERO_DRUMS = 5,
    SDL_SANTROLLER_SUB_TYPE_ROCK_BAND_DRUMS = 6,
    SDL_SANTROLLER_SUB_TYPE_LIVE_GUITAR = 7,
    SDL_SANTROLLER_SUB_TYPE_DJ_HERO_TURNTABLE = 8,
    SDL_SANTROLLER_SUB_TYPE_STAGE_KIT = 9,
    SDL_SANTROLLER_SUB_TYPE_DISNEY_INFINITY = 10,
    SDL_SANTROLLER_SUB_TYPE_SKYLANDERS = 11,
    SDL_SANTROLLER_SUB_TYPE_LEGO_DIMENSIONS = 12,
    SDL_SANTROLLER_SUB_TYPE_PROJECT_DIVA = 13,
    SDL_SANTROLLER_SUB_TYPE_GUITAR_FREAKS = 14,
    SDL_SANTROLLER_SUB_TYPE_WHEEL = 15,
    SDL_SANTROLLER_SUB_TYPE_PRO_KEYS = 16,
    SDL_SANTROLLER_SUB_TYPE_PRO_GUITAR_MUSTANG = 17,
    SDL_SANTROLLER_SUB_TYPE_PRO_GUITAR_SQUIRE = 18,
    SDL_SANTROLLER_SUB_TYPE_KEYBOARD_MOUSE = 19,
    SDL_SANTROLLER_SUB_TYPE_FIGHT_STICK = 20,
    SDL_SANTROLLER_SUB_TYPE_FLIGHT_STICK = 21,
    SDL_SANTROLLER_SUB_TYPE_POP_N_MUSIC = 22,
    SDL_SANTROLLER_SUB_TYPE_DJ_MAX = 23,
    SDL_SANTROLLER_SUB_TYPE_TAIKO = 24,
    SDL_SANTROLLER_SUB_TYPE_POWER_GIG_DRUM = 25,
    SDL_SANTROLLER_SUB_TYPE_POWER_GIG_GUITAR = 26,
    SDL_SANTROLLER_SUB_TYPE_ROCK_REVOLUTION_GUITAR = 27,
};

#define SANTROLLER_DEVICE_FEATURES_GET_REPORT_SIZE 3      // Size of features report
#define SANTROLLER_DEVICE_FEATURES_SET_REPORT_SIZE 2      // Size of features report
#define SANTROLLER_DEVICE_FEATURES_USAGE           0x2882 // Usage page for features report

#define SANTROLLER_DEVICE_REPORT_ID_JOYSTICK_INPUT  0x01
#define SANTROLLER_DEVICE_REPORT_ID_COMMAND_OUTPUT  0x01
#define SANTROLLER_DEVICE_REPORT_ID_FEATURES        0x10
#define SANTROLLER_DEVICE_REPORT_COMMAND_RUMBLE     0x5A
#define SANTROLLER_DEVICE_REPORT_COMMAND_PLAYER_LED 0x5C
#define SANTROLLER_DEVICE_REPORT_COMMAND_RGB_LED    0x5D

typedef struct
{
    SDL_HIDAPI_Device *device;
    Uint16 protocol_version;
    Uint16 usb_device_version;
    bool sensors_enabled;

    Uint8 player_idx;
    Uint8 sub_type;

    bool player_leds_supported;
    bool joystick_rgb_supported;
    bool instrument_led_supported;
    bool rumble_supported;
    bool dpad_as_buttons;
    bool new_format;
    int buttons_count;
    int axes_count;

    Uint8 last_state[USB_PACKET_LENGTH];
} SDL_DriverSantroller_Context;

static bool RetrieveSDLFeatures(SDL_HIDAPI_Device *device)
{
    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)device->context;
    int written = 0;

    // Originally, santroller devices only supported enumerating as a single device at a time
    // and encoded all features into the device version.

    // Santroller V2 added support for mulitple sumultaneous device, so since bcdDevice would
    // be the same across multiple devices, they moved features to a HID report instead.
    // Check for presence of that report to determine which device we are communicating with,
    // and if it's a V2 device, query features from the device instead of relying on version encoding.
    Uint8 descriptor[1024];
    int descriptor_len = SDL_hid_get_report_descriptor(device->dev, descriptor, sizeof(descriptor));
    SDL_ReportDescriptor *parsed_descriptor = SDL_ParseReportDescriptor(descriptor, descriptor_len);
    if (SDL_DescriptorHasUsage(parsed_descriptor, 0xFF00, SANTROLLER_DEVICE_FEATURES_USAGE)) {
        // New revesion device, request features from the device
        // Attempt to send the SDL features get command.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const Uint8 featuresGetCommand[SANTROLLER_DEVICE_FEATURES_SET_REPORT_SIZE] = { SANTROLLER_DEVICE_REPORT_ID_FEATURES, 0 };
            // This write will occasionally return -1, so ignore failure here and try again
            written = SDL_hid_write(device->dev, featuresGetCommand, sizeof(featuresGetCommand));
            if (written == SANTROLLER_DEVICE_FEATURES_SET_REPORT_SIZE) {
                break;
            }
        }

        if (written < 2) {
            SDL_SetError("Santroller device SDL Features GET command could not write");
            return false;
        }

        int read = 0;

        // Read the reply
        for (int i = 0; i < 100; ++i) {
            SDL_Delay(1);

            Uint8 data[SANTROLLER_DEVICE_FEATURES_GET_REPORT_SIZE];
            read = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0);
            if (read < 0) {
                SDL_SetError("Santroller device SDL Features GET command could not read");
                return false;
            }
            if (read == 0) {
                continue;
            }

#ifdef DEBUG_SANTROLLER_PROTOCOL
            HIDAPI_DumpPacket("Santroller packet: size = %d", data, read);
#endif

            if ((read == SANTROLLER_DEVICE_FEATURES_GET_REPORT_SIZE) && (data[0] == SANTROLLER_DEVICE_REPORT_ID_FEATURES)) {
                ctx->sub_type = data[1];
                ctx->player_leds_supported = (data[2] & 0x01) != 0;
                ctx->joystick_rgb_supported = (data[2] & 0x02) != 0;
                ctx->instrument_led_supported = (data[2] & 0x04) != 0;
                ctx->rumble_supported = (data[2] & 0x08) != 0;
                ctx->dpad_as_buttons = ctx->sub_type == SDL_SANTROLLER_SUB_TYPE_DANCEPAD; // Only dance pads has dpad as buttons
                ctx->new_format = true;
                ctx->buttons_count = ctx->dpad_as_buttons ? 16 : 12; // Dance pads have 4 additional buttons for the dpad directions
                ctx->axes_count = 6;
#if defined(DEBUG_SANTROLLER_INIT)
                SDL_Log("Received Santroller SDL Features response: %d %d", data[1], data[2]);
#endif
                return true;
            }
        }
    } else {
        // Older revision device, sub_type is encoded into device version
        ctx->sub_type = (device->version >> 8) & 0xFF;
        // Older devices don't expose capabilities, but will ignore anything
        // they don't support, so just assume everything is supported
        ctx->player_leds_supported = true;
        ctx->joystick_rgb_supported = true;
        ctx->instrument_led_supported = true;
        ctx->rumble_supported = ctx->sub_type == SDL_SANTROLLER_SUB_TYPE_GAMEPAD;
        ctx->dpad_as_buttons = false;
        ctx->new_format = false;
        ctx->buttons_count = 12; // Dance pads have 4 additional buttons for the dpad directions
        ctx->axes_count = 6;
        return true;
    }

    return false;
}

static void HIDAPI_DriverSantroller_RegisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_AddHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SANTROLLER, callback, userdata);
}

static void HIDAPI_DriverSantroller_UnregisterHints(SDL_HintCallback callback, void *userdata)
{
    SDL_RemoveHintCallback(SDL_HINT_JOYSTICK_HIDAPI_SANTROLLER, callback, userdata);
}

static bool HIDAPI_DriverSantroller_IsEnabled(void)
{
    return SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI_SANTROLLER, SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, SDL_HIDAPI_DEFAULT));
}

static bool HIDAPI_DriverSantroller_IsSupportedDevice(SDL_HIDAPI_Device *device, const char *name, SDL_GamepadType type, Uint16 vendor_id, Uint16 product_id, Uint16 version, int interface_number, int interface_class, int interface_subclass, int interface_protocol)
{
    return SDL_IsJoystickSantrollerController(vendor_id, product_id);
}

static bool HIDAPI_DriverSantroller_InitDevice(SDL_HIDAPI_Device *device)
{
#if defined(DEBUG_SANTROLLER_INIT)
    SDL_Log("Santroller device Init");
#endif

    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        return false;
    }

    ctx->device = device;
    device->context = ctx;

    if (!RetrieveSDLFeatures(device)) {
        return false;
    }

    switch (ctx->sub_type) {
    case SDL_SANTROLLER_SUB_TYPE_GAMEPAD:
        device->joystick_type = SDL_JOYSTICK_TYPE_GAMEPAD;
        break;
    case SDL_SANTROLLER_SUB_TYPE_DANCEPAD:
        device->joystick_type = SDL_JOYSTICK_TYPE_DANCE_PAD;
        break;
    case SDL_SANTROLLER_SUB_TYPE_POWER_GIG_GUITAR:
    case SDL_SANTROLLER_SUB_TYPE_ROCK_REVOLUTION_GUITAR:
    case SDL_SANTROLLER_SUB_TYPE_GUITAR_HERO_GUITAR:
    case SDL_SANTROLLER_SUB_TYPE_ROCK_BAND_GUITAR:
    case SDL_SANTROLLER_SUB_TYPE_LIVE_GUITAR:
    case SDL_SANTROLLER_SUB_TYPE_PRO_GUITAR_MUSTANG:
    case SDL_SANTROLLER_SUB_TYPE_PRO_GUITAR_SQUIRE:
        device->joystick_type = SDL_JOYSTICK_TYPE_GUITAR;
        break;
    case SDL_SANTROLLER_SUB_TYPE_TAIKO:
    case SDL_SANTROLLER_SUB_TYPE_GUITAR_HERO_DRUMS:
    case SDL_SANTROLLER_SUB_TYPE_ROCK_BAND_DRUMS:
    case SDL_SANTROLLER_SUB_TYPE_STAGE_KIT:
    case SDL_SANTROLLER_SUB_TYPE_POWER_GIG_DRUM:
        device->joystick_type = SDL_JOYSTICK_TYPE_DRUM_KIT;
        break;
    }

    return HIDAPI_JoystickConnected(device, NULL);
}

static int HIDAPI_DriverSantroller_GetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id)
{
    return -1;
}

static void HIDAPI_DriverSantroller_SetDevicePlayerIndex(SDL_HIDAPI_Device *device, SDL_JoystickID instance_id, int player_index)
{
    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)device->context;

    if (ctx->player_led_supported) {
        Uint8 playerReport[32] = { SANTROLLER_DEVICE_REPORT_ID_COMMAND_OUTPUT, SANTROLLER_DEVICE_REPORT_COMMAND_PLAYER_LED, (Uint8)(player_index) };

        SDL_hid_write(device->dev, playerReport, sizeof(playerReport));
    }
}

static bool HIDAPI_DriverSantroller_OpenJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
#if defined(DEBUG_SANTROLLER_INIT)
    SDL_Log("Santroller device Open");
#endif

    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)device->context;

    SDL_AssertJoysticksLocked();

    joystick->nbuttons = ctx->buttons_count;

    SDL_zeroa(ctx->last_state);

    joystick->naxes = ctx->axes_count;

    joystick->nhats = 1;

    return true;
}

static bool HIDAPI_DriverSantroller_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{

    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)device->context;

    if (ctx->rumble_supported) {
        Uint8 hapticReport[32] = { SANTROLLER_DEVICE_REPORT_ID_COMMAND_OUTPUT, SANTROLLER_DEVICE_REPORT_COMMAND_RUMBLE, (Uint8)(low_frequency_rumble >> 8), (Uint8)(high_frequency_rumble >> 8) };

        SDL_hid_write(device->dev, hapticReport, sizeof(hapticReport));
        return true;
    }

    return SDL_Unsupported();
}

static bool HIDAPI_DriverSantroller_RumbleJoystickTriggers(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 left_rumble, Uint16 right_rumble)
{
    return SDL_Unsupported();
}

static Uint32 HIDAPI_DriverSantroller_GetJoystickCapabilities(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)device->context;

    Uint32 caps = 0;
    if (ctx->rumble_supported) {
        caps |= SDL_JOYSTICK_CAP_RUMBLE;
    }

    if (ctx->player_leds_supported) {
        caps |= SDL_JOYSTICK_CAP_PLAYER_LED;
    }

    if (ctx->joystick_rgb_supported) {
        caps |= SDL_JOYSTICK_CAP_RGB_LED;
    }

    return caps;
}

static bool HIDAPI_DriverSantroller_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)device->context;

    if (ctx->joystick_rgb_supported) {

        Uint8 playerReport[32] = { SANTROLLER_DEVICE_REPORT_ID_COMMAND_OUTPUT, SANTROLLER_DEVICE_REPORT_COMMAND_RGB_LED, red, green, blue};

        SDL_hid_write(device->dev, playerReport, sizeof(playerReport));

        return true;
    }
    return SDL_Unsupported();
}

static bool HIDAPI_DriverSantroller_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverSantroller_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    return SDL_Unsupported();
}

static void HIDAPI_DriverSantroller_HandleStatePacketNew(SDL_Joystick *joystick, SDL_DriverSantroller_Context *ctx, Uint8 *data, int size)
{
    Sint16 axis;
    Uint64 timestamp = SDL_GetTicksNS();

    if (ctx->last_state[2] != data[2]) {
        Uint8 hat = 0;
        if (ctx->dpad_as_buttons) {
            if (data[2] & 0x01) {
                hat |= SDL_HAT_UP;
            }
            if (data[2] & 0x02) {
                hat |= SDL_HAT_DOWN;
            }
            if (data[2] & 0x04) {
                hat |= SDL_HAT_LEFT;
            }
            if (data[2] & 0x08) {
                hat |= SDL_HAT_RIGHT;
            }
        } else {
            switch (data[2] & 0x0f) {
            case 0:
                hat = SDL_HAT_UP;
                break;
            case 1:
                hat = SDL_HAT_RIGHTUP;
                break;
            case 2:
                hat = SDL_HAT_RIGHT;
                break;
            case 3:
                hat = SDL_HAT_RIGHTDOWN;
                break;
            case 4:
                hat = SDL_HAT_DOWN;
                break;
            case 5:
                hat = SDL_HAT_LEFTDOWN;
                break;
            case 6:
                hat = SDL_HAT_LEFT;
                break;
            case 7:
                hat = SDL_HAT_LEFTUP;
                break;
            default:
                hat = SDL_HAT_CENTERED;
                break;
            }
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);

        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data[2] & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data[2] & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data[2] & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data[2] & 0x80) != 0));
    }

    if (ctx->last_state[3] != data[3]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[3] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[3] & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[3] & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[3] & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[3] & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[3] & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[3] & 0x80) != 0));
    }

    axis = ((int)data[4] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);
    axis = ((int)data[5] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[6]));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[8]));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[10]));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis);
    axis = SDL_Swap16LE(*(Sint16 *)(&data[12]));
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, axis);

    SDL_memcpy(ctx->last_state, data, SDL_min((size_t)size, sizeof(ctx->last_state)));
}

static void HIDAPI_DriverSantroller_HandleStatePacketOldGamepad(SDL_Joystick *joystick, SDL_DriverSantroller_Context *ctx, Uint8 *data, int size)
{
    Sint16 axis;
    Uint64 timestamp = SDL_GetTicksNS();

    if (ctx->last_state[3] != data[3]) {
        Uint8 hat = 0;
        switch (data[3] & 0x0f) {
        case 0:
            hat = SDL_HAT_UP;
            break;
        case 1:
            hat = SDL_HAT_RIGHTUP;
            break;
        case 2:
            hat = SDL_HAT_RIGHT;
            break;
        case 3:
            hat = SDL_HAT_RIGHTDOWN;
            break;
        case 4:
            hat = SDL_HAT_DOWN;
            break;
        case 5:
            hat = SDL_HAT_LEFTDOWN;
            break;
        case 6:
            hat = SDL_HAT_LEFT;
            break;
        case 7:
            hat = SDL_HAT_LEFTUP;
            break;
        default:
            hat = SDL_HAT_CENTERED;
            break;
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);
    }
    if (ctx->last_state[1] != data[1]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[1] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[1] & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[1] & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[1] & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[1] & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[1] & 0x20) != 0));
    }

    if (ctx->last_state[2] != data[2]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data[2] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data[2] & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_STICK, ((data[2] & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_STICK, ((data[2] & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[2] & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_MISC1, ((data[2] & 0x20) != 0));
    }

    axis = ((int)data[4] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, axis);
    axis = ((int)data[5] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTY, axis);
    axis = ((int)data[6] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis);
    axis = ((int)data[7] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, axis);
    axis = ((int)data[8] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, axis);
    axis = ((int)data[9] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, axis);

    SDL_memcpy(ctx->last_state, data, SDL_min((size_t)size, sizeof(ctx->last_state)));
}

static void HIDAPI_DriverSantroller_HandleStatePacketOldGuitarHeroGuitar(SDL_Joystick *joystick, SDL_DriverSantroller_Context *ctx, Uint8 *data, int size)
{
    Sint16 axis;
    Uint64 timestamp = SDL_GetTicksNS();

    if (ctx->last_state[3] != data[3]) {
        Uint8 hat = 0;
        switch (data[3] & 0x0f) {
        case 0:
            hat = SDL_HAT_UP;
            break;
        case 1:
            hat = SDL_HAT_RIGHTUP;
            break;
        case 2:
            hat = SDL_HAT_RIGHT;
            break;
        case 3:
            hat = SDL_HAT_RIGHTDOWN;
            break;
        case 4:
            hat = SDL_HAT_DOWN;
            break;
        case 5:
            hat = SDL_HAT_LEFTDOWN;
            break;
        case 6:
            hat = SDL_HAT_LEFT;
            break;
        case 7:
            hat = SDL_HAT_LEFTUP;
            break;
        default:
            hat = SDL_HAT_CENTERED;
            break;
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);
    }
    if (ctx->last_state[1] != data[1]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[1] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[1] & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[1] & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[1] & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[1] & 0x10) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ((data[1] & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data[1] & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data[1] & 0x80) != 0));
    }

    if (ctx->last_state[2] != data[2]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[2] & 0x01) != 0));
    }

    axis = ((int)data[4] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis);
    axis = ((int)data[5] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFTX, axis);
    axis = ((int)data[6] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, axis);
    axis = ((int)data[7] * 257) - 32768;

    SDL_memcpy(ctx->last_state, data, SDL_min((size_t)size, sizeof(ctx->last_state)));
}

static void HIDAPI_DriverSantroller_HandleStatePacketOldRockBandGuitar(SDL_Joystick *joystick, SDL_DriverSantroller_Context *ctx, Uint8 *data, int size)
{
    Sint16 axis;
    Uint64 timestamp = SDL_GetTicksNS();

    if (ctx->last_state[3] != data[3]) {
        Uint8 hat = 0;
        switch (data[3] & 0x0f) {
        case 0:
            hat = SDL_HAT_UP;
            break;
        case 1:
            hat = SDL_HAT_RIGHTUP;
            break;
        case 2:
            hat = SDL_HAT_RIGHT;
            break;
        case 3:
            hat = SDL_HAT_RIGHTDOWN;
            break;
        case 4:
            hat = SDL_HAT_DOWN;
            break;
        case 5:
            hat = SDL_HAT_LEFTDOWN;
            break;
        case 6:
            hat = SDL_HAT_LEFT;
            break;
        case 7:
            hat = SDL_HAT_LEFTUP;
            break;
        default:
            hat = SDL_HAT_CENTERED;
            break;
        }
        SDL_SendJoystickHat(timestamp, joystick, 0, hat);
    }
    if (ctx->last_state[1] != data[1] || ctx->last_state[2] != data[2]) {
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_SOUTH, ((data[1] & 0x01) != 0) || ((data[1] & 0x20) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_EAST, ((data[1] & 0x02) != 0) || ((data[1] & 0x40) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_NORTH, ((data[1] & 0x04) != 0) || ((data[1] & 0x80) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_WEST, ((data[1] & 0x08) != 0) || ((data[2] & 0x01) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ((data[1] & 0x10) != 0) || ((data[2] & 0x02) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_BACK, ((data[2] & 0x04) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_START, ((data[2] & 0x08) != 0));
        SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_GUIDE, ((data[2] & 0x10) != 0));
    }

    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, (((data[1] & 0x20) != 0) || ((data[1] & 0x40) != 0) || ((data[1] & 0x80) != 0) || ((data[2] & 0x01) != 0) || ((data[2] & 0x02) != 0)) ? 255 : 0);
    axis = ((int)data[4] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTX, axis);
    axis = ((int)data[5] * 257) - 32768;
    SDL_SendJoystickAxis(timestamp, joystick, SDL_GAMEPAD_AXIS_RIGHTY, axis);

    // Align with ps3 mappings
    SDL_SendJoystickButton(timestamp, joystick, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, data[6] > 0xD0);

    SDL_memcpy(ctx->last_state, data, SDL_min((size_t)size, sizeof(ctx->last_state)));
}

static bool HIDAPI_DriverSantroller_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)device->context;
    SDL_Joystick *joystick = NULL;
    Uint8 data[USB_PACKET_LENGTH];
    int size = 0;

    if (device->num_joysticks > 0) {
        joystick = SDL_GetJoystickFromID(device->joysticks[0]);
    } else {
        return false;
    }

    while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
#ifdef DEBUG_SANTROLLER_PROTOCOL
        HIDAPI_DumpPacket("Santroller packet: size = %d", data, size);
#endif
        if (!joystick) {
            continue;
        }
        if (data[0] != SANTROLLER_DEVICE_REPORT_ID_JOYSTICK_INPUT) {
            continue;
        }
        if (ctx->new_format) {
            HIDAPI_DriverSantroller_HandleStatePacketNew(joystick, ctx, data, size);
        } else {
            switch (ctx->sub_type) {
            case SDL_SANTROLLER_SUB_TYPE_GAMEPAD:
                HIDAPI_DriverSantroller_HandleStatePacketOldGamepad(joystick, ctx, data, size);
                break;
            case SDL_SANTROLLER_SUB_TYPE_GUITAR_HERO_GUITAR:
                HIDAPI_DriverSantroller_HandleStatePacketOldGuitarHeroGuitar(joystick, ctx, data, size);
                break;
            case SDL_SANTROLLER_SUB_TYPE_ROCK_BAND_GUITAR:
                HIDAPI_DriverSantroller_HandleStatePacketOldRockBandGuitar(joystick, ctx, data, size);
                break;
            }
        }
    }

    if (size < 0) {
        // Read error, device is disconnected
        HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
    }
    return (size >= 0);
}

static void HIDAPI_DriverSantroller_CloseJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick)
{
}

static void HIDAPI_DriverSantroller_FreeDevice(SDL_HIDAPI_Device *device)
{
}

SDL_HIDAPI_DeviceDriver SDL_HIDAPI_DriverSantroller = {
    SDL_HINT_JOYSTICK_HIDAPI_SANTROLLER,
    true,
    HIDAPI_DriverSantroller_RegisterHints,
    HIDAPI_DriverSantroller_UnregisterHints,
    HIDAPI_DriverSantroller_IsEnabled,
    HIDAPI_DriverSantroller_IsSupportedDevice,
    HIDAPI_DriverSantroller_InitDevice,
    HIDAPI_DriverSantroller_GetDevicePlayerIndex,
    HIDAPI_DriverSantroller_SetDevicePlayerIndex,
    HIDAPI_DriverSantroller_UpdateDevice,
    HIDAPI_DriverSantroller_OpenJoystick,
    HIDAPI_DriverSantroller_RumbleJoystick,
    HIDAPI_DriverSantroller_RumbleJoystickTriggers,
    HIDAPI_DriverSantroller_GetJoystickCapabilities,
    HIDAPI_DriverSantroller_SetJoystickLED,
    HIDAPI_DriverSantroller_SendJoystickEffect,
    HIDAPI_DriverSantroller_SetJoystickSensorsEnabled,
    HIDAPI_DriverSantroller_CloseJoystick,
    HIDAPI_DriverSantroller_FreeDevice,
};

#endif // SDL_JOYSTICK_HIDAPI_SANTROLLER

#endif // SDL_JOYSTICK_HIDAPI
