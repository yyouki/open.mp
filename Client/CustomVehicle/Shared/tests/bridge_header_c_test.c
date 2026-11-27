#include <custom_vehicle/bridge.h>
#include <custom_vehicle_windows.h>

/* C99-compatible compile-time ABI checks for the Windows/native spawn bridge. */
typedef char custom_vehicle_stream_in_vehicle_size_must_be_76[
	sizeof(custom_vehicle_stream_in_vehicle_v1) == 76 ? 1 : -1];
typedef char custom_vehicle_stream_in_vehicle_position_must_be_at_12[
	offsetof(custom_vehicle_stream_in_vehicle_v1, position) == 12 ? 1 : -1];
typedef char custom_vehicle_stream_in_vehicle_health_must_be_at_32[
	offsetof(custom_vehicle_stream_in_vehicle_v1, health) == 32 ? 1 : -1];
typedef char custom_vehicle_stream_in_vehicle_mods_must_be_at_51[
	offsetof(custom_vehicle_stream_in_vehicle_v1, mods) == 51 ? 1 : -1];
typedef char custom_vehicle_stream_in_vehicle_body_colours_must_be_at_68[
	offsetof(custom_vehicle_stream_in_vehicle_v1, body_colour1) == 68 ? 1 : -1];
typedef char custom_vehicle_model_request_size_must_be_44[
	sizeof(custom_vehicle_model_request_v1) == 44 ? 1 : -1];
typedef char custom_vehicle_model_request_type_must_be_at_8[
	offsetof(custom_vehicle_model_request_v1, type) == 8 ? 1 : -1];
typedef char custom_vehicle_model_request_base_id_must_be_at_16[
	offsetof(custom_vehicle_model_request_v1, base_id) == 16 ? 1 : -1];
typedef char custom_vehicle_model_request_dff_checksum_must_be_at_24[
	offsetof(custom_vehicle_model_request_v1, dff_checksum) == 24 ? 1 : -1];
typedef char custom_vehicle_model_request_time_on_must_be_at_40[
	offsetof(custom_vehicle_model_request_v1, time_on) == 40 ? 1 : -1];
typedef char custom_vehicle_native_asset_pending_must_be_1[
	CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_PENDING == 1 ? 1 : -1];
typedef char custom_vehicle_native_asset_ready_must_be_2[
	CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_READY == 2 ? 1 : -1];
typedef char custom_vehicle_native_stage_reserved_must_be_0[
	CUSTOM_VEHICLE_NATIVE_MODEL_STAGE_RESERVED == 0 ? 1 : -1];
typedef char custom_vehicle_native_stage_model_info_must_be_1[
	CUSTOM_VEHICLE_NATIVE_MODEL_STAGE_MODEL_INFO_CREATED == 1 ? 1 : -1];
typedef char custom_vehicle_native_stage_assets_requested_must_be_2[
	CUSTOM_VEHICLE_NATIVE_MODEL_STAGE_ASSETS_REQUESTED == 2 ? 1 : -1];
typedef char custom_vehicle_native_stage_assets_loaded_must_be_3[
	CUSTOM_VEHICLE_NATIVE_MODEL_STAGE_ASSETS_LOADED == 3 ? 1 : -1];
typedef char custom_vehicle_native_stage_clump_attached_must_be_4[
	CUSTOM_VEHICLE_NATIVE_MODEL_STAGE_CLUMP_ATTACHED == 4 ? 1 : -1];

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
	(void)custom_vehicle_adapter_rewrite_stream_in_vehicle;
	(void)CustomVehicleWindows_Shutdown;
	(void)CustomVehicleWindows_ModelCount;
	(void)CustomVehicleWindows_CopyModel;
	(void)CustomVehicleWindows_DecodeStreamInVehicle;
	(void)CustomVehicleWindows_RewriteStreamInVehicle;
	(void)CustomVehicleWindows_IsModelReady;
	(void)custom_vehicle_native_model_slot_registry_create;
	(void)custom_vehicle_native_model_slot_registry_destroy;
	(void)custom_vehicle_native_model_slot_registry_has_valid_range;
	(void)custom_vehicle_native_model_slot_registry_size;
	(void)custom_vehicle_native_model_slot_registry_reserve;
	(void)custom_vehicle_native_model_slot_registry_find;
	(void)custom_vehicle_native_model_slot_registry_retain;
	(void)custom_vehicle_native_model_slot_registry_release_use;
	(void)custom_vehicle_native_model_slot_registry_retire;
	(void)custom_vehicle_native_model_slot_registry_can_release;
	(void)custom_vehicle_native_model_slot_registry_release;
	(void)custom_vehicle_native_vehicle_model_backend_create;
	(void)custom_vehicle_native_vehicle_model_backend_destroy;
	(void)custom_vehicle_native_vehicle_model_backend_has_valid_range;
	(void)custom_vehicle_native_vehicle_model_backend_size;
	(void)custom_vehicle_native_vehicle_model_backend_activate;
	(void)custom_vehicle_native_vehicle_model_backend_retain_for_vehicle;
	(void)custom_vehicle_native_vehicle_model_backend_release_vehicle_use;
	(void)custom_vehicle_native_vehicle_model_backend_retire;
	(void)custom_vehicle_native_vehicle_model_backend_retry_cleanup;
	(void)custom_vehicle_native_vehicle_model_backend_find;
	(void)custom_vehicle_native_vehicle_model_backend_is_active;
	(void)custom_vehicle_pending_vehicle_queue_create;
	(void)custom_vehicle_pending_vehicle_queue_destroy;
	(void)custom_vehicle_pending_vehicle_queue_size;
	(void)custom_vehicle_pending_vehicle_queue_enqueue;
	(void)custom_vehicle_pending_vehicle_queue_pop;
	(void)custom_vehicle_pending_vehicle_queue_cancel;
	(void)custom_vehicle_pending_vehicle_queue_clear;
	{
		custom_vehicle_stream_in_vehicle_v1 stream = { 0 };
		(void)stream;
	}
	(void)callbacks;
	(void)model;
}
