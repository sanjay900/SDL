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

#include "SDL_hidapijoystick_c.h"
#include "SDL_hidapi_rumble.h"
#include "SDL_hidapi_santroller.h"
#include "SDL_report_descriptor.h"

#ifdef SDL_JOYSTICK_HIDAPI_SANTROLLER

/*****************************************************************************************************/
// This protocol is documented at:
// https://docs.handheldlegend.com/s/sinput
/*****************************************************************************************************/

// Define this if you want to log all packets from the controller
#if 0
#define DEBUG_SANTROLLER_PROTOCOL
#endif

#if 0
#define DEBUG_SANTROLLER_INIT
#endif


#define SANTROLLER_DEVICE_REPORT_SIZE           64 // Size of input reports (And CMD Input reports)
#define SANTROLLER_DEVICE_REPORT_COMMAND_SIZE   48 // Size of command OUTPUT reports
#define SANTROLLER_DEVICE_FEATURES_REPORT_SIZE  2  // Size of features report
#define SANTROLLER_DEVICE_FEATURES_USAGE        0x2021  // Usage page for features report

#define SANTROLLER_DEVICE_REPORT_ID_JOYSTICK_INPUT  0x01
#define SANTROLLER_DEVICE_REPORT_ID_INPUT_CMDDAT    0x02
#define SANTROLLER_DEVICE_REPORT_ID_OUTPUT_CMDDAT   0x03
#define SANTROLLER_DEVICE_REPORT_ID_FEATURES    0x10

typedef struct
{
    SDL_HIDAPI_Device *device;
    Uint16 protocol_version;
    Uint16 usb_device_version;
    bool sensors_enabled;

    Uint8 player_idx;

    bool player_leds_supported;
    bool joystick_rgb_supported;
    bool rumble_supported;
    bool accelerometer_supported;
    bool gyroscope_supported;
    bool left_analog_stick_supported;
    bool right_analog_stick_supported;
    bool left_analog_trigger_supported;
    bool right_analog_trigger_supported;
    bool dpad_supported;
    bool touchpad_supported;
    bool is_handheld;

    Uint8 touchpad_count;        // 2 touchpads maximum
    Uint8 touchpad_finger_count; // 2 fingers for one touchpad, or 1 per touchpad (2 max)

    Uint16 polling_rate_us;
    Uint8 sub_product;    // Subtype of the device, 0 in most cases

    Uint16 accelRange; // Example would be 2,4,8,16 +/- (g-force)
    Uint16 gyroRange;  // Example would be 1000,2000,4000 +/- (degrees per second)

    float accelScale; // Scale factor for accelerometer values
    float gyroScale;  // Scale factor for gyroscope values
    Uint8 last_state[USB_PACKET_LENGTH];

    Uint8 axes_count;
    Uint8 buttons_count;
    Uint8 usage_masks[4];

    Uint32 last_imu_timestamp_us;

    Uint64 imu_timestamp_ns; // Nanoseconds. We accumulate with received deltas
} SDL_DriverSantroller_Context;

// Converts raw int16_t gyro scale setting
static inline float CalculateGyroScale(uint16_t dps_range)
{
    return SDL_PI_F / 180.0f / (32768.0f / (float)dps_range);
}

// Converts raw int16_t accel scale setting
static inline float CalculateAccelScale(uint16_t g_range)
{
    return SDL_STANDARD_GRAVITY / (32768.0f / (float)g_range);
}

static bool RetrieveSDLFeatures(SDL_HIDAPI_Device *device)
{
    int written = 0;

    // Attempt to send the SDL features get command.
    for (int attempt = 0; attempt < 8; ++attempt) {
        const Uint8 featuresGetCommand[SANTROLLER_DEVICE_FEATURES_REPORT_SIZE] = { SANTROLLER_DEVICE_REPORT_ID_FEATURES, 0 };
        // This write will occasionally return -1, so ignore failure here and try again
        written = SDL_hid_write(device->dev, featuresGetCommand, sizeof(featuresGetCommand));

        if (written == SANTROLLER_DEVICE_FEATURES_REPORT_SIZE) {
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

        Uint8 data[SANTROLLER_DEVICE_FEATURES_REPORT_SIZE];
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

        if ((read == SANTROLLER_DEVICE_FEATURES_REPORT_SIZE) && (data[0] == SANTROLLER_DEVICE_REPORT_ID_FEATURES)) {
            SDL_Log("Received Santroller SDL Features response %d", data[1]);
#if defined(DEBUG_SANTROLLER_INIT)
            SDL_Log("Received Santroller SDL Features command response");
#endif
            return true;
        }
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

    Uint8 descriptor[1024];
    int descriptor_len = SDL_hid_get_report_descriptor(device->dev, descriptor, sizeof(descriptor));
    SDL_ReportDescriptor* parsed_descriptor = SDL_ParseReportDescriptor(descriptor, descriptor_len);
    if (SDL_DescriptorHasUsage(parsed_descriptor, 0xFF00, SANTROLLER_DEVICE_FEATURES_USAGE)) {
        // New revesion device, request features from the device
        if (!RetrieveSDLFeatures(device)) {
            return false;
        }
    } else {
        // Older revision device, features are encoded into device version
        SDL_log("test: %04x", device->version);
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

    if (ctx->dpad_supported) {
        joystick->nhats = 1;
    }

    if (ctx->gyroscope_supported) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_GYRO, 1000000.0f / ctx->polling_rate_us);
    }

    if (ctx->accelerometer_supported) {
        SDL_PrivateJoystickAddSensor(joystick, SDL_SENSOR_ACCEL, 1000000.0f / ctx->polling_rate_us);
    }

    return true;
}

static bool HIDAPI_DriverSantroller_RumbleJoystick(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble)
{

    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)device->context;

    // if (ctx->rumble_supported) {
    //     SANTROLLER_HAPTIC_S hapticData = { 0 };
    //     Uint8 hapticReport[SANTROLLER_DEVICE_REPORT_COMMAND_SIZE] = { SANTROLLER_DEVICE_REPORT_ID_OUTPUT_CMDDAT, SANTROLLER_DEVICE_COMMAND_HAPTIC };

    //     // Low Frequency  = Left
    //     // High Frequency = Right
    //     hapticData.type_2.left.amplitude = (Uint8) (low_frequency_rumble >> 8);
    //     hapticData.type_2.right.amplitude = (Uint8)(high_frequency_rumble >> 8);

    //     HapticsType2Pack(&hapticData, &(hapticReport[2]));

    //     SDL_HIDAPI_SendRumble(device, hapticReport, SANTROLLER_DEVICE_REPORT_COMMAND_SIZE);

    //     return true;
    // }

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
    // if (ctx->rumble_supported) {
    //     caps |= SDL_JOYSTICK_CAP_RUMBLE;
    // }

    // if (ctx->player_leds_supported) {
    //     caps |= SDL_JOYSTICK_CAP_PLAYER_LED;
    // }

    // if (ctx->joystick_rgb_supported) {
    //     caps |= SDL_JOYSTICK_CAP_RGB_LED;
    // }

    return caps;
}

static bool HIDAPI_DriverSantroller_SetJoystickLED(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, Uint8 red, Uint8 green, Uint8 blue)
{
    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)device->context;

    // if (ctx->joystick_rgb_supported) {
    //     Uint8 joystickRGBCommand[SANTROLLER_DEVICE_REPORT_COMMAND_SIZE] = { SANTROLLER_DEVICE_REPORT_ID_OUTPUT_CMDDAT, SANTROLLER_DEVICE_COMMAND_JOYSTICKRGB, red, green, blue };
    //     int joystickRGBBytesWritten = SDL_hid_write(device->dev, joystickRGBCommand, SANTROLLER_DEVICE_REPORT_COMMAND_SIZE);

    //     if (joystickRGBBytesWritten < 0) {
    //         SDL_SetError("Santroller device joystick rgb command could not write");
    //         return false;
    //     }

    //     return true;
    // }
    return SDL_Unsupported();
}

static bool HIDAPI_DriverSantroller_SendJoystickEffect(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, const void *data, int size)
{
    return SDL_Unsupported();
}

static bool HIDAPI_DriverSantroller_SetJoystickSensorsEnabled(SDL_HIDAPI_Device *device, SDL_Joystick *joystick, bool enabled)
{
    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)device->context;

    // if (ctx->accelerometer_supported || ctx->gyroscope_supported) {
    //     ctx->sensors_enabled = enabled;
    //     return true;
    // }
    return SDL_Unsupported();
}

static void HIDAPI_DriverSantroller_HandleStatePacket(SDL_Joystick *joystick, SDL_DriverSantroller_Context *ctx, Uint8 *data, int size)
{
    // Sint16 axis = 0;
    // Sint16 accel = 0;
    // Sint16 gyro = 0;
    // Uint64 timestamp = SDL_GetTicksNS();
    // float imu_values[3] = { 0 };
    // Uint8 output_idx = 0;

    // // Process digital buttons according to the supplied
    // // button mask to create a contiguous button input set
    // for (Uint8 processes = 0; processes < 4; ++processes) {

    //     Uint8 button_idx = SANTROLLER_REPORT_IDX_BUTTONS_0 + processes;

    //     for (Uint8 buttons = 0; buttons < 8; ++buttons) {

    //         // If a button is enabled by our usage mask
    //         const Uint8 mask = (0x01 << buttons);
    //         if ((ctx->usage_masks[processes] & mask) != 0) {

    //             bool down = (data[button_idx] & mask) != 0;

    //             if ( (output_idx < SDL_GAMEPAD_BUTTON_COUNT) && (ctx->last_state[button_idx] != data[button_idx]) ) {
    //                 SDL_SendJoystickButton(timestamp, joystick, output_idx, down);
    //             }

    //             ++output_idx;
    //         }
    //     }
    // }

    // if (ctx->dpad_supported) {
    //     Uint8 hat = SDL_HAT_CENTERED;

    //     if (data[SANTROLLER_REPORT_IDX_BUTTONS_0] & (1 << SANTROLLER_BUTTON_IDX_DPAD_UP)) {
    //         hat |= SDL_HAT_UP;
    //     }
    //     if (data[SANTROLLER_REPORT_IDX_BUTTONS_0] & (1 << SANTROLLER_BUTTON_IDX_DPAD_DOWN)) {
    //         hat |= SDL_HAT_DOWN;
    //     }
    //     if (data[SANTROLLER_REPORT_IDX_BUTTONS_0] & (1 << SANTROLLER_BUTTON_IDX_DPAD_LEFT)) {
    //         hat |= SDL_HAT_LEFT;
    //     }
    //     if (data[SANTROLLER_REPORT_IDX_BUTTONS_0] & (1 << SANTROLLER_BUTTON_IDX_DPAD_RIGHT)) {
    //         hat |= SDL_HAT_RIGHT;
    //     }
    //     SDL_SendJoystickHat(timestamp, joystick, 0, hat);
    // }

    // // Analog inputs map to a signed Sint16 range of -32768 to 32767 from the device.
    // // Use an axis index because not all gamepads will have the same axis inputs.
    // Uint8 axis_idx = 0;

    // // Left Analog Stick
    // axis = 0; // Reset axis value for joystick
    // if (ctx->left_analog_stick_supported) {
    //     axis = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_LEFT_X);
    //     SDL_SendJoystickAxis(timestamp, joystick, axis_idx, axis);
    //     ++axis_idx;

    //     axis = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_LEFT_Y);
    //     SDL_SendJoystickAxis(timestamp, joystick, axis_idx, axis);
    //     ++axis_idx;
    // }

    // // Right Analog Stick
    // axis = 0; // Reset axis value for joystick
    // if (ctx->right_analog_stick_supported) {
    //     axis = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_RIGHT_X);
    //     SDL_SendJoystickAxis(timestamp, joystick, axis_idx, axis);
    //     ++axis_idx;

    //     axis = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_RIGHT_Y);
    //     SDL_SendJoystickAxis(timestamp, joystick, axis_idx, axis);
    //     ++axis_idx;
    // }

    // // Left Analog Trigger
    // axis = SDL_MIN_SINT16; // Reset axis value for trigger
    // if (ctx->left_analog_trigger_supported) {
    //     axis = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_LEFT_TRIGGER);
    //     SDL_SendJoystickAxis(timestamp, joystick, axis_idx, axis);
    //     ++axis_idx;
    // }

    // // Right Analog Trigger
    // axis = SDL_MIN_SINT16; // Reset axis value for trigger
    // if (ctx->right_analog_trigger_supported) {
    //     axis = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_RIGHT_TRIGGER);
    //     SDL_SendJoystickAxis(timestamp, joystick, axis_idx, axis);
    // }

    // // Battery/Power state handling
    // if (ctx->last_state[SANTROLLER_REPORT_IDX_PLUG_STATUS]  != data[SANTROLLER_REPORT_IDX_PLUG_STATUS] ||
    //     ctx->last_state[SANTROLLER_REPORT_IDX_CHARGE_LEVEL] != data[SANTROLLER_REPORT_IDX_CHARGE_LEVEL]) {

    //     SDL_PowerState state = SDL_POWERSTATE_UNKNOWN;
    //     Uint8 status = data[SANTROLLER_REPORT_IDX_PLUG_STATUS];
    //     int percent = data[SANTROLLER_REPORT_IDX_CHARGE_LEVEL];

    //     percent = SDL_clamp(percent, 0, 100); // Ensure percent is within valid range

    //     switch (status) {
    //     case 1:
    //         state = SDL_POWERSTATE_NO_BATTERY;
    //         percent = 0;
    //         break;
    //     case 2:
    //         state = SDL_POWERSTATE_CHARGING;
    //         break;
    //     case 3:
    //         state = SDL_POWERSTATE_CHARGED;
    //         percent = 100;
    //         break;
    //     case 4:
    //         state = SDL_POWERSTATE_ON_BATTERY;
    //         break;
    //     default:
    //         break;
    //     }

    //     if (state != SDL_POWERSTATE_UNKNOWN) {
    //         SDL_SendJoystickPowerInfo(joystick, state, percent);
    //     }
    // }

    // // Extract the IMU timestamp delta (in microseconds)
    // Uint32 imu_timestamp_us = EXTRACTUINT32(data, SANTROLLER_REPORT_IDX_IMU_TIMESTAMP);
    // Uint32 imu_time_delta_us = 0;

    // // Check if we should process IMU data and if sensors are enabled
    // if (ctx->sensors_enabled) {

    //     if (imu_timestamp_us >= ctx->last_imu_timestamp_us) {
    //         imu_time_delta_us = (imu_timestamp_us - ctx->last_imu_timestamp_us);
    //     } else {
    //         // Handle rollover case
    //         imu_time_delta_us = (UINT32_MAX - ctx->last_imu_timestamp_us) + imu_timestamp_us + 1;
    //     }

    //     // Convert delta to nanoseconds and update running timestamp
    //     ctx->imu_timestamp_ns += (Uint64)imu_time_delta_us * 1000;

    //     // Update last timestamp
    //     ctx->last_imu_timestamp_us = imu_timestamp_us;

    //     // Process Gyroscope
    //     if (ctx->gyroscope_supported) {

    //         gyro = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_IMU_GYRO_Y);
    //         imu_values[2] = -(float)gyro * ctx->gyroScale; // Y-axis rotation

    //         gyro = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_IMU_GYRO_Z);
    //         imu_values[1] = (float)gyro * ctx->gyroScale; // Z-axis rotation

    //         gyro = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_IMU_GYRO_X);
    //         imu_values[0] = -(float)gyro * ctx->gyroScale; // X-axis rotation

    //         SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_GYRO, ctx->imu_timestamp_ns, imu_values, 3);
    //     }

    //     // Process Accelerometer
    //     if (ctx->accelerometer_supported) {

    //         accel = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_IMU_ACCEL_Y);
    //         imu_values[2] = -(float)accel * ctx->accelScale; // Y-axis acceleration

    //         accel = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_IMU_ACCEL_Z);
    //         imu_values[1] = (float)accel * ctx->accelScale; // Z-axis acceleration

    //         accel = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_IMU_ACCEL_X);
    //         imu_values[0] = -(float)accel * ctx->accelScale; // X-axis acceleration

    //         SDL_SendJoystickSensor(timestamp, joystick, SDL_SENSOR_ACCEL, ctx->imu_timestamp_ns, imu_values, 3);
    //     }
    // }

    // // Check if we should process touchpad
    // if (ctx->touchpad_supported && ctx->touchpad_count > 0) {
    //     Uint8 touchpad = 0;
    //     Uint8 finger = 0;

    //     Sint16 touch1X = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_TOUCH1_X);
    //     Sint16 touch1Y = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_TOUCH1_Y);
    //     Uint16 touch1P = EXTRACTUINT16(data, SANTROLLER_REPORT_IDX_TOUCH1_P);

    //     Sint16 touch2X = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_TOUCH2_X);
    //     Sint16 touch2Y = EXTRACTSINT16(data, SANTROLLER_REPORT_IDX_TOUCH2_Y);
    //     Uint16 touch2P = EXTRACTUINT16(data, SANTROLLER_REPORT_IDX_TOUCH2_P);

    //     SDL_SendJoystickTouchpad(timestamp, joystick, touchpad, finger,
    //         touch1P > 0,
    //         touch1X / 65536.0f + 0.5f,
    //         touch1Y / 65536.0f + 0.5f,
    //         touch1P / 32768.0f);

    //     if (ctx->touchpad_count > 1) {
    //         ++touchpad;
    //     } else if (ctx->touchpad_finger_count > 1) {
    //         ++finger;
    //     }

    //     if ((touchpad > 0) || (finger > 0)) {
    //         SDL_SendJoystickTouchpad(timestamp, joystick, touchpad, finger,
    //                                  touch2P > 0,
    //                                  touch2X / 65536.0f + 0.5f,
    //                                  touch2Y / 65536.0f + 0.5f,
    //                                  touch2P / 32768.0f);
    //     }
    // }

    // SDL_memcpy(ctx->last_state, data, SDL_min(size, sizeof(ctx->last_state)));
}

static bool HIDAPI_DriverSantroller_UpdateDevice(SDL_HIDAPI_Device *device)
{
    SDL_DriverSantroller_Context *ctx = (SDL_DriverSantroller_Context *)device->context;
    SDL_Joystick *joystick = NULL;
//     Uint8 data[USB_PACKET_LENGTH];
//     int size = 0;

//     if (device->num_joysticks > 0) {
//         joystick = SDL_GetJoystickFromID(device->joysticks[0]);
//     } else {
//         return false;
//     }

//     while ((size = SDL_hid_read_timeout(device->dev, data, sizeof(data), 0)) > 0) {
// #ifdef DEBUG_SANTROLLER_PROTOCOL
//         HIDAPI_DumpPacket("Santroller packet: size = %d", data, size);
// #endif
//         if (!joystick) {
//             continue;
//         }

//         if (data[0] == SANTROLLER_DEVICE_REPORT_ID_JOYSTICK_INPUT) {
//             HIDAPI_DriverSantroller_HandleStatePacket(joystick, ctx, data, size);
//         }
//     }

//     if (size < 0) {
//         // Read error, device is disconnected
//         HIDAPI_JoystickDisconnected(device, device->joysticks[0]);
//     }
//     return (size >= 0);
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
