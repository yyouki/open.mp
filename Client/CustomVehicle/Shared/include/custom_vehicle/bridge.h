#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CUSTOM_VEHICLE_BRIDGE_ABI_VERSION 1u

typedef struct custom_vehicle_adapter custom_vehicle_adapter;
typedef struct custom_vehicle_native_model_slot_registry custom_vehicle_native_model_slot_registry;
typedef struct custom_vehicle_native_vehicle_model_backend custom_vehicle_native_vehicle_model_backend;
typedef struct custom_vehicle_pending_vehicle_queue custom_vehicle_pending_vehicle_queue;
struct custom_vehicle_model_request_v1;

/* Non-zero means that the GTA model-info table entry is currently empty. */
typedef int32_t (*custom_vehicle_native_slot_vacancy_fn)(
	void* user_data,
	int32_t model_slot);

/*
 * Callbacks for the staged native vehicle model lifecycle controller.
 *
 * The caller must invoke activate/retire/retry from the verified GTA game
 * thread. Callbacks run synchronously while controller state is locked and
 * must not re-enter the backend API. On a failed create/load/attach callback,
 * that callback must leave no resources of its own operation behind.
 *
 * request_or_poll_vehicle_assets is called on successive game ticks. It
 * returns FAILED (0), PENDING (1), or READY (2); it must be idempotent and must
 * not block while GTA streaming works. destroy_model must be idempotent. It is
 * passed the last fully completed stage and returns non-zero only when all
 * native resources have been cleared. A zero return keeps the slot retired and
 * reserved for a later cleanup retry.
 */
enum
{
	CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_FAILED = 0,
	CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_PENDING = 1,
	CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_READY = 2
};

/* Last fully completed resource stage passed to destroy_model. */
enum
{
	CUSTOM_VEHICLE_NATIVE_MODEL_STAGE_RESERVED = 0,
	CUSTOM_VEHICLE_NATIVE_MODEL_STAGE_MODEL_INFO_CREATED = 1,
	CUSTOM_VEHICLE_NATIVE_MODEL_STAGE_ASSETS_REQUESTED = 2,
	CUSTOM_VEHICLE_NATIVE_MODEL_STAGE_ASSETS_LOADED = 3,
	CUSTOM_VEHICLE_NATIVE_MODEL_STAGE_CLUMP_ATTACHED = 4
};

typedef int32_t (*custom_vehicle_native_model_create_fn)(
	void* user_data,
	int32_t native_model_slot,
	const struct custom_vehicle_model_request_v1* model);
/* Called repeatedly from the game thread: request once, then poll until Ready/Failed. */
typedef int32_t (*custom_vehicle_native_model_load_fn)(
	void* user_data,
	int32_t native_model_slot,
	const struct custom_vehicle_model_request_v1* model);
typedef int32_t (*custom_vehicle_native_model_attach_fn)(
	void* user_data,
	int32_t native_model_slot,
	const struct custom_vehicle_model_request_v1* model);
typedef int32_t (*custom_vehicle_native_model_destroy_fn)(
	void* user_data,
	int32_t native_model_slot,
	const struct custom_vehicle_model_request_v1* model,
	uint8_t completed_stage);

#define CUSTOM_VEHICLE_NATIVE_MODEL_CALLBACKS_ABI_VERSION 1u

typedef struct custom_vehicle_native_vehicle_model_callbacks_v1
{
	uint32_t struct_size;
	uint32_t abi_version;
	void* user_data;
	custom_vehicle_native_slot_vacancy_fn is_slot_vacant;
	custom_vehicle_native_model_create_fn create_model_info;
	custom_vehicle_native_model_load_fn request_or_poll_vehicle_assets;
	custom_vehicle_native_model_attach_fn attach_vehicle_clump;
	custom_vehicle_native_model_destroy_fn destroy_model;
} custom_vehicle_native_vehicle_model_callbacks_v1;

enum
{
	CUSTOM_VEHICLE_STREAM_QUEUE_QUEUED = 1,
	CUSTOM_VEHICLE_STREAM_QUEUE_UPDATED = 2,
	CUSTOM_VEHICLE_STREAM_QUEUE_INVALID_VEHICLE_ID = -1,
	CUSTOM_VEHICLE_STREAM_QUEUE_INVALID_CUSTOM_MODEL = -2,
	CUSTOM_VEHICLE_STREAM_QUEUE_INVALID_BASE_MODEL = -3,
	CUSTOM_VEHICLE_STREAM_QUEUE_INVALID_DATA = -4,
	CUSTOM_VEHICLE_STREAM_QUEUE_FULL = -5
};

enum
{
	CUSTOM_VEHICLE_NATIVE_MODEL_ACTIVATED = 1,
	CUSTOM_VEHICLE_NATIVE_MODEL_ALREADY_ACTIVE = 2,
	CUSTOM_VEHICLE_NATIVE_MODEL_ASSETS_PENDING = 3,
	CUSTOM_VEHICLE_NATIVE_MODEL_RELEASED = 4,
	CUSTOM_VEHICLE_NATIVE_MODEL_NATIVE_ASSETS_PENDING = 5,
	CUSTOM_VEHICLE_NATIVE_MODEL_NOT_FOUND = 0,
	CUSTOM_VEHICLE_NATIVE_MODEL_INVALID_REQUEST = -1,
	CUSTOM_VEHICLE_NATIVE_MODEL_MISSING_OPERATIONS = -2,
	CUSTOM_VEHICLE_NATIVE_MODEL_NO_FREE_SLOT = -3,
	CUSTOM_VEHICLE_NATIVE_MODEL_RETIRING = -4,
	CUSTOM_VEHICLE_NATIVE_MODEL_REPLACEMENT_REQUIRED = -5,
	CUSTOM_VEHICLE_NATIVE_MODEL_CREATE_FAILED = -6,
	CUSTOM_VEHICLE_NATIVE_MODEL_LOAD_FAILED = -7,
	CUSTOM_VEHICLE_NATIVE_MODEL_ATTACH_FAILED = -8,
	CUSTOM_VEHICLE_NATIVE_MODEL_IN_USE = -9,
	CUSTOM_VEHICLE_NATIVE_MODEL_CLEANUP_PENDING = -10
};

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
/* Caller must quiesce concurrent API calls before destruction. Returns 1 and
 * destroys the registry only when it owns no reserved slots. A 0 return leaves
 * the registry valid and unchanged.
 */
int32_t custom_vehicle_native_model_slot_registry_destroy(
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

/*
 * Staged native vehicle model lifecycle API.
 *
 * activate returns ASSETS_PENDING until both DFF and TXD have passed local
 * size/CRC checks and the server acknowledgement. Native callbacks are
 * synchronous and must be run only from a verified game-thread context.
 */
custom_vehicle_native_vehicle_model_backend* custom_vehicle_native_vehicle_model_backend_create(
	int32_t first_slot,
	int32_t last_slot);
/* Caller must quiesce concurrent API calls before destruction. Returns 1 and
 * destroys the backend only after every model lifecycle entry is released. A
 * 0 return leaves the backend and all pending cleanup state valid and unchanged.
 */
int32_t custom_vehicle_native_vehicle_model_backend_destroy(
	custom_vehicle_native_vehicle_model_backend* backend);
int32_t custom_vehicle_native_vehicle_model_backend_has_valid_range(
	const custom_vehicle_native_vehicle_model_backend* backend);
size_t custom_vehicle_native_vehicle_model_backend_size(
	const custom_vehicle_native_vehicle_model_backend* backend);
int32_t custom_vehicle_native_vehicle_model_backend_activate(
	custom_vehicle_native_vehicle_model_backend* backend,
	const custom_vehicle_model_request_v1* model,
	int32_t assets_ready,
	const custom_vehicle_native_vehicle_model_callbacks_v1* callbacks,
	int32_t* native_model_slot);
int32_t custom_vehicle_native_vehicle_model_backend_retain_for_vehicle(
	custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id,
	int32_t* native_model_slot);
int32_t custom_vehicle_native_vehicle_model_backend_release_vehicle_use(
	custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id,
	int32_t native_model_slot);
int32_t custom_vehicle_native_vehicle_model_backend_retire(
	custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id,
	const custom_vehicle_native_vehicle_model_callbacks_v1* callbacks);
int32_t custom_vehicle_native_vehicle_model_backend_retry_cleanup(
	custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id,
	const custom_vehicle_native_vehicle_model_callbacks_v1* callbacks);
int32_t custom_vehicle_native_vehicle_model_backend_find(
	const custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id,
	int32_t* native_model_slot);
int32_t custom_vehicle_native_vehicle_model_backend_is_active(
	const custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id);

/*
 * Bounded RPC-164 handoff queue.
 *
 * Enqueue copied, validated custom stream-in data in the network callback.
 * Drain it from a verified game-thread callback. Duplicate pending updates for
 * one vehicle ID replace that vehicle's queued values without growing the
 * queue. Cancel on the matching vehicle-remove event before native processing.
 */
custom_vehicle_pending_vehicle_queue* custom_vehicle_pending_vehicle_queue_create(void);
/* Caller must quiesce concurrent API calls. Returns 1 only when the queue is empty. */
int32_t custom_vehicle_pending_vehicle_queue_destroy(
	custom_vehicle_pending_vehicle_queue* queue);
size_t custom_vehicle_pending_vehicle_queue_size(
	const custom_vehicle_pending_vehicle_queue* queue);
int32_t custom_vehicle_pending_vehicle_queue_enqueue(
	custom_vehicle_pending_vehicle_queue* queue,
	const custom_vehicle_stream_in_vehicle_v1* request);
int32_t custom_vehicle_pending_vehicle_queue_pop(
	custom_vehicle_pending_vehicle_queue* queue,
	custom_vehicle_stream_in_vehicle_v1* output);
int32_t custom_vehicle_pending_vehicle_queue_cancel(
	custom_vehicle_pending_vehicle_queue* queue,
	uint16_t vehicle_id);
void custom_vehicle_pending_vehicle_queue_clear(
	custom_vehicle_pending_vehicle_queue* queue);

#ifdef __cplusplus
}
#endif
