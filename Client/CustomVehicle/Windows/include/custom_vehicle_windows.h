#pragma once

#include <custom_vehicle/bridge.h>

#if defined(_WIN32) && defined(_MSC_VER)
#define CUSTOM_VEHICLE_WINDOWS_CALL __stdcall
#elif defined(_WIN32) && (defined(__GNUC__) || defined(__clang__))
#define CUSTOM_VEHICLE_WINDOWS_CALL __attribute__((stdcall))
#else
#define CUSTOM_VEHICLE_WINDOWS_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Exports provided by custom_vehicle_windows.asi.
 *
 * This header intentionally declares only the stable native-facing adapter
 * surface. It does not expose pointers to GTA, RenderWare, or the SDK host.
 */

#define CUSTOM_VEHICLE_WINDOWS_STATUS_INVALID_OR_UNKNOWN (-1)
#define CUSTOM_VEHICLE_WINDOWS_STATUS_STANDARD_VEHICLE 0
#define CUSTOM_VEHICLE_WINDOWS_STATUS_CUSTOM_READY 1
#define CUSTOM_VEHICLE_WINDOWS_STATUS_CUSTOM_PENDING 2

/* Stop callbacks/workers before unloading the ASI. Must not run in DllMain,
 * a network callback, or the game thread. Returns non-zero on success.
 */
int32_t CUSTOM_VEHICLE_WINDOWS_CALL CustomVehicleWindows_Shutdown(void);

/* Returns the number of custom model records currently held by the adapter. */
size_t CUSTOM_VEHICLE_WINDOWS_CALL CustomVehicleWindows_ModelCount(void);

/* Copies one model manifest entry; returns non-zero on success. */
int32_t CUSTOM_VEHICLE_WINDOWS_CALL CustomVehicleWindows_CopyModel(
    size_t index,
    custom_vehicle_model_request_v1* output);

/* Decodes the exact payload bytes of server RPC 164.
 *
 * Return values:
 *  -1: malformed payload, unknown custom model, or inactive adapter.
 *   0: stock vehicle; pass the original payload to the stock handler.
 *   1: registered custom vehicle with verified and acknowledged DFF/TXD.
 *   2: registered custom vehicle whose assets are still pending.
 *
 * The output is written only for return values 0, 1, and 2.
 */
int32_t CUSTOM_VEHICLE_WINDOWS_CALL CustomVehicleWindows_DecodeStreamInVehicle(
    const uint8_t* payload,
    size_t payload_size,
    custom_vehicle_stream_in_vehicle_v1* output);

/* Rechecks local verification plus server acknowledgement for both assets. */
int32_t CUSTOM_VEHICLE_WINDOWS_CALL CustomVehicleWindows_IsModelReady(
    int32_t custom_model_id);

#ifdef __cplusplus
}
#endif

#undef CUSTOM_VEHICLE_WINDOWS_CALL
