#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUSTOM_VEHICLE_BRIDGE_ABI_VERSION 1u

typedef struct custom_vehicle_adapter custom_vehicle_adapter;

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

// Call after the downloader has persisted the file in the asset cache.
// A successful result queues RPC 184; RPC 185 advances the serialized download queue.
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

#ifdef __cplusplus
}
#endif
