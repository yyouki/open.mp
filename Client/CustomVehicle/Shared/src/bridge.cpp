#include <custom_vehicle/bridge.h>
#include <custom_vehicle/network_adapter.hpp>
#include <custom_vehicle/model_slot_registry.hpp>
#include <custom_vehicle/native_vehicle_model_backend.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using namespace custom_vehicle::protocol;

class CallbackTransport final : public RpcTransport
{
public:
	explicit CallbackTransport(const custom_vehicle_callbacks_v1& callbacks) noexcept
		: callbacks_(callbacks)
	{
	}

	bool sendRpc(RpcId id, const std::uint8_t* payload, std::size_t size) noexcept override
	{
		return callbacks_.send_rpc(
			callbacks_.user_data,
			static_cast<std::uint8_t>(id),
			payload,
			size) != 0;
	}

private:
	const custom_vehicle_callbacks_v1& callbacks_;
};

class CallbackDownloadSink final : public AssetDownloadSink
{
public:
	explicit CallbackDownloadSink(const custom_vehicle_callbacks_v1& callbacks) noexcept
		: callbacks_(callbacks)
	{
	}

	bool beginDownload(const AssetDownload& download) noexcept override
	{
		return callbacks_.begin_download(
			callbacks_.user_data,
			static_cast<std::uint8_t>(download.type),
			download.checksum,
			download.expectedSize,
			download.url.data(),
			download.url.size()) != 0;
	}

private:
	const custom_vehicle_callbacks_v1& callbacks_;
};

struct NativeSlotVacancyCallbackContext
{
	custom_vehicle_native_slot_vacancy_fn callback;
	void* user_data;
};

bool queryNativeSlotVacancy(void* user_data, std::int32_t model_slot) noexcept
{
	const auto* context = static_cast<const NativeSlotVacancyCallbackContext*>(user_data);
	if (context == nullptr || context->callback == nullptr)
	{
		return false;
	}
	try
	{
		return context->callback(context->user_data, model_slot) != 0;
	}
	catch (...)
	{
		return false;
	}
}

struct NativeVehicleCallbackContext
{
	const custom_vehicle_native_vehicle_model_callbacks_v1* callbacks;
};

bool copyVehicleModelRequest(
	const custom_vehicle_model_request_v1& source,
	custom_vehicle::protocol::ModelRequest& destination) noexcept
{
	using custom_vehicle::protocol::ModelType;
	switch (source.type)
	{
		case static_cast<std::uint8_t>(ModelType::Skin):
			destination.type = ModelType::Skin;
			break;
		case static_cast<std::uint8_t>(ModelType::Object):
			destination.type = ModelType::Object;
			break;
		case static_cast<std::uint8_t>(ModelType::Vehicle):
			destination.type = ModelType::Vehicle;
			break;
		default:
			return false;
	}

	destination.poolId = source.pool_id;
	destination.modelCount = source.model_count;
	destination.virtualWorld = source.virtual_world;
	destination.baseId = source.base_id;
	destination.newId = source.new_id;
	destination.dffChecksum = source.dff_checksum;
	destination.txdChecksum = source.txd_checksum;
	destination.dffSize = source.dff_size;
	destination.txdSize = source.txd_size;
	destination.timeOn = source.time_on;
	destination.timeOff = source.time_off;
	return true;
}

custom_vehicle_model_request_v1 makeVehicleModelRequest(
	const custom_vehicle::protocol::ModelRequest& model) noexcept
{
	custom_vehicle_model_request_v1 result {};
	result.pool_id = model.poolId;
	result.model_count = model.modelCount;
	result.type = static_cast<std::uint8_t>(model.type);
	result.virtual_world = model.virtualWorld;
	result.base_id = model.baseId;
	result.new_id = model.newId;
	result.dff_checksum = model.dffChecksum;
	result.txd_checksum = model.txdChecksum;
	result.dff_size = model.dffSize;
	result.txd_size = model.txdSize;
	result.time_on = model.timeOn;
	result.time_off = model.timeOff;
	return result;
}

bool nativeVehicleSlotVacant(void* user_data, std::int32_t slot) noexcept
{
	const auto* context = static_cast<const NativeVehicleCallbackContext*>(user_data);
	if (context == nullptr || context->callbacks == nullptr
		|| context->callbacks->is_slot_vacant == nullptr)
	{
		return false;
	}
	try
	{
		return context->callbacks->is_slot_vacant(
			context->callbacks->user_data,
			slot) != 0;
	}
	catch (...)
	{
		return false;
	}
}

bool nativeVehicleCreateModelInfo(
	void* user_data,
	std::int32_t slot,
	const custom_vehicle::protocol::ModelRequest& model) noexcept
{
	const auto* context = static_cast<const NativeVehicleCallbackContext*>(user_data);
	if (context == nullptr || context->callbacks == nullptr
		|| context->callbacks->create_model_info == nullptr)
	{
		return false;
	}
	const custom_vehicle_model_request_v1 record = makeVehicleModelRequest(model);
	try
	{
		return context->callbacks->create_model_info(
			context->callbacks->user_data,
			slot,
			&record) != 0;
	}
	catch (...)
	{
		return false;
	}
}

bool nativeVehicleLoadAssets(
	void* user_data,
	std::int32_t slot,
	const custom_vehicle::protocol::ModelRequest& model) noexcept
{
	const auto* context = static_cast<const NativeVehicleCallbackContext*>(user_data);
	if (context == nullptr || context->callbacks == nullptr
		|| context->callbacks->load_vehicle_assets == nullptr)
	{
		return false;
	}
	const custom_vehicle_model_request_v1 record = makeVehicleModelRequest(model);
	try
	{
		return context->callbacks->load_vehicle_assets(
			context->callbacks->user_data,
			slot,
			&record) != 0;
	}
	catch (...)
	{
		return false;
	}
}

bool nativeVehicleAttachClump(
	void* user_data,
	std::int32_t slot,
	const custom_vehicle::protocol::ModelRequest& model) noexcept
{
	const auto* context = static_cast<const NativeVehicleCallbackContext*>(user_data);
	if (context == nullptr || context->callbacks == nullptr
		|| context->callbacks->attach_vehicle_clump == nullptr)
	{
		return false;
	}
	const custom_vehicle_model_request_v1 record = makeVehicleModelRequest(model);
	try
	{
		return context->callbacks->attach_vehicle_clump(
			context->callbacks->user_data,
			slot,
			&record) != 0;
	}
	catch (...)
	{
		return false;
	}
}

bool nativeVehicleDestroyModel(
	void* user_data,
	std::int32_t slot,
	const custom_vehicle::protocol::ModelRequest& model,
	custom_vehicle::native::VehicleModelResourceStage completed_stage) noexcept
{
	const auto* context = static_cast<const NativeVehicleCallbackContext*>(user_data);
	if (context == nullptr || context->callbacks == nullptr
		|| context->callbacks->destroy_model == nullptr)
	{
		return false;
	}
	const custom_vehicle_model_request_v1 record = makeVehicleModelRequest(model);
	try
	{
		return context->callbacks->destroy_model(
			context->callbacks->user_data,
			slot,
			&record,
			static_cast<std::uint8_t>(completed_stage)) != 0;
	}
	catch (...)
	{
		return false;
	}
}

bool makeNativeVehicleOperations(
	const custom_vehicle_native_vehicle_model_callbacks_v1* callbacks,
	custom_vehicle::native::VehicleModelNativeOperations& operations) noexcept
{
	if (callbacks == nullptr
		|| callbacks->struct_size < sizeof(custom_vehicle_native_vehicle_model_callbacks_v1)
		|| callbacks->abi_version != CUSTOM_VEHICLE_NATIVE_MODEL_CALLBACKS_ABI_VERSION
		|| callbacks->is_slot_vacant == nullptr
		|| callbacks->create_model_info == nullptr
		|| callbacks->load_vehicle_assets == nullptr
		|| callbacks->attach_vehicle_clump == nullptr
		|| callbacks->destroy_model == nullptr)
	{
		return false;
	}

	operations.is_slot_vacant = nativeVehicleSlotVacant;
	operations.create_model_info = nativeVehicleCreateModelInfo;
	operations.load_vehicle_assets = nativeVehicleLoadAssets;
	operations.attach_vehicle_clump = nativeVehicleAttachClump;
	operations.destroy_model = nativeVehicleDestroyModel;
	return true;
}

std::int32_t mapNativeVehicleBackendResult(
	custom_vehicle::native::VehicleModelBackendResult result) noexcept
{
	using custom_vehicle::native::VehicleModelBackendResult;
	switch (result)
	{
		case VehicleModelBackendResult::Activated:
			return CUSTOM_VEHICLE_NATIVE_MODEL_ACTIVATED;
		case VehicleModelBackendResult::AlreadyActive:
			return CUSTOM_VEHICLE_NATIVE_MODEL_ALREADY_ACTIVE;
		case VehicleModelBackendResult::AssetsPending:
			return CUSTOM_VEHICLE_NATIVE_MODEL_ASSETS_PENDING;
		case VehicleModelBackendResult::InvalidRequest:
			return CUSTOM_VEHICLE_NATIVE_MODEL_INVALID_REQUEST;
		case VehicleModelBackendResult::MissingOperations:
			return CUSTOM_VEHICLE_NATIVE_MODEL_MISSING_OPERATIONS;
		case VehicleModelBackendResult::NoFreeSlot:
			return CUSTOM_VEHICLE_NATIVE_MODEL_NO_FREE_SLOT;
		case VehicleModelBackendResult::ModelRetiring:
			return CUSTOM_VEHICLE_NATIVE_MODEL_RETIRING;
		case VehicleModelBackendResult::ModelReplacementRequired:
			return CUSTOM_VEHICLE_NATIVE_MODEL_REPLACEMENT_REQUIRED;
		case VehicleModelBackendResult::CreateModelInfoFailed:
			return CUSTOM_VEHICLE_NATIVE_MODEL_CREATE_FAILED;
		case VehicleModelBackendResult::LoadAssetsFailed:
			return CUSTOM_VEHICLE_NATIVE_MODEL_LOAD_FAILED;
		case VehicleModelBackendResult::AttachClumpFailed:
			return CUSTOM_VEHICLE_NATIVE_MODEL_ATTACH_FAILED;
		case VehicleModelBackendResult::ModelInUse:
			return CUSTOM_VEHICLE_NATIVE_MODEL_IN_USE;
		case VehicleModelBackendResult::CleanupPending:
			return CUSTOM_VEHICLE_NATIVE_MODEL_CLEANUP_PENDING;
		case VehicleModelBackendResult::Released:
			return CUSTOM_VEHICLE_NATIVE_MODEL_RELEASED;
		case VehicleModelBackendResult::NotFound:
			return CUSTOM_VEHICLE_NATIVE_MODEL_NOT_FOUND;
	}
	return CUSTOM_VEHICLE_NATIVE_MODEL_INVALID_REQUEST;
}
} // namespace

struct custom_vehicle_adapter
{
	custom_vehicle_callbacks_v1 callbacks;
	CallbackTransport transport;
	CallbackDownloadSink downloader;
	custom_vehicle::protocol::ClientNetworkAdapter adapter;

	explicit custom_vehicle_adapter(const custom_vehicle_callbacks_v1& value)
		: callbacks(value)
		, transport(callbacks)
		, downloader(callbacks)
		, adapter(transport, downloader)
	{
	}
};

struct custom_vehicle_native_model_slot_registry
{
	custom_vehicle::native::VehicleModelSlotRegistry registry;

	custom_vehicle_native_model_slot_registry(std::int32_t first_slot, std::int32_t last_slot)
		: registry(first_slot, last_slot)
	{
	}
};

struct custom_vehicle_native_vehicle_model_backend
{
	custom_vehicle::native::VehicleModelLifecycleController controller;

	custom_vehicle_native_vehicle_model_backend(std::int32_t first_slot, std::int32_t last_slot)
		: controller(first_slot, last_slot)
	{
	}
};

extern "C"
{
custom_vehicle_adapter* custom_vehicle_adapter_create(const custom_vehicle_callbacks_v1* callbacks)
{
	if (callbacks == nullptr
		|| callbacks->struct_size < sizeof(custom_vehicle_callbacks_v1)
		|| callbacks->abi_version != CUSTOM_VEHICLE_BRIDGE_ABI_VERSION
		|| callbacks->send_rpc == nullptr
		|| callbacks->begin_download == nullptr)
	{
		return nullptr;
	}

	try
	{
		return new custom_vehicle_adapter(*callbacks);
	}
	catch (...)
	{
		return nullptr;
	}
}

void custom_vehicle_adapter_destroy(custom_vehicle_adapter* adapter)
{
	// The host must stop and join downloader workers before destroying the adapter.
	delete adapter;
}

int32_t custom_vehicle_adapter_begin_connection(custom_vehicle_adapter* adapter)
{
	return adapter != nullptr && adapter->adapter.beginConnection() ? 1 : 0;
}

void custom_vehicle_adapter_end_connection(custom_vehicle_adapter* adapter)
{
	if (adapter != nullptr)
	{
		adapter->adapter.endConnection();
	}
}

int32_t custom_vehicle_adapter_handle_server_rpc(
	custom_vehicle_adapter* adapter,
	uint8_t rpc_id,
	const uint8_t* payload,
	size_t payload_size)
{
	if (adapter == nullptr || (payload == nullptr && payload_size != 0))
	{
		return -1;
	}

	const auto result = adapter->adapter.handleServerRpc(rpc_id, payload, payload_size);
	return result == custom_vehicle::protocol::RpcDisposition::Consume ? 1 : 0;
}

int32_t custom_vehicle_adapter_complete_asset(
	custom_vehicle_adapter* adapter,
	uint8_t asset_type,
	uint32_t checksum,
	const uint8_t* data,
	size_t data_size)
{
	if (adapter == nullptr || (data == nullptr && data_size != 0))
	{
		return 0;
	}

	custom_vehicle::protocol::AssetType type;
	switch (asset_type)
	{
		case static_cast<uint8_t>(custom_vehicle::protocol::AssetType::Dff):
			type = custom_vehicle::protocol::AssetType::Dff;
			break;
		case static_cast<uint8_t>(custom_vehicle::protocol::AssetType::Txd):
			type = custom_vehicle::protocol::AssetType::Txd;
			break;
		default:
			return 0;
	}

	return adapter->adapter.completeAsset(type, checksum, data, data_size) ? 1 : 0;
}

int32_t custom_vehicle_adapter_retry_active_asset(custom_vehicle_adapter* adapter)
{
	return adapter != nullptr && adapter->adapter.retryActiveAsset() ? 1 : 0;
}

size_t custom_vehicle_adapter_model_count(const custom_vehicle_adapter* adapter)
{
	if (adapter == nullptr)
	{
		return 0;
	}

	try
	{
		return adapter->adapter.vehicleModels().size();
	}
	catch (...)
	{
		return 0;
	}
}

int32_t custom_vehicle_adapter_is_model_ready(
	const custom_vehicle_adapter* adapter,
	int32_t custom_model_id)
{
	if (adapter == nullptr)
	{
		return 0;
	}

	try
	{
		return adapter->adapter.isVehicleModelReady(custom_model_id) ? 1 : 0;
	}
	catch (...)
	{
		return 0;
	}
}

int32_t custom_vehicle_adapter_decode_stream_in_vehicle(
	const custom_vehicle_adapter* adapter,
	const uint8_t* payload,
	size_t payload_size,
	custom_vehicle_stream_in_vehicle_v1* output)
{
	if (adapter == nullptr || payload == nullptr || output == nullptr)
	{
		return -1;
	}

	try
	{
		custom_vehicle::protocol::StreamInVehicleData decoded;
		custom_vehicle::protocol::DecodeError error = custom_vehicle::protocol::DecodeError::None;
		if (!custom_vehicle::protocol::decodeStreamInVehicle(payload, payload_size, decoded, error))
		{
			return -1;
		}

		if (!custom_vehicle::protocol::isCustomVehicleModelId(decoded.modelId))
		{
			return decoded.modelId >= 400 && decoded.modelId <= 611 ? 0 : -1;
		}

		custom_vehicle::protocol::ModelRequest model;
		bool modelReady = false;
		if (!adapter->adapter.resolveVehicleModel(decoded.modelId, model, modelReady))
		{
			return -1;
		}

		custom_vehicle_stream_in_vehicle_v1 result {};
		result.vehicle_id = decoded.vehicleId;
		result.custom_model_id = decoded.modelId;
		result.base_model_id = model.baseId;
		std::copy(decoded.position.begin(), decoded.position.end(), result.position);
		result.angle = decoded.angle;
		result.colour1 = decoded.colour1;
		result.colour2 = decoded.colour2;
		result.health = decoded.health;
		result.interior = decoded.interior;
		result.door_damage = decoded.doorDamage;
		result.panel_damage = decoded.panelDamage;
		result.light_damage = decoded.lightDamage;
		result.tyre_damage = decoded.tyreDamage;
		result.siren = decoded.siren;
		std::copy(decoded.mods.begin(), decoded.mods.end(), result.mods);
		result.paintjob = decoded.paintjob;
		result.body_colour1 = decoded.bodyColour1;
		result.body_colour2 = decoded.bodyColour2;
		*output = result;

		return modelReady ? 1 : 2;
	}
	catch (...)
	{
		return -1;
	}
}

int32_t custom_vehicle_adapter_rewrite_stream_in_vehicle(
	const custom_vehicle_adapter* adapter,
	const uint8_t* payload,
	size_t payload_size,
	uint8_t* output,
	size_t output_capacity,
	size_t* output_size,
	int32_t* custom_model_id,
	int32_t* base_model_id)
{
	if (adapter == nullptr || payload == nullptr || output == nullptr
		|| output_size == nullptr || custom_model_id == nullptr || base_model_id == nullptr)
	{
		return -1;
	}

	try
	{
		custom_vehicle::protocol::StreamInVehicleData vehicle;
		custom_vehicle::protocol::DecodeError error = custom_vehicle::protocol::DecodeError::None;
		if (!custom_vehicle::protocol::decodeStreamInVehicle(payload, payload_size, vehicle, error))
		{
			return -1;
		}

		if (vehicle.modelId >= 400 && vehicle.modelId <= 611)
		{
			return 0;
		}
		if (!custom_vehicle::protocol::isCustomVehicleModelId(vehicle.modelId))
		{
			return -1;
		}

		custom_vehicle::protocol::ModelRequest model;
		bool modelReady = false;
		if (!adapter->adapter.resolveVehicleModel(vehicle.modelId, model, modelReady)
			|| !custom_vehicle::protocol::validateVehicleModelRequest(model))
		{
			return -1;
		}

		std::array<std::uint8_t, custom_vehicle::protocol::StreamInVehicleWireSize> rewritten {};
		std::size_t rewrittenSize = 0;
		if (!custom_vehicle::protocol::rewriteStreamInVehicleToBaseModel(
			payload,
			payload_size,
			vehicle.modelId,
			model.baseId,
			rewritten.data(),
			rewritten.size(),
			rewrittenSize,
			error)
			|| output_capacity < rewrittenSize)
		{
			return -1;
		}

		// Publish output and metadata together only after all validation succeeds.
		std::copy(rewritten.begin(), rewritten.begin() + rewrittenSize, output);
		*output_size = rewrittenSize;
		*custom_model_id = vehicle.modelId;
		*base_model_id = model.baseId;
		return modelReady ? 1 : 2;
	}
	catch (...)
	{
		return -1;
	}
}

int32_t custom_vehicle_adapter_copy_model(
	const custom_vehicle_adapter* adapter,
	size_t index,
	custom_vehicle_model_request_v1* output)
{
	if (adapter == nullptr || output == nullptr)
	{
		return 0;
	}

	try
	{
		const std::vector<custom_vehicle::protocol::ModelRequest> models =
			adapter->adapter.vehicleModels();
		if (index >= models.size())
		{
			return 0;
		}

		const auto& model = models[index];
		custom_vehicle_model_request_v1 result {};
		result.pool_id = model.poolId;
		result.model_count = model.modelCount;
		result.type = static_cast<std::uint8_t>(model.type);
		result.virtual_world = model.virtualWorld;
		result.base_id = model.baseId;
		result.new_id = model.newId;
		result.dff_checksum = model.dffChecksum;
		result.txd_checksum = model.txdChecksum;
		result.dff_size = model.dffSize;
		result.txd_size = model.txdSize;
		result.time_on = model.timeOn;
		result.time_off = model.timeOff;
		*output = result;
		return 1;
	}
	catch (...)
	{
		return 0;
	}
}

custom_vehicle_native_model_slot_registry* custom_vehicle_native_model_slot_registry_create(
	int32_t first_slot,
	int32_t last_slot)
{
	try
	{
		return new custom_vehicle_native_model_slot_registry(first_slot, last_slot);
	}
	catch (...)
	{
		return nullptr;
	}
}

void custom_vehicle_native_model_slot_registry_destroy(
	custom_vehicle_native_model_slot_registry* registry)
{
	delete registry;
}

int32_t custom_vehicle_native_model_slot_registry_has_valid_range(
	const custom_vehicle_native_model_slot_registry* registry)
{
	return registry != nullptr && registry->registry.hasValidRange() ? 1 : 0;
}

size_t custom_vehicle_native_model_slot_registry_size(
	const custom_vehicle_native_model_slot_registry* registry)
{
	return registry == nullptr ? 0 : registry->registry.size();
}

int32_t custom_vehicle_native_model_slot_registry_reserve(
	custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id,
	custom_vehicle_native_slot_vacancy_fn is_slot_vacant,
	void* user_data,
	int32_t* native_model_slot)
{
	if (registry == nullptr || native_model_slot == nullptr)
	{
		return CUSTOM_VEHICLE_MODEL_SLOT_INVALID_ARGUMENT;
	}

	NativeSlotVacancyCallbackContext callback_context { is_slot_vacant, user_data };
	const auto query = is_slot_vacant == nullptr ? nullptr : queryNativeSlotVacancy;
	std::int32_t slot = -1;
	const auto result = registry->registry.reserve(
		custom_model_id,
		query,
		is_slot_vacant == nullptr ? nullptr : &callback_context,
		slot);

	switch (result)
	{
		case custom_vehicle::native::ModelSlotReserveResult::Reserved:
			*native_model_slot = slot;
			return CUSTOM_VEHICLE_MODEL_SLOT_RESERVED;
		case custom_vehicle::native::ModelSlotReserveResult::AlreadyReserved:
			*native_model_slot = slot;
			return CUSTOM_VEHICLE_MODEL_SLOT_ALREADY_RESERVED;
		case custom_vehicle::native::ModelSlotReserveResult::InvalidCustomModelId:
			return CUSTOM_VEHICLE_MODEL_SLOT_INVALID_CUSTOM_ID;
		case custom_vehicle::native::ModelSlotReserveResult::InvalidSlotRange:
			return CUSTOM_VEHICLE_MODEL_SLOT_INVALID_RANGE;
		case custom_vehicle::native::ModelSlotReserveResult::NoFreeSlot:
			return CUSTOM_VEHICLE_MODEL_SLOT_NO_FREE_SLOT;
		case custom_vehicle::native::ModelSlotReserveResult::ModelRetiring:
			return CUSTOM_VEHICLE_MODEL_SLOT_RETIRING;
		case custom_vehicle::native::ModelSlotReserveResult::MissingVacancyQuery:
			return CUSTOM_VEHICLE_MODEL_SLOT_MISSING_VACANCY_QUERY;
	}
	return CUSTOM_VEHICLE_MODEL_SLOT_INVALID_ARGUMENT;
}

int32_t custom_vehicle_native_model_slot_registry_find(
	const custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id,
	int32_t* native_model_slot)
{
	if (registry == nullptr || native_model_slot == nullptr)
	{
		return 0;
	}
	return registry->registry.find(custom_model_id, *native_model_slot) ? 1 : 0;
}

int32_t custom_vehicle_native_model_slot_registry_retain(
	custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id,
	int32_t* native_model_slot)
{
	if (registry == nullptr || native_model_slot == nullptr)
	{
		return 0;
	}
	return registry->registry.retain(custom_model_id, *native_model_slot) ? 1 : 0;
}

int32_t custom_vehicle_native_model_slot_registry_release_use(
	custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id,
	int32_t native_model_slot)
{
	return registry != nullptr
		&& registry->registry.releaseUse(custom_model_id, native_model_slot) ? 1 : 0;
}

int32_t custom_vehicle_native_model_slot_registry_retire(
	custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id)
{
	return registry != nullptr && registry->registry.retire(custom_model_id) ? 1 : 0;
}

int32_t custom_vehicle_native_model_slot_registry_can_release(
	const custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id)
{
	return registry != nullptr && registry->registry.canRelease(custom_model_id) ? 1 : 0;
}

int32_t custom_vehicle_native_model_slot_registry_release(
	custom_vehicle_native_model_slot_registry* registry,
	int32_t custom_model_id,
	int32_t expected_native_model_slot,
	custom_vehicle_native_slot_vacancy_fn is_slot_vacant,
	void* user_data)
{
	if (registry == nullptr)
	{
		return CUSTOM_VEHICLE_MODEL_SLOT_INVALID_ARGUMENT;
	}

	NativeSlotVacancyCallbackContext callback_context { is_slot_vacant, user_data };
	const auto query = is_slot_vacant == nullptr ? nullptr : queryNativeSlotVacancy;
	const auto result = registry->registry.release(
		custom_model_id,
		expected_native_model_slot,
		query,
		is_slot_vacant == nullptr ? nullptr : &callback_context);

	switch (result)
	{
		case custom_vehicle::native::ModelSlotReleaseResult::Released:
			return CUSTOM_VEHICLE_MODEL_SLOT_RELEASED;
		case custom_vehicle::native::ModelSlotReleaseResult::NotFound:
			return CUSTOM_VEHICLE_MODEL_SLOT_NOT_FOUND;
		case custom_vehicle::native::ModelSlotReleaseResult::SlotMismatch:
			return CUSTOM_VEHICLE_MODEL_SLOT_RELEASE_MISMATCH;
		case custom_vehicle::native::ModelSlotReleaseResult::NotRetiring:
			return CUSTOM_VEHICLE_MODEL_SLOT_NOT_RETIRING;
		case custom_vehicle::native::ModelSlotReleaseResult::ModelInUse:
			return CUSTOM_VEHICLE_MODEL_SLOT_IN_USE;
		case custom_vehicle::native::ModelSlotReleaseResult::NativeSlotStillOccupied:
			return CUSTOM_VEHICLE_MODEL_SLOT_STILL_OCCUPIED;
		case custom_vehicle::native::ModelSlotReleaseResult::MissingVacancyQuery:
			return CUSTOM_VEHICLE_MODEL_SLOT_RELEASE_MISSING_QUERY;
	}
	return CUSTOM_VEHICLE_MODEL_SLOT_INVALID_ARGUMENT;
}

custom_vehicle_native_vehicle_model_backend* custom_vehicle_native_vehicle_model_backend_create(
	int32_t first_slot,
	int32_t last_slot)
{
	try
	{
		return new custom_vehicle_native_vehicle_model_backend(first_slot, last_slot);
	}
	catch (...)
	{
		return nullptr;
	}
}

void custom_vehicle_native_vehicle_model_backend_destroy(
	custom_vehicle_native_vehicle_model_backend* backend)
{
	delete backend;
}

int32_t custom_vehicle_native_vehicle_model_backend_has_valid_range(
	const custom_vehicle_native_vehicle_model_backend* backend)
{
	return backend != nullptr && backend->controller.hasValidSlotRange() ? 1 : 0;
}

size_t custom_vehicle_native_vehicle_model_backend_size(
	const custom_vehicle_native_vehicle_model_backend* backend)
{
	return backend == nullptr ? 0 : backend->controller.size();
}

int32_t custom_vehicle_native_vehicle_model_backend_activate(
	custom_vehicle_native_vehicle_model_backend* backend,
	const custom_vehicle_model_request_v1* model,
	int32_t assets_ready,
	const custom_vehicle_native_vehicle_model_callbacks_v1* callbacks,
	int32_t* native_model_slot)
{
	if (backend == nullptr || model == nullptr || native_model_slot == nullptr)
	{
		return CUSTOM_VEHICLE_NATIVE_MODEL_INVALID_REQUEST;
	}

	custom_vehicle::protocol::ModelRequest decoded_model;
	if (!copyVehicleModelRequest(*model, decoded_model))
	{
		return CUSTOM_VEHICLE_NATIVE_MODEL_INVALID_REQUEST;
	}

	custom_vehicle::native::VehicleModelNativeOperations operations;
	if (!makeNativeVehicleOperations(callbacks, operations))
	{
		return CUSTOM_VEHICLE_NATIVE_MODEL_MISSING_OPERATIONS;
	}

	NativeVehicleCallbackContext callback_context { callbacks };
	std::int32_t slot = -1;
	try
	{
		const auto result = backend->controller.activate(
			decoded_model,
			assets_ready != 0,
			operations,
			&callback_context,
			slot);
		const std::int32_t mapped = mapNativeVehicleBackendResult(result);
		if (mapped == CUSTOM_VEHICLE_NATIVE_MODEL_ACTIVATED
			|| mapped == CUSTOM_VEHICLE_NATIVE_MODEL_ALREADY_ACTIVE)
		{
			*native_model_slot = slot;
		}
		return mapped;
	}
	catch (...)
	{
		return CUSTOM_VEHICLE_NATIVE_MODEL_INVALID_REQUEST;
	}
}

int32_t custom_vehicle_native_vehicle_model_backend_retain_for_vehicle(
	custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id,
	int32_t* native_model_slot)
{
	if (backend == nullptr || native_model_slot == nullptr)
	{
		return 0;
	}
	try
	{
		std::int32_t slot = -1;
		if (!backend->controller.retainForVehicle(custom_model_id, slot))
		{
			return 0;
		}
		*native_model_slot = slot;
		return 1;
	}
	catch (...)
	{
		return 0;
	}
}

int32_t custom_vehicle_native_vehicle_model_backend_release_vehicle_use(
	custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id,
	int32_t native_model_slot)
{
	if (backend == nullptr)
	{
		return 0;
	}
	try
	{
		return backend->controller.releaseVehicleUse(custom_model_id, native_model_slot) ? 1 : 0;
	}
	catch (...)
	{
		return 0;
	}
}

int32_t custom_vehicle_native_vehicle_model_backend_retire(
	custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id,
	const custom_vehicle_native_vehicle_model_callbacks_v1* callbacks)
{
	if (backend == nullptr)
	{
		return CUSTOM_VEHICLE_NATIVE_MODEL_INVALID_REQUEST;
	}

	custom_vehicle::native::VehicleModelNativeOperations operations;
	if (!makeNativeVehicleOperations(callbacks, operations))
	{
		return CUSTOM_VEHICLE_NATIVE_MODEL_MISSING_OPERATIONS;
	}
	NativeVehicleCallbackContext callback_context { callbacks };
	try
	{
		return mapNativeVehicleBackendResult(
			backend->controller.retire(custom_model_id, operations, &callback_context));
	}
	catch (...)
	{
		return CUSTOM_VEHICLE_NATIVE_MODEL_CLEANUP_PENDING;
	}
}

int32_t custom_vehicle_native_vehicle_model_backend_retry_cleanup(
	custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id,
	const custom_vehicle_native_vehicle_model_callbacks_v1* callbacks)
{
	if (backend == nullptr)
	{
		return CUSTOM_VEHICLE_NATIVE_MODEL_INVALID_REQUEST;
	}

	custom_vehicle::native::VehicleModelNativeOperations operations;
	if (!makeNativeVehicleOperations(callbacks, operations))
	{
		return CUSTOM_VEHICLE_NATIVE_MODEL_MISSING_OPERATIONS;
	}
	NativeVehicleCallbackContext callback_context { callbacks };
	try
	{
		return mapNativeVehicleBackendResult(
			backend->controller.retryRetiredCleanup(custom_model_id, operations, &callback_context));
	}
	catch (...)
	{
		return CUSTOM_VEHICLE_NATIVE_MODEL_CLEANUP_PENDING;
	}
}

int32_t custom_vehicle_native_vehicle_model_backend_find(
	const custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id,
	int32_t* native_model_slot)
{
	if (backend == nullptr || native_model_slot == nullptr)
	{
		return 0;
	}
	try
	{
		std::int32_t slot = -1;
		if (!backend->controller.find(custom_model_id, slot))
		{
			return 0;
		}
		*native_model_slot = slot;
		return 1;
	}
	catch (...)
	{
		return 0;
	}
}

int32_t custom_vehicle_native_vehicle_model_backend_is_active(
	const custom_vehicle_native_vehicle_model_backend* backend,
	int32_t custom_model_id)
{
	if (backend == nullptr)
	{
		return 0;
	}
	try
	{
		return backend->controller.isActive(custom_model_id) ? 1 : 0;
	}
	catch (...)
	{
		return 0;
	}
}
} // extern "C"
