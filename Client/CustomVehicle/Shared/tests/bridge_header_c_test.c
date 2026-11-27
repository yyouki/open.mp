#include <custom_vehicle/bridge.h>

static int32_t send_rpc(
	void* user_data,
	uint8_t rpc_id,
	const uint8_t* payload,
	size_t payload_size)
{
	(void)user_data;
	(void)rpc_id;
	(void)payload;
	(void)payload_size;
	return 1;
}

static int32_t begin_download(
	void* user_data,
	uint8_t asset_type,
	uint32_t checksum,
	uint32_t expected_size,
	const char* url,
	size_t url_size)
{
	(void)user_data;
	(void)asset_type;
	(void)checksum;
	(void)expected_size;
	(void)url;
	(void)url_size;
	return 1;
}

void custom_vehicle_bridge_c_header_smoke(void)
{
	custom_vehicle_callbacks_v1 callbacks = { 0 };
	custom_vehicle_model_request_v1 model = { 0 };

	callbacks.struct_size = (uint32_t)sizeof(callbacks);
	callbacks.abi_version = CUSTOM_VEHICLE_BRIDGE_ABI_VERSION;
	callbacks.send_rpc = send_rpc;
	callbacks.begin_download = begin_download;
	model.type = 3;

	(void)custom_vehicle_adapter_create;
	(void)custom_vehicle_adapter_destroy;
	(void)custom_vehicle_adapter_begin_connection;
	(void)custom_vehicle_adapter_end_connection;
	(void)custom_vehicle_adapter_handle_server_rpc;
	(void)custom_vehicle_adapter_complete_asset;
	(void)custom_vehicle_adapter_retry_active_asset;
	(void)custom_vehicle_adapter_model_count;
	(void)custom_vehicle_adapter_copy_model;
	(void)custom_vehicle_adapter_is_model_ready;
	(void)custom_vehicle_adapter_decode_stream_in_vehicle;
	{
		custom_vehicle_stream_in_vehicle_v1 stream = { 0 };
		(void)stream;
	}
	(void)callbacks;
	(void)model;
}
