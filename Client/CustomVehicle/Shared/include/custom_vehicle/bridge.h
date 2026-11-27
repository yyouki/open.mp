#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUSTOM_VEHICLE_BRIDGE_ABI_VERSION 1u

typedef struct custom_vehicle_adapter custom_vehicle_adapter;
typedef struct custom_vehicle_native_model_slot_registry custom_vehicle_native_model_slot_registry;

/* Non-zero means that the GTA model-info table entry is currently empty. */
typedef int32_t (*custom_vehicle_native_slot_vacancy_fn)(
	void* user_data,
	int32_t model_slot);

enum
{
	CUSTOM_VEHICLE_MODEL_SLOT_RESERVED = 1,
	CUSTOM_VEHICLE_MODEL_SLOT_ALREADY_RESERVED = 2,
	CUSTOM_VEHICLE_MODEL_SLOT_INVALID_ARGUMENT = -1,
	CUSTOM_VEHICLE_MODEL_SLOT_INVALID_CUSTOM_ID = -2,
	CUSTOM_VEHICLE_MODEL_SLOT_INVALID_RANGE = -3,
	CUSTOM_VEHICLE_MODEL_SLOT_NO_FREE_SLOT = -4,
	CUSTOM_VEHICLE_MODEL_SLOT_RETIRING = -5,
	CUSTOM_VEHICLE_MODEL_SLOT_MISSING_VACANCY_QUERY = -6
};

enum
{
	CUSTOM_VEHICLE_MODEL_SLOT_RELEASED = 1,
	CUSTOM_VEHICLE_MODEL_SLOT_NOT_FOUND = 0,
	CUSTOM_VEHICLE_MODEL_SLOT_RELEASE_MISMATCH = -1,
	CUSTOM_VEHICLE_MODEL_SLOT_NOT_RETIRING = -2,
	CUSTOM_VEHICLE_MODEL_SLOT_IN_USE = -3,
	CUSTOM_VEHICLE_MODEL_SLOT_STILL_OCCUPIED = -4,
	CUSTOM_VEHICLE_MODEL_SLOT_RELEASE_MISSING_QUERY = -5
};

typedef int32_t (*custom_vehicle_send_rpc_fn)(
	void* user_data,
	uint8_t rpc_id,
	const uint8_t* payload,
	size_t payload_size);

typedef int32_t (*custom_vehicle_begin_download_fn)(
	void* user_data,
	uint8_t asset_type,
	uint32_t checksum,
	uint32_t expected_size,
	const char* url,
	size_t url_size);

typedef struct custom_vehicle_callbacks_v1
{
	uint32_t struct_size;
	uint32_t abi_version;
	void* user_data;
	custom_vehicle_send_rpc_fn send_rpc;
	custom_vehicle_begin_download_fn begin_download;
} custom_vehicle_callbacks_v1;

typedef struct custom_vehicle_model_request_v1
{
	uint32_t pool_id;
	uint32_t model_count;
	uint8_t type;
	int32_t virtual_world;
	int32_t base_id;
	int32_t new_id;
	uint32_t dff_checksum;
	uint32_t txd_checksum;
	uint32_t dff_size;
	uint32_t txd_size;
	uint8_t time_on;
	uint8_t time_off;
} custom_vehicle_model_request_v1;

#define CUSTOM_VEHICLE_STREAM_IN_VEHICLE_MOD_COUNT 14u

typedef struct custom_vehicle_stream_in_vehicle_v1
{
	uint16_t vehicle_id;
	int32_t custom_model_id;
	int32_t base_model_id;
	float position[3];
	float angle;
	uint8_t colour1;
	uint8_t colour2;
	float health;
	uint8_t interior;
	uint32_t door_damage;
	uint32_t panel_damage;
	uint8_t light_damage;
	uint8_t tyre_damage;
	uint8_t siren;
	uint8_t mods[CUSTOM_VEHICLE_STREAM_IN_VEHICLE_MOD_COUNT];
	uint8_t paintjob;
	int32_t body_colour1;
	int32_t body_colour2;
} custom_vehicle_stream_in_vehicle_v1;

/* RPC 164 result: -1 invalid/unknown custom model, 0 standard vehicle,
 * 1 registered custom model ready for native loading, 2 registered but assets pending.
 */

// Callback functions run while adapter state is locked. They must not re-enter this API.
// begin_download must copy the borrowed URL and schedule network/file I/O asynchronously.
custom_vehicle_adapter* custom_vehicle_adapter_create(const custom_vehicle_callbacks_v1* callbacks);
void custom_vehicle_adapter_destroy(custom_vehicle_adapter* adapter);

// Starts a fresh capability negotiation (RPC 221) for the current multiplayer connection.
int32_t custom_vehicle_adapter_begin_connection(custom_vehicle_adapter* adapter);
void custom_vehicle_adapter_end_connection(custom_vehicle_adapter* adapter);

// Returns 0 to forward an RPC to the original client, 1 to consume it, and -1 on bad arguments.
int32_t custom_vehicle_adapter_handle_server_rpc(
	custom_vehicle_adapter* adapter,
	uint8_t rpc_id,
	const uint8_t* payload,
	size_t payload_size);

// Call after the downloader has persisted and verified the file in the asset cache.
// A successful result queues RPC 226 with asset_type and checksum; matching ACK RPC 227 advances the serialized queue.
int32_t custom_vehicle_adapter_complete_asset(
	custom_vehicle_adapter* adapter,
	uint8_t asset_type,
	uint32_t checksum,
	const uint8_t* data,
	size_t data_size);

// Requeues the active/failed transfer and dispatches the next checksum request.
int32_t custom_vehicle_adapter_retry_active_asset(custom_vehicle_adapter* adapter);

size_t custom_vehicle_adapter_model_count(const custom_vehicle_adapter* adapter);
int32_t custom_vehicle_adapter_copy_model(
	const custom_vehicle_adapter* adapter,
	size_t index,
	custom_vehicle_model_request_v1* output);

// Returns 1 only after this registered model's DFF and TXD are verified and acknowledged.
int32_t custom_vehicle_adapter_is_model_ready(
	const custom_vehicle_adapter* adapter,
	int32_t custom_model_id);

int32_t custom_vehicle_adapter_decode_stream_in_vehicle(
	const custom_vehicle_adapter* adapter,
	const uint8_t* payload,
	size_t payload_size,
	custom_vehicle_stream_in_vehicle_v1* output);

/* RPC 164 preparation result: -1 invalid, 0 stock vehicle, 1 custom ready,
 * 2 custom manifest known but its DFF/TXD assets are not ready yet.
 * For custom results the 63-byte output differs from input only at model ID
 * bytes 2-5, which are rewritten to the manifest's compatible base vehicle ID.
 */
int32_t custom_vehicle_adapter_rewrite_stream_in_vehicle(
	const custom_vehicle_adapter* adapter,
	const uint8_t* payload,
	size_t payload_size,
	uint8_t* output,
	size_t output_capacity,
	size_t* output_size,
	int32_t* custom_model_id,
	int32_t* base_model_id);

/*
 * Native vehicle model-slot registry.
 *
 * The default slot range is 612-19999, avoiding stock vehicle IDs and staying
 * inside GTA SA's original 20,000-entry model-info table. The caller owns the
 * native model-info pointers and must clear all resources before release.
 */
custom_vehicle_native_model_slot_registry* custom_vehicle_native_model_slot_registry_create(
	int32_t first_slot,
	int32_t last_slot);
void custom_vehicle_native_model_slot_registry_destroy(
	custom_vehicle_native_model_slot_registry* registry);
int32_t custom_vehicle_native_model_slot_registry_has_valid_range(
	const custom_vehicle_native_model_slot_registry* registry);
size_t custom_vehicle_native_model_slot_registry_size(
	const custom_vehicle_native_model_slot_registry* registry);
int32_t custom_vehicle_native_model_slot_registry_reserve(
	custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id,
	custom_vehicle_native_slot_vacancy_fn is_slot_vacant,
	void* user_data,
	int32_t* native_model_slot);
int32_t custom_vehicle_native_model_slot_registry_find(
	const custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id,
	int32_t* native_model_slot);
int32_t custom_vehicle_native_model_slot_registry_retain(
	custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id,
	int32_t* native_model_slot);
int32_t custom_vehicle_native_model_slot_registry_release_use(
	custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id,
	int32_t native_model_slot);
int32_t custom_vehicle_native_model_slot_registry_retire(
	custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id);
int32_t custom_vehicle_native_model_slot_registry_can_release(
	const custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id);
int32_t custom_vehicle_native_model_slot_registry_release(
	custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id,
	int32_t expected_native_model_slot,
	custom_vehicle_native_slot_vacancy_fn is_slot_vacant,
	void* user_data);

#ifdef __cplusplus
}
#endif
