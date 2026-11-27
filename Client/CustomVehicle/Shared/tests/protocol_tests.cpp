#include <custom_vehicle/protocol.hpp>
#include <custom_vehicle/session.hpp>
#include <custom_vehicle/network_adapter.hpp>
#include <custom_vehicle/asset_cache.hpp>
#include <custom_vehicle/bridge.h>
#include <custom_vehicle/model_slot_registry.hpp>
#include <custom_vehicle/vehicle_stream_queue.hpp>
#include <custom_vehicle/native_vehicle_model_backend.hpp>

#ifdef NDEBUG
#undef NDEBUG // Keep assertions active even in Release test builds.
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace custom_vehicle::protocol;

struct CAbiNativeSlotState
{
	std::vector<std::int32_t> occupiedSlots;
};

extern "C" std::int32_t cAbiNativeModelSlotIsVacant(void* userData, std::int32_t modelSlot)
{
	if (userData == nullptr)
	{
		return 0;
	}
	const auto& state = *static_cast<const CAbiNativeSlotState*>(userData);
	for (const std::int32_t occupied : state.occupiedSlots)
	{
		if (occupied == modelSlot)
		{
			return 0;
		}
	}
	return 1;
}

static_assert(sizeof(custom_vehicle_stream_in_vehicle_v1) == 76);
static_assert(offsetof(custom_vehicle_stream_in_vehicle_v1, vehicle_id) == 0);
static_assert(offsetof(custom_vehicle_stream_in_vehicle_v1, custom_model_id) == 4);
static_assert(offsetof(custom_vehicle_stream_in_vehicle_v1, position) == 12);
static_assert(offsetof(custom_vehicle_stream_in_vehicle_v1, health) == 32);
static_assert(offsetof(custom_vehicle_stream_in_vehicle_v1, mods) == 51);
static_assert(offsetof(custom_vehicle_stream_in_vehicle_v1, body_colour1) == 68);
static_assert(sizeof(custom_vehicle_vehicle_model_binding_v1) == 24);
static_assert(offsetof(custom_vehicle_vehicle_model_binding_v1, custom_model_id) == 4);
static_assert(offsetof(custom_vehicle_vehicle_model_binding_v1, native_model_slot) == 8);
static_assert(offsetof(custom_vehicle_vehicle_model_binding_v1, generation) == 16);

namespace
{
struct NativeSlotVacancyState
{
	std::vector<std::int32_t> occupiedSlots;
};

bool isNativeModelSlotVacant(void* userData, std::int32_t slot) noexcept
{
	if (userData == nullptr)
	{
		return false;
	}
	const auto& state = *static_cast<const NativeSlotVacancyState*>(userData);
	for (const std::int32_t occupied : state.occupiedSlots)
	{
		if (occupied == slot)
		{
			return false;
		}
	}
	return true;
}

struct NativeVehicleBackendFixture
{
	std::vector<std::int32_t> occupiedSlots { 612 };
	std::vector<char> callbackOrder;
	bool modelInfoCreated = false;
	bool assetsLoaded = false;
	bool clumpAttached = false;
	bool failCreate = false;
	bool failLoad = false;
	bool failAttach = false;
	bool failDestroyOnce = false;
	std::size_t pendingAssetPolls = 0;
	std::int32_t lastBaseModelId = -1;
	std::int32_t lastCustomModelId = -1;
};

bool backendSlotIsVacant(void* userData, std::int32_t slot) noexcept
{
	if (userData == nullptr)
	{
		return false;
	}
	const auto& fixture = *static_cast<const NativeVehicleBackendFixture*>(userData);
	return std::find(
		fixture.occupiedSlots.begin(),
		fixture.occupiedSlots.end(),
		slot) == fixture.occupiedSlots.end();
}

bool backendCreateModelInfo(
	void* userData,
	std::int32_t slot,
	const ModelRequest& model) noexcept
{
	auto& fixture = *static_cast<NativeVehicleBackendFixture*>(userData);
	fixture.callbackOrder.push_back('C');
	if (fixture.failCreate || !backendSlotIsVacant(userData, slot))
	{
		return false;
	}
	fixture.occupiedSlots.push_back(slot);
	fixture.modelInfoCreated = true;
	fixture.lastBaseModelId = model.baseId;
	fixture.lastCustomModelId = model.newId;
	return true;
}

custom_vehicle::native::VehicleAssetLoadResult backendRequestOrPollVehicleAssets(
	void* userData,
	std::int32_t slot,
	const ModelRequest& model) noexcept
{
	using custom_vehicle::native::VehicleAssetLoadResult;
	auto& fixture = *static_cast<NativeVehicleBackendFixture*>(userData);
	fixture.callbackOrder.push_back('L');
	if (fixture.failLoad || !fixture.modelInfoCreated
		|| fixture.lastCustomModelId != model.newId
		|| backendSlotIsVacant(userData, slot))
	{
		return VehicleAssetLoadResult::Failed;
	}
	if (fixture.pendingAssetPolls != 0)
	{
		--fixture.pendingAssetPolls;
		return VehicleAssetLoadResult::Pending;
	}
	fixture.assetsLoaded = true;
	return VehicleAssetLoadResult::Ready;
}

bool backendAttachVehicleClump(
	void* userData,
	std::int32_t slot,
	const ModelRequest& model) noexcept
{
	auto& fixture = *static_cast<NativeVehicleBackendFixture*>(userData);
	fixture.callbackOrder.push_back('A');
	if (fixture.failAttach || !fixture.modelInfoCreated || !fixture.assetsLoaded
		|| fixture.lastCustomModelId != model.newId
		|| backendSlotIsVacant(userData, slot))
	{
		return false;
	}
	fixture.clumpAttached = true;
	return true;
}

bool backendDestroyModel(
	void* userData,
	std::int32_t slot,
	const ModelRequest& model,
	custom_vehicle::native::VehicleModelResourceStage completedStage) noexcept
{
	auto& fixture = *static_cast<NativeVehicleBackendFixture*>(userData);
	fixture.callbackOrder.push_back('D');
	if (fixture.failDestroyOnce)
	{
		fixture.failDestroyOnce = false;
		return false; // Preserve all state so cleanup can be retried.
	}

	assert(model.newId == fixture.lastCustomModelId || !fixture.modelInfoCreated);
	(void)completedStage;
	fixture.clumpAttached = false;
	fixture.assetsLoaded = false;
	fixture.modelInfoCreated = false;
	for (auto it = fixture.occupiedSlots.begin(); it != fixture.occupiedSlots.end(); ++it)
	{
		if (*it == slot)
		{
			fixture.occupiedSlots.erase(it);
			break;
		}
	}
	return true;
}

custom_vehicle::native::VehicleModelNativeOperations makeNativeVehicleOperations()
{
	return {
		backendSlotIsVacant,
		backendCreateModelInfo,
		backendRequestOrPollVehicleAssets,
		backendAttachVehicleClump,
		backendDestroyModel
	};
}

ModelRequest makeNativeVehicleModelRequest(
	std::int32_t customModelId,
	std::int32_t baseModelId)
{
	ModelRequest model;
	model.poolId = 0;
	model.modelCount = 1;
	model.type = ModelType::Vehicle;
	model.virtualWorld = -1;
	model.baseId = baseModelId;
	model.newId = customModelId;
	model.dffChecksum = 0x10203040u;
	model.txdChecksum = 0x50607080u;
	model.dffSize = 4096;
	model.txdSize = 2048;
	return model;
}

ModelRequest makeNativeVehicleModelRequest(
	const custom_vehicle_model_request_v1& source)
{
	ModelRequest model;
	model.poolId = source.pool_id;
	model.modelCount = source.model_count;
	model.type = static_cast<ModelType>(source.type);
	model.virtualWorld = source.virtual_world;
	model.baseId = source.base_id;
	model.newId = source.new_id;
	model.dffChecksum = source.dff_checksum;
	model.txdChecksum = source.txd_checksum;
	model.dffSize = source.dff_size;
	model.txdSize = source.txd_size;
	model.timeOn = source.time_on;
	model.timeOff = source.time_off;
	return model;
}

std::int32_t cAbiNativeVehicleSlotVacant(void* userData, std::int32_t slot)
{
	return backendSlotIsVacant(userData, slot) ? 1 : 0;
}

std::int32_t cAbiNativeVehicleCreate(
	void* userData,
	std::int32_t slot,
	const custom_vehicle_model_request_v1* model)
{
	return userData != nullptr && model != nullptr
		&& backendCreateModelInfo(userData, slot, makeNativeVehicleModelRequest(*model)) ? 1 : 0;
}

std::int32_t cAbiNativeVehicleLoad(
	void* userData,
	std::int32_t slot,
	const custom_vehicle_model_request_v1* model)
{
	if (userData == nullptr || model == nullptr)
	{
		return CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_FAILED;
	}
	switch (backendRequestOrPollVehicleAssets(
		userData, slot, makeNativeVehicleModelRequest(*model)))
	{
		case custom_vehicle::native::VehicleAssetLoadResult::Pending:
			return CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_PENDING;
		case custom_vehicle::native::VehicleAssetLoadResult::Ready:
			return CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_READY;
		case custom_vehicle::native::VehicleAssetLoadResult::Failed:
		default:
			return CUSTOM_VEHICLE_NATIVE_ASSET_LOAD_FAILED;
	}
}

std::int32_t cAbiNativeVehicleAttach(
	void* userData,
	std::int32_t slot,
	const custom_vehicle_model_request_v1* model)
{
	return userData != nullptr && model != nullptr
		&& backendAttachVehicleClump(userData, slot, makeNativeVehicleModelRequest(*model)) ? 1 : 0;
}

std::int32_t cAbiNativeVehicleDestroy(
	void* userData,
	std::int32_t slot,
	const custom_vehicle_model_request_v1* model,
	std::uint8_t completedStage)
{
	return userData != nullptr && model != nullptr
		&& backendDestroyModel(
			userData,
			slot,
			makeNativeVehicleModelRequest(*model),
			static_cast<custom_vehicle::native::VehicleModelResourceStage>(completedStage))
		? 1 : 0;
}

custom_vehicle_native_vehicle_model_callbacks_v1 makeNativeVehicleCAbiCallbacks(
	NativeVehicleBackendFixture& fixture)
{
	custom_vehicle_native_vehicle_model_callbacks_v1 callbacks {};
	callbacks.struct_size = sizeof(callbacks);
	callbacks.abi_version = CUSTOM_VEHICLE_NATIVE_MODEL_CALLBACKS_ABI_VERSION;
	callbacks.user_data = &fixture;
	callbacks.is_slot_vacant = cAbiNativeVehicleSlotVacant;
	callbacks.create_model_info = cAbiNativeVehicleCreate;
	callbacks.request_or_poll_vehicle_assets = cAbiNativeVehicleLoad;
	callbacks.attach_vehicle_clump = cAbiNativeVehicleAttach;
	callbacks.destroy_model = cAbiNativeVehicleDestroy;
	return callbacks;
}

void testNativeVehicleModelVehicleBindings()
{
	using namespace custom_vehicle::native;

	NativeVehicleBackendFixture fixture;
	VehicleModelLifecycleController backend(612, 615);
	const auto operations = makeNativeVehicleOperations();
	const ModelRequest model = makeNativeVehicleModelRequest(30001, 400);
	std::int32_t slot = -1;
	assert(backend.activate(model, true, operations, &fixture, slot)
		== VehicleModelBackendResult::Activated);
	assert(slot == 613);

	VehicleModelBinding first;
	assert(backend.bindVehicle(77, 30001, first) == VehicleModelBindingResult::Bound);
	assert(first.vehicle_id == 77);
	assert(first.custom_model_id == 30001);
	assert(first.native_model_slot == slot);
	assert(first.generation != 0);

	VehicleModelBinding repeated;
	assert(backend.bindVehicle(77, 30001, repeated)
		== VehicleModelBindingResult::AlreadyBound);
	assert(repeated.generation == first.generation);
	assert(repeated.native_model_slot == first.native_model_slot);

	// A different model cannot silently take over a pool ID that is still bound.
	VehicleModelBinding conflict;
	assert(backend.bindVehicle(77, 30002, conflict)
		== VehicleModelBindingResult::VehicleAlreadyBound);
	assert(backend.bindVehicle(0, 30001, conflict)
		== VehicleModelBindingResult::InvalidVehicleId);
	assert(backend.bindVehicle(2000, 30001, conflict)
		== VehicleModelBindingResult::InvalidVehicleId);
	assert(backend.bindVehicle(78, 30002, conflict)
		== VehicleModelBindingResult::ModelNotActive);

	VehicleModelBinding found;
	assert(backend.findVehicleBinding(77, found));
	assert(found.generation == first.generation);

	// An old generation cannot release the current lease.
	VehicleModelBinding stale = first;
	--stale.generation;
	assert(!backend.releaseVehicleBinding(stale));
	assert(backend.findVehicleBinding(77, found));
	assert(found.generation == first.generation);

	// Once the native vehicle has actually been removed, release then reuse its
	// SA-MP pool ID. The replacement receives a strictly newer generation token.
	assert(backend.releaseVehicleBinding(first));
	assert(!backend.releaseVehicleBinding(first));
	assert(!backend.findVehicleBinding(77, found));

	VehicleModelBinding replacement;
	assert(backend.bindVehicle(77, 30001, replacement) == VehicleModelBindingResult::Bound);
	assert(replacement.generation > first.generation);
	assert(!backend.releaseVehicleBinding(first));
	assert(backend.bindVehicle(78, 30001, conflict) == VehicleModelBindingResult::Bound);

	// The legacy anonymous release API cannot release a generation-bound lease.
	assert(!backend.releaseVehicleUse(30001, slot));

	// Retirement waits for every vehicle-specific lease, not for a guessed count.
	assert(backend.retire(30001, operations, &fixture) == VehicleModelBackendResult::ModelInUse);
	assert(!backend.isActive(30001));
	assert(backend.bindVehicle(77, 30001, found) == VehicleModelBindingResult::AlreadyBound);
	assert(backend.releaseVehicleBinding(replacement));
	assert(backend.retryRetiredCleanup(30001, operations, &fixture)
		== VehicleModelBackendResult::ModelInUse);
	assert(backend.releaseVehicleBinding(conflict));
	assert(backend.retryRetiredCleanup(30001, operations, &fixture)
		== VehicleModelBackendResult::Released);
	assert(backend.size() == 0);
	assert(!backend.findVehicleBinding(77, found));
	assert(!backend.findVehicleBinding(78, found));
	assert(backendSlotIsVacant(&fixture, slot));
}

void testNativeVehicleModelVehicleBindingsCAbi()
{
	NativeVehicleBackendFixture fixture;
	const auto callbacks = makeNativeVehicleCAbiCallbacks(fixture);
	auto* backend = custom_vehicle_native_vehicle_model_backend_create(612, 615);
	assert(backend != nullptr);

	custom_vehicle_model_request_v1 model {};
	model.pool_id = 0;
	model.model_count = 1;
	model.type = static_cast<std::uint8_t>(ModelType::Vehicle);
	model.virtual_world = -1;
	model.base_id = 400;
	model.new_id = 30001;
	model.dff_checksum = 0x10203040u;
	model.txd_checksum = 0x50607080u;
	model.dff_size = 4096;
	model.txd_size = 2048;

	std::int32_t slot = -1;
	assert(custom_vehicle_native_vehicle_model_backend_activate(
		backend, &model, 1, &callbacks, &slot) == CUSTOM_VEHICLE_NATIVE_MODEL_ACTIVATED);
	assert(slot == 613);

	custom_vehicle_vehicle_model_binding_v1 first {};
	assert(custom_vehicle_native_vehicle_model_backend_bind_vehicle(
		backend, 77, 30001, &first) == CUSTOM_VEHICLE_MODEL_BINDING_BOUND);
	assert(first.vehicle_id == 77);
	assert(first.custom_model_id == 30001);
	assert(first.native_model_slot == slot);
	assert(first.generation != 0);
	assert(first.reserved == 0 && first.reserved2 == 0);

	custom_vehicle_vehicle_model_binding_v1 repeated {};
	assert(custom_vehicle_native_vehicle_model_backend_bind_vehicle(
		backend, 77, 30001, &repeated) == CUSTOM_VEHICLE_MODEL_BINDING_ALREADY_BOUND);
	assert(repeated.generation == first.generation);
	assert(custom_vehicle_native_vehicle_model_backend_bind_vehicle(
		backend, 77, 30002, &repeated) == CUSTOM_VEHICLE_MODEL_BINDING_VEHICLE_ALREADY_BOUND);
	assert(custom_vehicle_native_vehicle_model_backend_bind_vehicle(
		backend, 0, 30001, &repeated) == CUSTOM_VEHICLE_MODEL_BINDING_INVALID_VEHICLE_ID);

	custom_vehicle_vehicle_model_binding_v1 found {};
	assert(custom_vehicle_native_vehicle_model_backend_find_vehicle_binding(
		backend, 77, &found) == 1);
	assert(found.generation == first.generation);

	custom_vehicle_vehicle_model_binding_v1 stale = first;
	--stale.generation;
	assert(custom_vehicle_native_vehicle_model_backend_release_vehicle_binding(
		backend, &stale) == 0);
	assert(custom_vehicle_native_vehicle_model_backend_release_vehicle_binding(
		backend, &first) == 1);
	assert(custom_vehicle_native_vehicle_model_backend_release_vehicle_binding(
		backend, &first) == 0);

	custom_vehicle_vehicle_model_binding_v1 replacement {};
	assert(custom_vehicle_native_vehicle_model_backend_bind_vehicle(
		backend, 77, 30001, &replacement) == CUSTOM_VEHICLE_MODEL_BINDING_BOUND);
	assert(replacement.generation > first.generation);
	assert(custom_vehicle_native_vehicle_model_backend_release_vehicle_binding(
		backend, &first) == 0);

	assert(custom_vehicle_native_vehicle_model_backend_retire(
		backend, 30001, &callbacks) == CUSTOM_VEHICLE_NATIVE_MODEL_IN_USE);
	assert(custom_vehicle_native_vehicle_model_backend_retry_cleanup(
		backend, 30001, &callbacks) == CUSTOM_VEHICLE_NATIVE_MODEL_IN_USE);
	assert(custom_vehicle_native_vehicle_model_backend_release_vehicle_use(
		backend, 30001, slot) == 0); // Anonymous release cannot consume a bound lease.
	assert(custom_vehicle_native_vehicle_model_backend_release_vehicle_binding(
		backend, &replacement) == 1);
	assert(custom_vehicle_native_vehicle_model_backend_retry_cleanup(
		backend, 30001, &callbacks) == CUSTOM_VEHICLE_NATIVE_MODEL_RELEASED);
	assert(custom_vehicle_native_vehicle_model_backend_size(backend) == 0);
	assert(custom_vehicle_native_vehicle_model_backend_find_vehicle_binding(
		backend, 77, &found) == 0);
	assert(backendSlotIsVacant(&fixture, slot));

	assert(custom_vehicle_native_vehicle_model_backend_destroy(backend) == 1);
}

void testNativeVehicleModelLifecycleCAbi()
{
	NativeVehicleBackendFixture fixture;
	const auto callbacks = makeNativeVehicleCAbiCallbacks(fixture);
	auto* backend = custom_vehicle_native_vehicle_model_backend_create(612, 615);
	assert(backend != nullptr);
	assert(custom_vehicle_native_vehicle_model_backend_has_valid_range(backend) == 1);
	assert(custom_vehicle_native_vehicle_model_backend_size(backend) == 0);

	custom_vehicle_model_request_v1 model {};
	model.pool_id = 0;
	model.model_count = 1;
	model.type = static_cast<std::uint8_t>(ModelType::Vehicle);
	model.virtual_world = -1;
	model.base_id = 400;
	model.new_id = 30001;
	model.dff_checksum = 0x10203040u;
	model.txd_checksum = 0x50607080u;
	model.dff_size = 4096;
	model.txd_size = 2048;

	std::int32_t slot = -77;
	assert(custom_vehicle_native_vehicle_model_backend_activate(
		backend, &model, 0, &callbacks, &slot)
		== CUSTOM_VEHICLE_NATIVE_MODEL_ASSETS_PENDING);
	assert(slot == -77); // Non-success statuses never publish a slot.
	assert(custom_vehicle_native_vehicle_model_backend_activate(
		backend, &model, 1, nullptr, &slot)
		== CUSTOM_VEHICLE_NATIVE_MODEL_MISSING_OPERATIONS);

	fixture.pendingAssetPolls = 1;
	assert(custom_vehicle_native_vehicle_model_backend_activate(
		backend, &model, 1, &callbacks, &slot)
		== CUSTOM_VEHICLE_NATIVE_MODEL_NATIVE_ASSETS_PENDING);
	assert(slot == -77);
	assert(custom_vehicle_native_vehicle_model_backend_size(backend) == 1);
	assert(custom_vehicle_native_vehicle_model_backend_destroy(backend) == 0);
	assert(custom_vehicle_native_vehicle_model_backend_size(backend) == 1); // Live state remains owned.
	std::int32_t pendingSlot = -1;
	assert(custom_vehicle_native_vehicle_model_backend_find(backend, 30001, &pendingSlot) == 1);
	assert(pendingSlot == 613);
	assert(custom_vehicle_native_vehicle_model_backend_is_active(backend, 30001) == 0);

	assert(custom_vehicle_native_vehicle_model_backend_activate(
		backend, &model, 1, &callbacks, &slot)
		== CUSTOM_VEHICLE_NATIVE_MODEL_ACTIVATED);
	assert(slot == 613);
	assert(custom_vehicle_native_vehicle_model_backend_size(backend) == 1);
	assert(custom_vehicle_native_vehicle_model_backend_is_active(backend, 30001) == 1);
	std::int32_t foundSlot = -1;
	assert(custom_vehicle_native_vehicle_model_backend_find(backend, 30001, &foundSlot) == 1);
	assert(foundSlot == slot);
	assert(fixture.callbackOrder == std::vector<char>({ 'C', 'L', 'L', 'A' }));

	custom_vehicle_model_request_v1 changedModel = model;
	changedModel.dff_checksum++;
	std::int32_t unchangedSlot = -88;
	assert(custom_vehicle_native_vehicle_model_backend_activate(
		backend, &changedModel, 1, &callbacks, &unchangedSlot)
		== CUSTOM_VEHICLE_NATIVE_MODEL_REPLACEMENT_REQUIRED);
	assert(unchangedSlot == -88);
	assert(custom_vehicle_native_vehicle_model_backend_is_active(backend, 30001) == 1);

	std::int32_t retainedSlot = -1;
	assert(custom_vehicle_native_vehicle_model_backend_retain_for_vehicle(
		backend, 30001, &retainedSlot) == 1);
	assert(retainedSlot == slot);
	assert(custom_vehicle_native_vehicle_model_backend_retain_for_vehicle(
		backend, 30001, &retainedSlot) == 1);
	assert(custom_vehicle_native_vehicle_model_backend_retire(backend, 30001, &callbacks)
		== CUSTOM_VEHICLE_NATIVE_MODEL_IN_USE);
	assert(custom_vehicle_native_vehicle_model_backend_is_active(backend, 30001) == 0);
	assert(custom_vehicle_native_vehicle_model_backend_retain_for_vehicle(
		backend, 30001, &retainedSlot) == 0);
	assert(custom_vehicle_native_vehicle_model_backend_release_vehicle_use(
		backend, 30001, slot) == 1);
	assert(custom_vehicle_native_vehicle_model_backend_retry_cleanup(
		backend, 30001, &callbacks) == CUSTOM_VEHICLE_NATIVE_MODEL_IN_USE);
	assert(custom_vehicle_native_vehicle_model_backend_release_vehicle_use(
		backend, 30001, slot) == 1);

	fixture.failDestroyOnce = true;
	assert(custom_vehicle_native_vehicle_model_backend_retry_cleanup(
		backend, 30001, &callbacks) == CUSTOM_VEHICLE_NATIVE_MODEL_CLEANUP_PENDING);
	assert(custom_vehicle_native_vehicle_model_backend_size(backend) == 1);
	assert(custom_vehicle_native_vehicle_model_backend_find(backend, 30001, &foundSlot) == 1);
	assert(foundSlot == slot);
	assert(custom_vehicle_native_vehicle_model_backend_retry_cleanup(
		backend, 30001, &callbacks) == CUSTOM_VEHICLE_NATIVE_MODEL_RELEASED);
	assert(custom_vehicle_native_vehicle_model_backend_size(backend) == 0);
	assert(custom_vehicle_native_vehicle_model_backend_find(backend, 30001, &foundSlot) == 0);
	assert(backendSlotIsVacant(&fixture, slot));
	assert(!fixture.modelInfoCreated && !fixture.assetsLoaded && !fixture.clumpAttached);

	assert(custom_vehicle_native_vehicle_model_backend_destroy(backend) == 1);
	assert(custom_vehicle_native_vehicle_model_backend_destroy(nullptr) == 0);
}

void testNativeVehicleModelLifecycle()
{
	using namespace custom_vehicle::native;

	NativeVehicleBackendFixture fixture;
	VehicleModelLifecycleController backend(612, 615);
	const auto operations = makeNativeVehicleOperations();
	const ModelRequest model = makeNativeVehicleModelRequest(30001, 400);

	assert(backend.hasValidSlotRange());
	std::int32_t unusedSlot = -1;
	assert(backend.activate(model, false, operations, &fixture, unusedSlot)
		== VehicleModelBackendResult::AssetsPending);
	assert(backend.size() == 0);
	assert(backend.activate(model, true, VehicleModelNativeOperations {}, &fixture, unusedSlot)
		== VehicleModelBackendResult::MissingOperations);

	fixture.pendingAssetPolls = 1;
	std::int32_t nativeSlot = -1;
	assert(backend.activate(model, true, operations, &fixture, nativeSlot)
		== VehicleModelBackendResult::NativeAssetsPending);
	assert(nativeSlot == -1);
	assert(backend.size() == 1);
	assert(!backend.isActive(30001));
	assert(fixture.modelInfoCreated && !fixture.assetsLoaded && !fixture.clumpAttached);
	assert(fixture.callbackOrder == std::vector<char>({ 'C', 'L' }));

	assert(backend.activate(model, true, operations, &fixture, nativeSlot)
		== VehicleModelBackendResult::Activated);
	assert(nativeSlot == 613);
	assert(backend.isActive(30001));
	assert(backend.size() == 1);
	assert(fixture.modelInfoCreated && fixture.assetsLoaded && fixture.clumpAttached);
	assert(fixture.lastBaseModelId == 400 && fixture.lastCustomModelId == 30001);
	assert(fixture.callbackOrder == std::vector<char>({ 'C', 'L', 'L', 'A' }));

	std::int32_t duplicateSlot = -1;
	const std::size_t callbackCount = fixture.callbackOrder.size();
	assert(backend.activate(model, true, operations, &fixture, duplicateSlot)
		== VehicleModelBackendResult::AlreadyActive);
	assert(duplicateSlot == nativeSlot);
	assert(fixture.callbackOrder.size() == callbackCount);

	std::int32_t retainedSlot = -1;
	assert(backend.retainForVehicle(30001, retainedSlot) && retainedSlot == nativeSlot);
	assert(backend.retainForVehicle(30001, retainedSlot) && retainedSlot == nativeSlot);
	assert(backend.retire(30001, operations, &fixture)
		== VehicleModelBackendResult::ModelInUse);
	assert(!backend.isActive(30001));
	assert(!backend.retainForVehicle(30001, retainedSlot));
	assert(backend.releaseVehicleUse(30001, nativeSlot));
	assert(backend.retryRetiredCleanup(30001, operations, &fixture)
		== VehicleModelBackendResult::ModelInUse);
	assert(backend.releaseVehicleUse(30001, nativeSlot));

	fixture.failDestroyOnce = true;
	assert(backend.retryRetiredCleanup(30001, operations, &fixture)
		== VehicleModelBackendResult::CleanupPending);
	assert(backend.size() == 1); // A failed teardown never releases the slot.
	assert(backend.find(30001, duplicateSlot) && duplicateSlot == nativeSlot);
	assert(std::find(fixture.occupiedSlots.begin(), fixture.occupiedSlots.end(), nativeSlot)
		!= fixture.occupiedSlots.end());

	assert(backend.retryRetiredCleanup(30001, operations, &fixture)
		== VehicleModelBackendResult::Released);
	assert(backend.size() == 0);
	assert(!backend.find(30001, duplicateSlot));
	assert(!fixture.modelInfoCreated && !fixture.assetsLoaded && !fixture.clumpAttached);
	assert(backendSlotIsVacant(&fixture, nativeSlot));

	// An attach failure rolls back loaded assets and model-info. If cleanup itself
	// fails once, the slot remains retired until a later explicit retry succeeds.
	fixture.failAttach = true;
	fixture.failDestroyOnce = true;
	const ModelRequest secondModel = makeNativeVehicleModelRequest(30002, 401);
	std::int32_t secondNativeSlot = -1;
	assert(backend.activate(secondModel, true, operations, &fixture, secondNativeSlot)
		== VehicleModelBackendResult::CleanupPending);
	assert(secondNativeSlot == -1); // Failed activation never publishes a slot.
	assert(backend.size() == 1);
	assert(!backend.isActive(30002));
	assert(backend.find(30002, duplicateSlot));
	assert(backendSlotIsVacant(&fixture, duplicateSlot) == false);
	assert(backend.retryRetiredCleanup(30002, operations, &fixture)
		== VehicleModelBackendResult::Released);
	assert(backend.size() == 0);
	assert(backendSlotIsVacant(&fixture, duplicateSlot));
	assert(!fixture.modelInfoCreated && !fixture.assetsLoaded && !fixture.clumpAttached);

	// A native streaming failure rolls back the created model-info record.
	fixture.failLoad = true;
	const ModelRequest thirdModel = makeNativeVehicleModelRequest(30003, 402);
	std::int32_t thirdSlot = -1;
	assert(backend.activate(thirdModel, true, operations, &fixture, thirdSlot)
		== VehicleModelBackendResult::LoadAssetsFailed);
	assert(backend.size() == 0);
	assert(thirdSlot == -1);
	assert(!fixture.modelInfoCreated && !fixture.assetsLoaded && !fixture.clumpAttached);
	fixture.failLoad = false;

	// A model-info creation failure also leaves no reserved slot behind.
	fixture.failCreate = true;
	const ModelRequest fourthModel = makeNativeVehicleModelRequest(30004, 403);
	assert(backend.activate(fourthModel, true, operations, &fixture, thirdSlot)
		== VehicleModelBackendResult::CreateModelInfoFailed);
	assert(backend.size() == 0);
	assert(!fixture.modelInfoCreated && !fixture.assetsLoaded && !fixture.clumpAttached);
	fixture.failCreate = false;

	// Every slot may be occupied by native models; fail closed without mutation.
	VehicleModelLifecycleController exhausted(612, 612);
	assert(exhausted.activate(
		makeNativeVehicleModelRequest(30005, 404),
		true,
		operations,
		&fixture,
		thirdSlot) == VehicleModelBackendResult::NoFreeSlot);
	assert(exhausted.size() == 0);
}

void testNativeVehicleModelSlotRegistryCAbi()
{
	CAbiNativeSlotState nativeState { { 612 } };
	auto* registry = custom_vehicle_native_model_slot_registry_create(612, 613);
	assert(registry != nullptr);
	assert(custom_vehicle_native_model_slot_registry_has_valid_range(registry) == 1);
	assert(custom_vehicle_native_model_slot_registry_size(registry) == 0);

	std::int32_t nativeSlot = -1;
	assert(custom_vehicle_native_model_slot_registry_reserve(
		registry, 30001, cAbiNativeModelSlotIsVacant, &nativeState, &nativeSlot)
		== CUSTOM_VEHICLE_MODEL_SLOT_RESERVED);
	assert(nativeSlot == 613);
	assert(custom_vehicle_native_model_slot_registry_size(registry) == 1);
	assert(custom_vehicle_native_model_slot_registry_destroy(registry) == 0);
	assert(custom_vehicle_native_model_slot_registry_size(registry) == 1); // Refuse to drop a live mapping.

	std::int32_t repeatedSlot = -1;
	assert(custom_vehicle_native_model_slot_registry_reserve(
		registry, 30001, cAbiNativeModelSlotIsVacant, &nativeState, &repeatedSlot)
		== CUSTOM_VEHICLE_MODEL_SLOT_ALREADY_RESERVED);
	assert(repeatedSlot == nativeSlot);
	assert(custom_vehicle_native_model_slot_registry_retain(registry, 30001, &repeatedSlot) == 1);
	assert(repeatedSlot == nativeSlot);
	assert(custom_vehicle_native_model_slot_registry_retire(registry, 30001) == 1);
	assert(custom_vehicle_native_model_slot_registry_retain(registry, 30001, &repeatedSlot) == 0);
	assert(custom_vehicle_native_model_slot_registry_can_release(registry, 30001) == 0);
	assert(custom_vehicle_native_model_slot_registry_release_use(registry, 30001, nativeSlot) == 1);
	assert(custom_vehicle_native_model_slot_registry_can_release(registry, 30001) == 1);

	nativeState.occupiedSlots.push_back(nativeSlot);
	assert(custom_vehicle_native_model_slot_registry_release(
		registry, 30001, nativeSlot, cAbiNativeModelSlotIsVacant, &nativeState)
		== CUSTOM_VEHICLE_MODEL_SLOT_STILL_OCCUPIED);
	nativeState.occupiedSlots.pop_back();
	assert(custom_vehicle_native_model_slot_registry_release(
		registry, 30001, nativeSlot, cAbiNativeModelSlotIsVacant, &nativeState)
		== CUSTOM_VEHICLE_MODEL_SLOT_RELEASED);
	assert(custom_vehicle_native_model_slot_registry_find(registry, 30001, &repeatedSlot) == 0);
	assert(custom_vehicle_native_model_slot_registry_size(registry) == 0);
	assert(custom_vehicle_native_model_slot_registry_reserve(
		registry, 30002, nullptr, &nativeState, &nativeSlot)
		== CUSTOM_VEHICLE_MODEL_SLOT_MISSING_VACANCY_QUERY);
	assert(custom_vehicle_native_model_slot_registry_destroy(registry) == 1);
	assert(custom_vehicle_native_model_slot_registry_destroy(nullptr) == 0);

	auto* invalidRange = custom_vehicle_native_model_slot_registry_create(611, 19999);
	assert(invalidRange != nullptr);
	assert(custom_vehicle_native_model_slot_registry_has_valid_range(invalidRange) == 0);
	assert(custom_vehicle_native_model_slot_registry_reserve(
		invalidRange, 30003, cAbiNativeModelSlotIsVacant, &nativeState, &nativeSlot)
		== CUSTOM_VEHICLE_MODEL_SLOT_INVALID_RANGE);
	assert(custom_vehicle_native_model_slot_registry_destroy(invalidRange) == 1);
}

void testNativeVehicleModelSlotRegistry()
{
	using namespace custom_vehicle::native;

	NativeSlotVacancyState nativeState { { 612, 613 } };
	VehicleModelSlotRegistry registry(612, 615);
	assert(registry.hasValidRange());
	assert(registry.firstSlot() == 612);
	assert(registry.lastSlot() == 615);
	assert(registry.size() == 0);

	std::int32_t nativeSlot = -1;
	assert(registry.reserve(30001, isNativeModelSlotVacant, &nativeState, nativeSlot)
		== ModelSlotReserveResult::Reserved);
	assert(nativeSlot == 614); // Occupied GTA slots are skipped.
	assert(registry.size() == 1);

	std::int32_t repeatedSlot = -1;
	assert(registry.reserve(30001, isNativeModelSlotVacant, &nativeState, repeatedSlot)
		== ModelSlotReserveResult::AlreadyReserved);
	assert(repeatedSlot == nativeSlot); // Manifest replay is idempotent.

	std::int32_t secondSlot = -1;
	assert(registry.reserve(30002, isNativeModelSlotVacant, &nativeState, secondSlot)
		== ModelSlotReserveResult::Reserved);
	assert(secondSlot == 615);
	assert(secondSlot != nativeSlot);

	assert(registry.reserve(30003, isNativeModelSlotVacant, &nativeState, repeatedSlot)
		== ModelSlotReserveResult::NoFreeSlot);
	assert(registry.reserve(30000, isNativeModelSlotVacant, &nativeState, repeatedSlot)
		== ModelSlotReserveResult::InvalidCustomModelId);
	assert(registry.reserve(32768, isNativeModelSlotVacant, &nativeState, repeatedSlot)
		== ModelSlotReserveResult::InvalidCustomModelId);
	assert(registry.reserve(30003, nullptr, &nativeState, repeatedSlot)
		== ModelSlotReserveResult::MissingVacancyQuery);

	std::int32_t retainedSlot = -1;
	assert(registry.retain(30001, retainedSlot));
	assert(retainedSlot == nativeSlot);
	assert(registry.retain(30001, retainedSlot)); // Two vehicles use this model.
	assert(registry.retire(30001));
	assert(registry.retire(30001)); // Retirement is idempotent.
	assert(!registry.retain(30001, retainedSlot)); // No new vehicles after retirement.
	assert(!registry.canRelease(30001));
	assert(!registry.releaseUse(30001, secondSlot)); // A mismatched slot cannot drop a lease.
	assert(registry.releaseUse(30001, nativeSlot));
	assert(!registry.canRelease(30001));
	assert(registry.releaseUse(30001, nativeSlot));
	assert(registry.canRelease(30001));

	assert(registry.release(30001, secondSlot, isNativeModelSlotVacant, &nativeState)
		== ModelSlotReleaseResult::SlotMismatch);
	assert(registry.release(30001, nativeSlot, nullptr, &nativeState)
		== ModelSlotReleaseResult::MissingVacancyQuery);
	assert(registry.find(30001, repeatedSlot) && repeatedSlot == nativeSlot);

	// Native teardown clears the model-info pointer before the mapping is reusable.
	nativeState.occupiedSlots.push_back(nativeSlot);
	assert(registry.release(30001, nativeSlot, isNativeModelSlotVacant, &nativeState)
		== ModelSlotReleaseResult::NativeSlotStillOccupied);
	nativeState.occupiedSlots.pop_back();
	assert(registry.release(30001, nativeSlot, isNativeModelSlotVacant, &nativeState)
		== ModelSlotReleaseResult::Released);
	assert(!registry.find(30001, repeatedSlot));
	assert(registry.size() == 1); // Model 30002 still owns its slot.

	assert(registry.reserve(30003, isNativeModelSlotVacant, &nativeState, repeatedSlot)
		== ModelSlotReserveResult::Reserved);
	assert(repeatedSlot == nativeSlot); // A cleared slot can be reused safely.

	VehicleModelSlotRegistry invalidRange(611, 19999);
	assert(!invalidRange.hasValidRange());
	assert(invalidRange.reserve(30004, isNativeModelSlotVacant, &nativeState, repeatedSlot)
		== ModelSlotReserveResult::InvalidSlotRange);
	assert(invalidRange.size() == 0);
}

custom_vehicle::native::VehicleStreamRequest makePendingVehicleStreamRequest(
	std::uint16_t vehicleId,
	std::int32_t customModelId = 30001,
	std::int32_t baseModelId = 400,
	float x = 1.0f)
{
	custom_vehicle::native::VehicleStreamRequest request;
	request.vehicle.vehicleId = vehicleId;
	request.vehicle.modelId = customModelId;
	request.vehicle.position = { x, 2.0f, 3.0f };
	request.vehicle.angle = 90.0f;
	request.vehicle.colour1 = 6;
	request.vehicle.colour2 = 7;
	request.vehicle.health = 950.0f;
	request.vehicle.interior = 0;
	request.vehicle.siren = 1;
	request.vehicle.paintjob = 2;
	request.baseModelId = baseModelId;
	return request;
}

void testVehicleStreamQueue()
{
	using custom_vehicle::native::VehicleStreamQueue;
	using custom_vehicle::native::VehicleStreamQueueResult;

	VehicleStreamQueue queue;
	assert(queue.size() == 0);

	auto first = makePendingVehicleStreamRequest(1, 30001, 400, 1.0f);
	assert(queue.enqueue(first) == VehicleStreamQueueResult::Queued);
	assert(queue.size() == 1);

	// A newer stream-in for the same vehicle replaces pending values in place.
	auto updated = makePendingVehicleStreamRequest(1, 30002, 411, 42.5f);
	assert(queue.enqueue(updated) == VehicleStreamQueueResult::Updated);
	assert(queue.size() == 1);

	auto second = makePendingVehicleStreamRequest(2, 30001, 400, 99.0f);
	assert(queue.enqueue(second) == VehicleStreamQueueResult::Queued);
	assert(queue.size() == 2);

	custom_vehicle::native::VehicleStreamRequest popped;
	assert(queue.pop(popped));
	assert(popped.vehicle.vehicleId == 1);
	assert(popped.vehicle.modelId == 30002);
	assert(popped.baseModelId == 411);
	assert(popped.vehicle.position[0] == 42.5f);
	assert(queue.pop(popped));
	assert(popped.vehicle.vehicleId == 2);
	assert(popped.vehicle.position[0] == 99.0f);
	assert(queue.size() == 0);
	assert(!queue.pop(popped));

	// Cancel removes a not-yet-consumed stream-in, so a later slot reuse is clean.
	assert(queue.enqueue(first) == VehicleStreamQueueResult::Queued);
	assert(queue.enqueue(second) == VehicleStreamQueueResult::Queued);
	assert(queue.cancel(1));
	assert(!queue.cancel(1));
	assert(queue.size() == 1);
	assert(queue.pop(popped) && popped.vehicle.vehicleId == 2);
	assert(!queue.pop(popped));

	assert(queue.enqueue(makePendingVehicleStreamRequest(0))
		== VehicleStreamQueueResult::InvalidVehicleId);
	assert(queue.enqueue(makePendingVehicleStreamRequest(2000))
		== VehicleStreamQueueResult::InvalidVehicleId);
	assert(queue.enqueue(makePendingVehicleStreamRequest(3, 411))
		== VehicleStreamQueueResult::InvalidCustomModelId);
	assert(queue.enqueue(makePendingVehicleStreamRequest(3, 30001, 537))
		== VehicleStreamQueueResult::InvalidBaseModelId);
	auto invalidData = makePendingVehicleStreamRequest(3);
	invalidData.vehicle.angle = std::numeric_limits<float>::infinity();
	assert(queue.enqueue(invalidData) == VehicleStreamQueueResult::InvalidVehicleData);

	// Every server vehicle ID has one bounded pending slot; a duplicate still updates at capacity.
	for (std::uint16_t id = 1; id < custom_vehicle::native::VehicleStreamQueueVehicleIdLimit; ++id)
	{
		assert(queue.enqueue(makePendingVehicleStreamRequest(id))
			== VehicleStreamQueueResult::Queued);
	}
	assert(queue.size() == custom_vehicle::native::VehicleStreamQueueCapacity);
	assert(queue.enqueue(makePendingVehicleStreamRequest(1, 30002, 411, 7.0f))
		== VehicleStreamQueueResult::Updated);
	assert(queue.enqueue(makePendingVehicleStreamRequest(2000))
		== VehicleStreamQueueResult::InvalidVehicleId);
	assert(queue.enqueue(makePendingVehicleStreamRequest(1))
		== VehicleStreamQueueResult::Updated);
	assert(queue.size() == custom_vehicle::native::VehicleStreamQueueCapacity);
	queue.clear();
	assert(queue.size() == 0);
	assert(!queue.pop(popped));
}

void testVehicleStreamQueueCAbi()
{
	auto* queue = custom_vehicle_pending_vehicle_queue_create();
	assert(queue != nullptr);
	assert(custom_vehicle_pending_vehicle_queue_size(queue) == 0);

	custom_vehicle_stream_in_vehicle_v1 request {};
	request.vehicle_id = 17;
	request.custom_model_id = 30001;
	request.base_model_id = 400;
	request.position[0] = 10.0f;
	request.position[1] = 20.0f;
	request.position[2] = 30.0f;
	request.angle = 180.0f;
	request.health = 800.0f;
	request.paintjob = 2;
	assert(custom_vehicle_pending_vehicle_queue_enqueue(queue, &request)
		== CUSTOM_VEHICLE_STREAM_QUEUE_QUEUED);
	assert(custom_vehicle_pending_vehicle_queue_size(queue) == 1);

	custom_vehicle_stream_in_vehicle_v1 updated = request;
	updated.custom_model_id = 30002;
	updated.base_model_id = 411;
	updated.position[0] = 77.0f;
	assert(custom_vehicle_pending_vehicle_queue_enqueue(queue, &updated)
		== CUSTOM_VEHICLE_STREAM_QUEUE_UPDATED);
	assert(custom_vehicle_pending_vehicle_queue_size(queue) == 1);
	assert(custom_vehicle_pending_vehicle_queue_destroy(queue) == 0);

	custom_vehicle_stream_in_vehicle_v1 output {};
	assert(custom_vehicle_pending_vehicle_queue_pop(queue, &output) == 1);
	assert(output.vehicle_id == 17);
	assert(output.custom_model_id == 30002);
	assert(output.base_model_id == 411);
	assert(output.position[0] == 77.0f);
	assert(output.health == 800.0f);
	assert(custom_vehicle_pending_vehicle_queue_pop(queue, &output) == 0);

	assert(custom_vehicle_pending_vehicle_queue_enqueue(queue, &request)
		== CUSTOM_VEHICLE_STREAM_QUEUE_QUEUED);
	assert(custom_vehicle_pending_vehicle_queue_cancel(queue, 17) == 1);
	assert(custom_vehicle_pending_vehicle_queue_cancel(queue, 17) == 0);
	assert(custom_vehicle_pending_vehicle_queue_size(queue) == 0);
	assert(custom_vehicle_pending_vehicle_queue_destroy(queue) == 1);
	assert(custom_vehicle_pending_vehicle_queue_destroy(nullptr) == 0);

	queue = custom_vehicle_pending_vehicle_queue_create();
	assert(queue != nullptr);
	request.vehicle_id = 0;
	assert(custom_vehicle_pending_vehicle_queue_enqueue(queue, &request)
		== CUSTOM_VEHICLE_STREAM_QUEUE_INVALID_VEHICLE_ID);
	request.vehicle_id = 18;
	request.custom_model_id = 411;
	assert(custom_vehicle_pending_vehicle_queue_enqueue(queue, &request)
		== CUSTOM_VEHICLE_STREAM_QUEUE_INVALID_CUSTOM_MODEL);
	request.custom_model_id = 30001;
	request.base_model_id = 537;
	assert(custom_vehicle_pending_vehicle_queue_enqueue(queue, &request)
		== CUSTOM_VEHICLE_STREAM_QUEUE_INVALID_BASE_MODEL);
	assert(custom_vehicle_pending_vehicle_queue_pop(queue, &output) == 0);
	custom_vehicle_pending_vehicle_queue_clear(queue);
	assert(custom_vehicle_pending_vehicle_queue_destroy(queue) == 1);
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8));
	bytes.push_back(static_cast<std::uint8_t>(value >> 16));
	bytes.push_back(static_cast<std::uint8_t>(value >> 24));
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendFloatLittleEndian(std::vector<std::uint8_t>& bytes, float value)
{
	std::uint32_t raw = 0;
	static_assert(sizeof(raw) == sizeof(value), "32-bit float widths must match");
	std::memcpy(&raw, &value, sizeof(raw));
	appendU32(bytes, raw);
}

std::vector<std::uint8_t> makeStreamInVehicle(std::int32_t modelId = 30001)
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve(StreamInVehicleWireSize);
	appendU16(bytes, 1234);             // Vehicle pool ID.
	appendU32(bytes, static_cast<std::uint32_t>(modelId)); // Standard or custom vehicle model ID.
	appendFloatLittleEndian(bytes, 1.25f);
	appendFloatLittleEndian(bytes, -2.5f);
	appendFloatLittleEndian(bytes, 3.75f);
	appendFloatLittleEndian(bytes, 90.0f);
	bytes.push_back(6);                  // Primary colour.
	bytes.push_back(7);                  // Secondary colour.
	appendFloatLittleEndian(bytes, 875.5f);
	bytes.push_back(2);                  // Interior.
	appendU32(bytes, 0x11223344u);       // Door damage.
	appendU32(bytes, 0x55667788u);       // Panel damage.
	bytes.push_back(0x12);               // Light damage.
	bytes.push_back(0x34);               // Tyre damage.
	bytes.push_back(1);                  // Siren.
	for (std::size_t index = 0; index < StreamInVehicleModCount; ++index)
	{
		bytes.push_back(static_cast<std::uint8_t>(index + 1));
	}
	bytes.push_back(3);                  // Paintjob.
	appendU32(bytes, 0xAABBCCDDu);       // Body colour 1.
	appendU32(bytes, 0x10203040u);       // Body colour 2.
	return bytes;
}

void appendI32(std::vector<std::uint8_t>& bytes, std::int32_t value)
{
	appendU32(bytes, static_cast<std::uint32_t>(value));
}

std::vector<std::uint8_t> makeVehicleRequest(
	std::uint32_t dffChecksum = 0x11223344u,
	std::uint32_t txdChecksum = 0x55667788u,
	std::uint32_t dffSize = 512,
	std::uint32_t txdSize = 1024,
	std::uint32_t poolId = 0,
	std::uint32_t modelCount = 1,
	std::int32_t modelId = 30001)
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve(ModelRequestWireSize);
	appendU32(bytes, poolId);        // Position in the shared model pool.
	appendU32(bytes, modelCount);    // Total entries in this manifest.
	bytes.push_back(3);              // Experimental vehicle model type.
	appendI32(bytes, -1);            // Global virtual world.
	appendI32(bytes, 400);           // Base vehicle model.
	appendI32(bytes, modelId);       // Custom vehicle model ID.
	appendU32(bytes, dffChecksum);   // DFF checksum.
	appendU32(bytes, txdChecksum);   // TXD checksum.
	appendU32(bytes, dffSize);       // DFF size.
	appendU32(bytes, txdSize);       // TXD size.
	bytes.push_back(0);              // Time-on hour.
	bytes.push_back(0);              // Time-off hour.
	return bytes;
}

std::vector<std::uint8_t> makeDownloadUrl(AssetType type, std::uint32_t checksum, std::string_view url)
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve(DownloadUrlHeaderSize + url.size());
	bytes.push_back(ModelUrlTransportMarker);
	bytes.push_back(static_cast<std::uint8_t>(type));
	appendU32(bytes, checksum);
	bytes.push_back(static_cast<std::uint8_t>(url.size()));
	bytes.insert(bytes.end(), url.begin(), url.end());
	return bytes;
}

void testRpcIdIsolation()
{
	static_assert(static_cast<std::uint8_t>(RpcId::ModelRequest) == 220);
	static_assert(static_cast<std::uint8_t>(RpcId::ClientSupportsCustomVehicleModels) == 221);
	static_assert(static_cast<std::uint8_t>(RpcId::ServerSupportsCustomVehicleModels) == 222);
	static_assert(static_cast<std::uint8_t>(RpcId::RequestDff) == 223);
	static_assert(static_cast<std::uint8_t>(RpcId::RequestTxd) == 224);
	static_assert(static_cast<std::uint8_t>(RpcId::ModelUrl) == 225);
	static_assert(static_cast<std::uint8_t>(RpcId::FinishDownload) == 226);
	static_assert(static_cast<std::uint8_t>(RpcId::DownloadCompleted) == 227);

	// These native SA-MP/0.3.DL IDs must never be claimed by the extension.
	constexpr std::array<std::uint8_t, 10> nativeRpcIds = {
		179, 181, 182, 183, 184, 185, 186, 187, 188, 189
	};
	for (const std::uint8_t nativeRpc : nativeRpcIds)
	{
		assert(nativeRpc != static_cast<std::uint8_t>(RpcId::ModelRequest));
		assert(nativeRpc != static_cast<std::uint8_t>(RpcId::ClientSupportsCustomVehicleModels));
		assert(nativeRpc != static_cast<std::uint8_t>(RpcId::ServerSupportsCustomVehicleModels));
		assert(nativeRpc != static_cast<std::uint8_t>(RpcId::RequestDff));
		assert(nativeRpc != static_cast<std::uint8_t>(RpcId::RequestTxd));
		assert(nativeRpc != static_cast<std::uint8_t>(RpcId::ModelUrl));
		assert(nativeRpc != static_cast<std::uint8_t>(RpcId::FinishDownload));
		assert(nativeRpc != static_cast<std::uint8_t>(RpcId::DownloadCompleted));
	}
}

void testManifestDecode()
{
	const auto bytes = makeVehicleRequest();
	ModelRequest request;
	DecodeError error = DecodeError::WrongSize;

	assert(decodeModelRequest(bytes.data(), bytes.size(), request, error));
	assert(error == DecodeError::None);
	assert(request.poolId == 0);
	assert(request.modelCount == 1);
	assert(request.type == ModelType::Vehicle);
	assert(request.virtualWorld == -1);
	assert(request.baseId == 400);
	assert(request.newId == 30001);
	assert(request.dffChecksum == 0x11223344u);
	assert(request.txdChecksum == 0x55667788u);
	assert(request.dffSize == 512);
	assert(request.txdSize == 1024);
	assert(validateVehicleModelRequest(request));
}

void testStreamInVehicleDecode()
{
	static_assert(StreamInVehicleWireSize == 40 + StreamInVehicleModCount + 1 + 4 + 4);
	const auto bytes = makeStreamInVehicle();
	StreamInVehicleData vehicle;
	DecodeError error = DecodeError::WrongSize;

	assert(bytes.size() == StreamInVehicleWireSize);
	assert(decodeStreamInVehicle(bytes.data(), bytes.size(), vehicle, error));
	assert(error == DecodeError::None);
	assert(vehicle.vehicleId == 1234);
	assert(vehicle.modelId == 30001);
	assert(vehicle.position[0] == 1.25f);
	assert(vehicle.position[1] == -2.5f);
	assert(vehicle.position[2] == 3.75f);
	assert(vehicle.angle == 90.0f);
	assert(vehicle.colour1 == 6);
	assert(vehicle.colour2 == 7);
	assert(vehicle.health == 875.5f);
	assert(vehicle.interior == 2);
	assert(vehicle.doorDamage == 0x11223344u);
	assert(vehicle.panelDamage == 0x55667788u);
	assert(vehicle.lightDamage == 0x12);
	assert(vehicle.tyreDamage == 0x34);
	assert(vehicle.siren == 1);
	for (std::size_t index = 0; index < StreamInVehicleModCount; ++index)
	{
		assert(vehicle.mods[index] == static_cast<std::uint8_t>(index + 1));
	}
	assert(vehicle.paintjob == 3);
	assert(static_cast<std::uint32_t>(vehicle.bodyColour1) == 0xAABBCCDDu);
	assert(vehicle.bodyColour2 == 0x10203040);

	// Native IDs remain valid, while values outside both valid ranges fail closed.
	auto standard = makeStreamInVehicle(411);
	assert(decodeStreamInVehicle(standard.data(), standard.size(), vehicle, error));
	assert(vehicle.modelId == 411);

	vehicle.modelId = 31234; // Failed decode must not partially overwrite output.
	auto invalid = makeStreamInVehicle(30000);
	assert(!decodeStreamInVehicle(invalid.data(), invalid.size(), vehicle, error));
	assert(error == DecodeError::InvalidVehicleModel);
	assert(vehicle.modelId == 31234);

	assert(!decodeStreamInVehicle(bytes.data(), bytes.size() - 1, vehicle, error));
	assert(error == DecodeError::WrongSize);
	assert(vehicle.modelId == 31234);
	assert(!decodeStreamInVehicle(nullptr, bytes.size(), vehicle, error));
	assert(error == DecodeError::NullBuffer);

	// A NaN in the first position coordinate is not accepted.
	auto nonFinite = bytes;
	nonFinite[6] = 0x00;
	nonFinite[7] = 0x00;
	nonFinite[8] = 0xC0;
	nonFinite[9] = 0x7F;
	assert(!decodeStreamInVehicle(nonFinite.data(), nonFinite.size(), vehicle, error));
	assert(error == DecodeError::InvalidVehicleData);
	assert(vehicle.modelId == 31234);
}

void testStreamInVehicleBaseRewrite()
{
	const auto original = makeStreamInVehicle(30001);
	std::array<std::uint8_t, StreamInVehicleWireSize> rewritten {};
	rewritten.fill(0xA5);
	std::size_t bytesWritten = 999;
	DecodeError error = DecodeError::WrongSize;

	assert(rewriteStreamInVehicleToBaseModel(
		original.data(),
		original.size(),
		30001,
		402,
		rewritten.data(),
		rewritten.size(),
		bytesWritten,
		error));
	assert(error == DecodeError::None);
	assert(bytesWritten == StreamInVehicleWireSize);

	// Only bytes 2-5 (the packed int32 model field) may differ.
	for (std::size_t index = 0; index < original.size(); ++index)
	{
		if (index >= StreamInVehicleModelFieldOffset
			&& index < StreamInVehicleModelFieldOffset + sizeof(std::int32_t))
		{
			continue;
		}
		assert(rewritten[index] == original[index]);
	}
	assert(rewritten[2] == 0x92); // 402 = 0x00000192, little-endian.
	assert(rewritten[3] == 0x01);
	assert(rewritten[4] == 0x00);
	assert(rewritten[5] == 0x00);

	StreamInVehicleData decoded;
	assert(decodeStreamInVehicle(
		rewritten.data(), rewritten.size(), decoded, error));
	assert(decoded.modelId == 402);
	assert(decoded.vehicleId == 1234);
	assert(decoded.position[0] == 1.25f);
	assert(decoded.health == 875.5f);
	assert(decoded.mods[13] == 14);
	assert(decoded.paintjob == 3);

	// The helper is also safe when the caller requests in-place rewriting.
	auto inPlace = original;
	assert(rewriteStreamInVehicleToBaseModel(
		inPlace.data(),
		inPlace.size(),
		30001,
		411,
		inPlace.data(),
		inPlace.size(),
		bytesWritten,
		error));
	assert(bytesWritten == inPlace.size());
	assert(inPlace[2] == 0x9B && inPlace[3] == 0x01);

	// Every rejected case leaves the output buffer and byte count unchanged.
	std::array<std::uint8_t, StreamInVehicleWireSize> untouched {};
	untouched.fill(0x5A);
	const auto sentinel = untouched;
	bytesWritten = 123;
	assert(!rewriteStreamInVehicleToBaseModel(
		original.data(), original.size(), 30002, 402,
		untouched.data(), untouched.size(), bytesWritten, error));
	assert(error == DecodeError::InvalidVehicleModel);
	assert(bytesWritten == 0);
	assert(untouched == sentinel); // Requested model must match packet ID.

	bytesWritten = 123;
	assert(!rewriteStreamInVehicleToBaseModel(
		original.data(), original.size(), 30001, 449,
		untouched.data(), untouched.size(), bytesWritten, error));
	assert(error == DecodeError::InvalidVehicleModel);
	assert(bytesWritten == 0);
	assert(untouched == sentinel); // Trains are not compatible base vehicles.

	const auto standard = makeStreamInVehicle(411);
	bytesWritten = 123;
	assert(!rewriteStreamInVehicleToBaseModel(
		standard.data(), standard.size(), 30001, 402,
		untouched.data(), untouched.size(), bytesWritten, error));
	assert(error == DecodeError::InvalidVehicleModel);
	assert(bytesWritten == 0);
	assert(untouched == sentinel); // Stock vehicles must remain untouched.

	bytesWritten = 123;
	assert(!rewriteStreamInVehicleToBaseModel(
		original.data(), original.size() - 1, 30001, 402,
		untouched.data(), untouched.size(), bytesWritten, error));
	assert(error == DecodeError::WrongSize);
	assert(bytesWritten == 0);
	assert(untouched == sentinel);

	bytesWritten = 123;
	assert(!rewriteStreamInVehicleToBaseModel(
		original.data(), original.size(), 30001, 402,
		untouched.data(), untouched.size() - 1, bytesWritten, error));
	assert(error == DecodeError::InvalidOutputBuffer);
	assert(bytesWritten == 0);
	assert(untouched == sentinel);
}

void testMalformedManifest()
{
	auto bytes = makeVehicleRequest();
	ModelRequest request;
	DecodeError error = DecodeError::None;

	assert(!decodeModelRequest(nullptr, bytes.size(), request, error));
	assert(error == DecodeError::NullBuffer);
	assert(!decodeModelRequest(bytes.data(), bytes.size() - 1, request, error));
	assert(error == DecodeError::WrongSize);

	bytes[8] = 99;
	assert(!decodeModelRequest(bytes.data(), bytes.size(), request, error));
	assert(error == DecodeError::InvalidModelType);
}

void testModelCountAndPoolIndexValidation()
{
	auto bytes = makeVehicleRequest();
	ModelRequest request;
	request.newId = 31234; // Failed decodes must not publish partial output.
	DecodeError error = DecodeError::None;

	// A signed -1 count must not become a huge unsigned pool capacity.
	bytes[4] = 0xFF;
	bytes[5] = 0xFF;
	bytes[6] = 0xFF;
	bytes[7] = 0xFF;
	assert(!decodeModelRequest(bytes.data(), bytes.size(), request, error));
	assert(error == DecodeError::InvalidModelCount);
	assert(request.newId == 31234);

	// A zero-entry manifest is invalid too.
	bytes[4] = 0;
	bytes[5] = 0;
	bytes[6] = 0;
	bytes[7] = 0;
	assert(!decodeModelRequest(bytes.data(), bytes.size(), request, error));
	assert(error == DecodeError::InvalidModelCount);
	assert(request.newId == 31234);

	// An entry index equal to the one-entry count is outside the pool.
	bytes[4] = 1;
	bytes[5] = 0;
	bytes[6] = 0;
	bytes[7] = 0;
	bytes[0] = 1;
	assert(!decodeModelRequest(bytes.data(), bytes.size(), request, error));
	assert(error == DecodeError::InvalidPoolIndex);
	assert(request.newId == 31234);
}

void testDownloadUrlCodec()
{
	const auto bytes = makeDownloadUrl(AssetType::Dff, 0xCBF43926u, "https://example.test/asset.dff");
	DownloadUrl url;
	DecodeError error = DecodeError::None;

	assert(decodeDownloadUrl(bytes.data(), bytes.size(), url, error));
	assert(error == DecodeError::None);
	assert(url.type == AssetType::Dff);
	assert(url.checksum == 0xCBF43926u);
	assert(url.url == "https://example.test/asset.dff");

	auto invalid = bytes;
	invalid[0] = 5;
	assert(!decodeDownloadUrl(invalid.data(), invalid.size(), url, error));
	assert(error == DecodeError::InvalidUrlMarker);

	invalid = bytes;
	invalid[1] = 3;
	assert(!decodeDownloadUrl(invalid.data(), invalid.size(), url, error));
	assert(error == DecodeError::InvalidAssetType);

	invalid = bytes;
	invalid[6]--;
	assert(!decodeDownloadUrl(invalid.data(), invalid.size(), url, error));
	assert(error == DecodeError::InvalidUrlLength);

	invalid = makeDownloadUrl(AssetType::Dff, 1, "file:///asset.dff");
	assert(!decodeDownloadUrl(invalid.data(), invalid.size(), url, error));
	assert(error == DecodeError::InvalidUrl);
}

void testAssetRequestEncoding()
{
	std::array<std::uint8_t, AssetRequestWireSize> payload {};
	std::size_t written = 0;
	RpcId rpcId = RpcId::ModelUrl;

	assert(encodeAssetRequest(AssetType::Dff, 0x12345678u, payload.data(), payload.size(), written, rpcId));
	assert(written == AssetRequestWireSize);
	assert(rpcId == RpcId::RequestDff);
	assert((payload == std::array<std::uint8_t, 4> { 0x78, 0x56, 0x34, 0x12 }));

	assert(encodeAssetRequest(AssetType::Txd, 0xAABBCCDDu, payload.data(), payload.size(), written, rpcId));
	assert(rpcId == RpcId::RequestTxd);
	assert((payload == std::array<std::uint8_t, 4> { 0xDD, 0xCC, 0xBB, 0xAA }));

	assert(!encodeAssetRequest(AssetType::Dff, 1, payload.data(), 3, written, rpcId));
	assert(written == 0);
}

void testAssetCompletionEncoding()
{
	std::array<std::uint8_t, AssetCompletionWireSize> payload {};
	std::size_t written = 0;

	assert(encodeAssetCompletion(AssetType::Dff, 0x12345678u,
		payload.data(), payload.size(), written));
	assert(written == AssetCompletionWireSize);
	assert((payload == std::array<std::uint8_t, 5> { 1, 0x78, 0x56, 0x34, 0x12 }));

	assert(encodeAssetCompletion(AssetType::Txd, 0xAABBCCDDu,
		payload.data(), payload.size(), written));
	assert((payload == std::array<std::uint8_t, 5> { 2, 0xDD, 0xCC, 0xBB, 0xAA }));

	assert(!encodeAssetCompletion(static_cast<AssetType>(0), 1,
		payload.data(), payload.size(), written));
	assert(written == 0);
	assert(!encodeAssetCompletion(AssetType::Dff, 1, payload.data(), 4, written));
	assert(written == 0);
}

void testVehicleValidation()
{
	auto bytes = makeVehicleRequest();
	ModelRequest request;
	DecodeError error = DecodeError::None;
	assert(decodeModelRequest(bytes.data(), bytes.size(), request, error));

	assert(isCustomVehicleModelId(30001));
	assert(isCustomVehicleModelId(32767));
	assert(!isCustomVehicleModelId(30000));
	assert(!isCustomVehicleModelId(32768));
	assert(isSupportedVehicleBaseId(400));
	assert(isSupportedVehicleBaseId(611));
	assert(!isSupportedVehicleBaseId(449));
	assert(!isSupportedVehicleBaseId(537));
	assert(!isSupportedVehicleBaseId(538));
	assert(!isSupportedVehicleBaseId(569));
	assert(!isSupportedVehicleBaseId(570));

	request.baseId = 537;
	assert(!validateVehicleModelRequest(request));
	request.baseId = 400;
	request.newId = 30000;
	assert(!validateVehicleModelRequest(request));
	request.newId = 30001;
	request.dffChecksum = request.txdChecksum;
	assert(!validateVehicleModelRequest(request));
}

void testSessionDownloadLifecycle()
{
	constexpr std::array<std::uint8_t, 9> dff = { '1','2','3','4','5','6','7','8','9' };
	constexpr std::array<std::uint8_t, 4> txd = { 't','x','d','!' };
	constexpr std::uint32_t dffCrc = 0xCBF43926u;
	const std::uint32_t txdCrc = calculateCrc32(txd.data(), txd.size());

	ClientSession session;
	assert(session.beginNegotiation());
	assert(session.capabilityState() == CapabilityState::AwaitingAcknowledgement);

	SessionError sessionError = SessionError::None;
	assert(session.acceptCapabilityAcknowledgement(nullptr, 0, sessionError));
	assert(session.capabilityState() == CapabilityState::Ready);

	auto manifest = makeVehicleRequest(dffCrc, txdCrc, static_cast<std::uint32_t>(dff.size()), static_cast<std::uint32_t>(txd.size()));
	DecodeError decodeError = DecodeError::None;
	assert(session.acceptModelManifest(manifest.data(), manifest.size(), decodeError, sessionError)
		== ManifestDisposition::AcceptedVehicle);
	assert(session.vehicleModels().size() == 1);
	assert(!session.isVehicleModelReady(30001));

	AssetRequest request;
	assert(session.nextAssetRequest(request));
	assert(request.type == AssetType::Dff);
	assert(request.checksum == dffCrc);
	assert(request.expectedSize == dff.size());

	auto urlPayload = makeDownloadUrl(AssetType::Dff, dffCrc, "https://example.test/model.dff");
	AssetDownload download;
	assert(session.acceptDownloadUrl(urlPayload.data(), urlPayload.size(), download, decodeError, sessionError));
	assert(download.expectedSize == dff.size());
	assert(download.url == "https://example.test/model.dff");
	assert(session.completeAsset(download.type, download.checksum, dff.data(), dff.size(), sessionError));
	assert(!session.isVehicleModelReady(30001)); // Download bytes alone are not enough; wait for server ACK.

	// Serialize downloads: the next file waits for vehicle ACK 227 after finish 226.
	assert(!session.nextAssetRequest(request));
	assert(session.acceptDownloadAcknowledgement(nullptr, 0, sessionError));
	assert(!session.isVehicleModelReady(30001)); // TXD is still missing.
	assert(session.nextAssetRequest(request));
	assert(request.type == AssetType::Txd);
	assert(request.checksum == txdCrc);

	urlPayload = makeDownloadUrl(AssetType::Txd, txdCrc, "https://example.test/model.txd");
	assert(session.acceptDownloadUrl(urlPayload.data(), urlPayload.size(), download, decodeError, sessionError));
	assert(session.completeAsset(download.type, download.checksum, txd.data(), txd.size(), sessionError));
	assert(!session.isVehicleModelReady(30001)); // Final RPC 227 acknowledgement is still pending.
	assert(session.acceptDownloadAcknowledgement(nullptr, 0, sessionError));
	assert(session.isVehicleModelReady(30001));
	assert(!session.isVehicleModelReady(39999));
	assert(!session.nextAssetRequest(request));
}

void testSessionRejectsMismatchedUrlAndCorruptFile()
{
	constexpr std::array<std::uint8_t, 4> dff = { 'd','f','f','!' };
	constexpr std::array<std::uint8_t, 4> txd = { 't','x','d','!' };
	const std::uint32_t dffCrc = calculateCrc32(dff.data(), dff.size());
	const std::uint32_t txdCrc = calculateCrc32(txd.data(), txd.size());

	ClientSession session;
	session.beginNegotiation();
	SessionError sessionError = SessionError::None;
	assert(session.acceptCapabilityAcknowledgement(nullptr, 0, sessionError));

	auto manifest = makeVehicleRequest(dffCrc, txdCrc, static_cast<std::uint32_t>(dff.size()), static_cast<std::uint32_t>(txd.size()));
	DecodeError decodeError = DecodeError::None;
	assert(session.acceptModelManifest(manifest.data(), manifest.size(), decodeError, sessionError)
		== ManifestDisposition::AcceptedVehicle);

	AssetRequest request;
	assert(session.nextAssetRequest(request));
	auto wrongUrl = makeDownloadUrl(AssetType::Txd, request.checksum, "https://example.test/wrong.txd");
	AssetDownload download;
	assert(!session.acceptDownloadUrl(wrongUrl.data(), wrongUrl.size(), download, decodeError, sessionError));
	assert(sessionError == SessionError::DownloadUrlMismatch);

	auto validUrl = makeDownloadUrl(AssetType::Dff, dffCrc, "https://example.test/model.dff");
	assert(session.acceptDownloadUrl(validUrl.data(), validUrl.size(), download, decodeError, sessionError));
	assert(!session.completeAsset(download.type, download.checksum, dff.data(), dff.size() - 1, sessionError));
	assert(sessionError == SessionError::AssetIntegrityFailure);

	assert(session.nextAssetRequest(request));
	assert(request.type == AssetType::Dff); // Failed bytes are retried, not acknowledged.
	assert(!session.acceptDownloadAcknowledgement(nullptr, 0, sessionError));
}


void testManifestGrowthAndPoolUniqueness()
{
	ClientSession session;
	session.beginNegotiation();
	SessionError sessionError = SessionError::None;
	assert(session.acceptCapabilityAcknowledgement(nullptr, 0, sessionError));
	DecodeError decodeError = DecodeError::None;

	auto first = makeVehicleRequest(11, 12, 20, 21, 0, 1, 30001);
	assert(session.acceptModelManifest(first.data(), first.size(), decodeError, sessionError)
		== ManifestDisposition::AcceptedVehicle);

	// Replayed identical manifest records are idempotent and do not add another pool entry.
	auto replayedFirst = makeVehicleRequest(11, 12, 20, 21, 0, 1, 30001);
	assert(session.acceptModelManifest(replayedFirst.data(), replayedFirst.size(), decodeError, sessionError)
		== ManifestDisposition::AcceptedVehicle);
	assert(session.vehicleModels().size() == 1);

	// Late model registrations append to the independent vehicle pool.
	auto second = makeVehicleRequest(13, 14, 22, 23, 1, 2, 30002);
	assert(session.acceptModelManifest(second.data(), second.size(), decodeError, sessionError)
		== ManifestDisposition::AcceptedVehicle);

	// A skipped index, duplicate index, or decreasing Count is not a valid append.
	auto skippedPool = makeVehicleRequest(15, 16, 24, 25, 3, 4, 30003);
	assert(session.acceptModelManifest(skippedPool.data(), skippedPool.size(), decodeError, sessionError)
		== ManifestDisposition::Rejected);
	assert(sessionError == SessionError::InconsistentManifest);

	auto duplicatePool = makeVehicleRequest(17, 18, 26, 27, 1, 3, 30004);
	assert(session.acceptModelManifest(duplicatePool.data(), duplicatePool.size(), decodeError, sessionError)
		== ManifestDisposition::Rejected);
	assert(sessionError == SessionError::InconsistentManifest);

	auto smallerCount = makeVehicleRequest(19, 20, 28, 29, 0, 1, 30005);
	assert(session.acceptModelManifest(smallerCount.data(), smallerCount.size(), decodeError, sessionError)
		== ManifestDisposition::Rejected);
	assert(sessionError == SessionError::InconsistentManifest);
	assert(session.vehicleModels().size() == 2);
}

class FakeTransport final : public RpcTransport
{
public:
	struct SentRpc
	{
		RpcId id;
		std::vector<std::uint8_t> payload;
	};

	bool sendRpc(RpcId id, const std::uint8_t* payload, std::size_t size) noexcept override
	{
		SentRpc sent { id, {} };
		if (payload != nullptr && size != 0)
		{
			sent.payload.assign(payload, payload + size);
		}
		sentCalls.push_back(std::move(sent));
		return true;
	}

	std::vector<SentRpc> sentCalls;
};

class FakeDownloader final : public AssetDownloadSink
{
public:
	struct Job
	{
		AssetType type;
		std::uint32_t checksum;
		std::uint32_t expectedSize;
		std::string url;
	};

	bool beginDownload(const AssetDownload& download) noexcept override
	{
		if (!acceptRequests)
		{
			return false;
		}
		jobs.push_back({ download.type, download.checksum, download.expectedSize,
			std::string(download.url) });
		return true;
	}

	bool acceptRequests = true;
	std::vector<Job> jobs;
};

void testNetworkAdapterFlow()
{
	constexpr std::array<std::uint8_t, 9> dff = { '1','2','3','4','5','6','7','8','9' };
	constexpr std::array<std::uint8_t, 4> txd = { 't','x','d','!' };
	constexpr std::uint32_t dffCrc = 0xCBF43926u;
	const std::uint32_t txdCrc = calculateCrc32(txd.data(), txd.size());

	FakeTransport transport;
	FakeDownloader downloader;
	ClientNetworkAdapter adapter(transport, downloader);

	assert(adapter.beginConnection());
	assert(transport.sentCalls.size() == 1);
	assert(transport.sentCalls.back().id == RpcId::ClientSupportsCustomVehicleModels);
	assert(transport.sentCalls.back().payload.empty());

	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ServerSupportsCustomVehicleModels),
		nullptr, 0) == RpcDisposition::Consume);

	auto manifest = makeVehicleRequest(dffCrc, txdCrc,
		static_cast<std::uint32_t>(dff.size()), static_cast<std::uint32_t>(txd.size()));
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelRequest),
		manifest.data(), manifest.size()) == RpcDisposition::Consume);
	assert(!adapter.isVehicleModelReady(30001));
	assert(transport.sentCalls.back().id == RpcId::RequestDff);
	assert(transport.sentCalls.back().payload.size() == AssetRequestWireSize);

	// The extension manifest channel is vehicle-only and fails closed on other model types.
	auto invalidExtensionManifest = manifest;
	invalidExtensionManifest[8] = static_cast<std::uint8_t>(ModelType::Skin);
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelRequest),
		invalidExtensionManifest.data(), invalidExtensionManifest.size()) == RpcDisposition::Consume);

	// Native SA-MP and 0.3.DL RPCs are never intercepted by this extension.
	constexpr std::array<std::uint8_t, 1> nativePayload = { 0x80 };
	for (const std::uint8_t nativeRpc : {
		static_cast<std::uint8_t>(179),
		static_cast<std::uint8_t>(181),
		static_cast<std::uint8_t>(182),
		static_cast<std::uint8_t>(183),
		static_cast<std::uint8_t>(184),
		static_cast<std::uint8_t>(185),
		static_cast<std::uint8_t>(186),
		static_cast<std::uint8_t>(187),
		static_cast<std::uint8_t>(188),
		static_cast<std::uint8_t>(189)
	})
	{
		assert(adapter.handleServerRpc(nativeRpc, nativePayload.data(), nativePayload.size())
			== RpcDisposition::ForwardToGame);
	}

	// Client-to-server extension IDs arriving from the server are reserved and consumed.
	for (const std::uint8_t wrongDirectionRpc : {
		static_cast<std::uint8_t>(221),
		static_cast<std::uint8_t>(223),
		static_cast<std::uint8_t>(224),
		static_cast<std::uint8_t>(226)
	})
	{
		assert(adapter.handleServerRpc(wrongDirectionRpc, nullptr, 0) == RpcDisposition::Consume);
	}

	auto malformedDffUrl = makeDownloadUrl(AssetType::Dff, dffCrc, "https://example.test/car.dff");
	malformedDffUrl[0] = 5; // Invalid transport marker while a custom DFF URL is expected.
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelUrl),
		malformedDffUrl.data(), malformedDffUrl.size()) == RpcDisposition::Consume);
	assert(downloader.jobs.empty());

	auto dffUrl = makeDownloadUrl(AssetType::Dff, dffCrc, "https://example.test/car.dff");
	downloader.acceptRequests = false; // Simulate a downloader refusing to create a worker.
	const std::size_t callsBeforeRetry = transport.sentCalls.size();
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelUrl),
		dffUrl.data(), dffUrl.size()) == RpcDisposition::Consume);
	assert(downloader.jobs.empty());
	assert(transport.sentCalls.size() == callsBeforeRetry); // No immediate retry loop.

	downloader.acceptRequests = true;
	assert(adapter.retryActiveAsset()); // Host-controlled retry dispatches the queued checksum request.
	assert(transport.sentCalls.back().id == RpcId::RequestDff);
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelUrl),
		dffUrl.data(), dffUrl.size()) == RpcDisposition::Consume);
	assert(downloader.jobs.size() == 1);
	assert(downloader.jobs.back().url == "https://example.test/car.dff");

	assert(adapter.completeAsset(AssetType::Dff, dffCrc, dff.data(), dff.size()));
	assert(transport.sentCalls.back().id == RpcId::FinishDownload);
	assert(transport.sentCalls.back().payload.size() == AssetCompletionWireSize);
	assert((transport.sentCalls.back().payload
		== std::vector<std::uint8_t> { 1, 0x26, 0x39, 0xF4, 0xCB }));

	const std::array<std::uint8_t, 1> malformedAck = { 1 };
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::DownloadCompleted),
		malformedAck.data(), malformedAck.size()) == RpcDisposition::Consume);
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::DownloadCompleted),
		nullptr, 0) == RpcDisposition::Consume);
	assert(transport.sentCalls.back().id == RpcId::RequestTxd);

	auto txdUrl = makeDownloadUrl(AssetType::Txd, txdCrc, "https://example.test/car.txd");
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelUrl),
		txdUrl.data(), txdUrl.size()) == RpcDisposition::Consume);
	assert(downloader.jobs.size() == 2);
	assert(adapter.completeAsset(AssetType::Txd, txdCrc, txd.data(), txd.size()));
	assert(transport.sentCalls.back().id == RpcId::FinishDownload);
	assert(transport.sentCalls.back().payload.size() == AssetCompletionWireSize);
	assert(transport.sentCalls.back().payload[0] == static_cast<std::uint8_t>(AssetType::Txd));

	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::DownloadCompleted),
		nullptr, 0) == RpcDisposition::Consume);
	assert(adapter.vehicleModels().size() == 1);
	assert(adapter.isVehicleModelReady(30001));
	assert(!adapter.isVehicleModelReady(39999));

	adapter.endConnection();
	assert(adapter.vehicleModels().empty());
	// RPC 225 is extension-only, even after the current session is reset.
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelUrl),
		dffUrl.data(), dffUrl.size()) == RpcDisposition::Consume);
}



struct BridgeTestState
{
	struct SentRpc
	{
		std::uint8_t id;
		std::vector<std::uint8_t> payload;
	};

	std::vector<SentRpc> sent;
	std::vector<std::string> downloadUrls;
};

std::int32_t bridgeTestSendRpc(
	void* userData,
	std::uint8_t rpcId,
	const std::uint8_t* payload,
	std::size_t payloadSize)
{
	if (userData == nullptr || (payload == nullptr && payloadSize != 0))
	{
		return 0;
	}

	auto& state = *static_cast<BridgeTestState*>(userData);
	BridgeTestState::SentRpc call { rpcId, {} };
	if (payloadSize != 0)
	{
		call.payload.assign(payload, payload + payloadSize);
	}
	state.sent.push_back(std::move(call));
	return 1;
}

std::int32_t bridgeTestBeginDownload(
	void* userData,
	std::uint8_t,
	std::uint32_t,
	std::uint32_t,
	const char* url,
	std::size_t urlSize)
{
	if (userData == nullptr || (url == nullptr && urlSize != 0))
	{
		return 0;
	}

	auto& state = *static_cast<BridgeTestState*>(userData);
	state.downloadUrls.emplace_back(url == nullptr ? "" : std::string(url, urlSize));
	return 1;
}

void testCAbiBridge()
{
	BridgeTestState state;
	custom_vehicle_callbacks_v1 callbacks {};
	callbacks.struct_size = sizeof(callbacks);
	callbacks.abi_version = CUSTOM_VEHICLE_BRIDGE_ABI_VERSION;
	callbacks.user_data = &state;
	callbacks.send_rpc = bridgeTestSendRpc;
	callbacks.begin_download = bridgeTestBeginDownload;

	custom_vehicle_adapter* adapter = custom_vehicle_adapter_create(&callbacks);
	assert(adapter != nullptr);
	assert(custom_vehicle_adapter_model_count(adapter) == 0);

	assert(custom_vehicle_adapter_begin_connection(adapter) == 1);
	assert(state.sent.size() == 1);
	assert(state.sent.back().id == static_cast<std::uint8_t>(RpcId::ClientSupportsCustomVehicleModels));

	assert(custom_vehicle_adapter_handle_server_rpc(
		adapter,
		static_cast<std::uint8_t>(RpcId::ServerSupportsCustomVehicleModels),
		nullptr,
		0) == 1);

	constexpr std::array<std::uint8_t, 4> dff = { 'd','f','f','!' };
	constexpr std::array<std::uint8_t, 4> txd = { 't','x','d','!' };
	const std::uint32_t dffCrc = calculateCrc32(dff.data(), dff.size());
	const std::uint32_t txdCrc = calculateCrc32(txd.data(), txd.size());
	const auto manifest = makeVehicleRequest(
		dffCrc,
		txdCrc,
		static_cast<std::uint32_t>(dff.size()),
		static_cast<std::uint32_t>(txd.size()));

	assert(custom_vehicle_adapter_handle_server_rpc(
		adapter,
		static_cast<std::uint8_t>(RpcId::ModelRequest),
		manifest.data(),
		manifest.size()) == 1);
	assert(state.sent.back().id == static_cast<std::uint8_t>(RpcId::RequestDff));

	assert(custom_vehicle_adapter_model_count(adapter) == 1);
	assert(custom_vehicle_adapter_is_model_ready(adapter, 30001) == 0);
	const auto streamIn = makeStreamInVehicle();
	custom_vehicle_stream_in_vehicle_v1 decodedVehicle {};
	assert(custom_vehicle_adapter_decode_stream_in_vehicle(
		adapter, streamIn.data(), streamIn.size(), &decodedVehicle) == 2);
	assert(decodedVehicle.vehicle_id == 1234);
	assert(decodedVehicle.custom_model_id == 30001);
	assert(decodedVehicle.base_model_id == 400);

	std::array<std::uint8_t, StreamInVehicleWireSize> rewrittenStreamIn {};
	rewrittenStreamIn.fill(0xA5);
	std::size_t rewrittenSize = 999;
	std::int32_t rewrittenCustomModelId = -1;
	std::int32_t rewrittenBaseModelId = -1;
	assert(custom_vehicle_adapter_rewrite_stream_in_vehicle(
		adapter,
		streamIn.data(),
		streamIn.size(),
		rewrittenStreamIn.data(),
		rewrittenStreamIn.size(),
		&rewrittenSize,
		&rewrittenCustomModelId,
		&rewrittenBaseModelId) == 2);
	assert(rewrittenSize == StreamInVehicleWireSize);
	assert(rewrittenCustomModelId == 30001);
	assert(rewrittenBaseModelId == 400);
	assert(rewrittenStreamIn[2] == 0x90 && rewrittenStreamIn[3] == 0x01);
	for (std::size_t index = 0; index < streamIn.size(); ++index)
	{
		if (index >= StreamInVehicleModelFieldOffset
			&& index < StreamInVehicleModelFieldOffset + sizeof(std::int32_t))
		{
			continue;
		}
		assert(rewrittenStreamIn[index] == streamIn[index]);
	}

	// A too-small C ABI destination must not receive a partial RPC payload or metadata.
	std::array<std::uint8_t, StreamInVehicleWireSize - 1> tooSmallOutput {};
	tooSmallOutput.fill(0x3C);
	const auto tooSmallSentinel = tooSmallOutput;
	std::size_t tooSmallSize = 777;
	std::int32_t tooSmallCustomId = 123;
	std::int32_t tooSmallBaseId = 456;
	assert(custom_vehicle_adapter_rewrite_stream_in_vehicle(
		adapter,
		streamIn.data(),
		streamIn.size(),
		tooSmallOutput.data(),
		tooSmallOutput.size(),
		&tooSmallSize,
		&tooSmallCustomId,
		&tooSmallBaseId) == -1);
	assert(tooSmallOutput == tooSmallSentinel);
	assert(tooSmallSize == 777);
	assert(tooSmallCustomId == 123 && tooSmallBaseId == 456);

	const auto standardStreamIn = makeStreamInVehicle(411);
	const auto rewriteSentinel = rewrittenStreamIn;
	rewrittenSize = 999;
	rewrittenCustomModelId = -1;
	rewrittenBaseModelId = -1;
	assert(custom_vehicle_adapter_rewrite_stream_in_vehicle(
		adapter,
		standardStreamIn.data(),
		standardStreamIn.size(),
		rewrittenStreamIn.data(),
		rewrittenStreamIn.size(),
		&rewrittenSize,
		&rewrittenCustomModelId,
		&rewrittenBaseModelId) == 0);
	assert(rewrittenSize == 999);
	assert(rewrittenCustomModelId == -1 && rewrittenBaseModelId == -1);
	assert(rewrittenStreamIn == rewriteSentinel);

	assert(custom_vehicle_adapter_decode_stream_in_vehicle(
		adapter, standardStreamIn.data(), standardStreamIn.size(), &decodedVehicle) == 0);
	assert(custom_vehicle_adapter_decode_stream_in_vehicle(
		adapter, nullptr, 0, &decodedVehicle) == -1);
	custom_vehicle_model_request_v1 model {};
	assert(custom_vehicle_adapter_copy_model(adapter, 0, &model) == 1);
	assert(model.new_id == 30001);
	assert(model.base_id == 400);
	assert(model.dff_checksum == dffCrc);
	assert(model.txd_checksum == txdCrc);
	assert(custom_vehicle_adapter_copy_model(adapter, 1, &model) == 0);

	auto dffUrl = makeDownloadUrl(AssetType::Dff, dffCrc, "https://example.test/cabi.dff");
	assert(custom_vehicle_adapter_handle_server_rpc(
		adapter,
		static_cast<std::uint8_t>(RpcId::ModelUrl),
		dffUrl.data(),
		dffUrl.size()) == 1);
	assert(state.downloadUrls.size() == 1);
	assert(state.downloadUrls.back() == "https://example.test/cabi.dff");
	assert(custom_vehicle_adapter_complete_asset(
		adapter, static_cast<std::uint8_t>(AssetType::Dff), dffCrc, dff.data(), dff.size()) == 1);
	assert(state.sent.back().id == static_cast<std::uint8_t>(RpcId::FinishDownload));
	assert(state.sent.back().payload.size() == AssetCompletionWireSize);
	assert(state.sent.back().payload[0] == static_cast<std::uint8_t>(AssetType::Dff));
	assert(state.sent.back().payload[1] == static_cast<std::uint8_t>(dffCrc));
	assert(state.sent.back().payload[2] == static_cast<std::uint8_t>(dffCrc >> 8));
	assert(state.sent.back().payload[3] == static_cast<std::uint8_t>(dffCrc >> 16));
	assert(state.sent.back().payload[4] == static_cast<std::uint8_t>(dffCrc >> 24));

	assert(custom_vehicle_adapter_handle_server_rpc(
		adapter,
		static_cast<std::uint8_t>(RpcId::DownloadCompleted),
		nullptr,
		0) == 1);
	assert(state.sent.back().id == static_cast<std::uint8_t>(RpcId::RequestTxd));

	auto txdUrl = makeDownloadUrl(AssetType::Txd, txdCrc, "https://example.test/cabi.txd");
	assert(custom_vehicle_adapter_handle_server_rpc(
		adapter,
		static_cast<std::uint8_t>(RpcId::ModelUrl),
		txdUrl.data(),
		txdUrl.size()) == 1);
	assert(custom_vehicle_adapter_complete_asset(
		adapter, static_cast<std::uint8_t>(AssetType::Txd), txdCrc, txd.data(), txd.size()) == 1);
	assert(state.sent.back().id == static_cast<std::uint8_t>(RpcId::FinishDownload));
	assert(state.sent.back().payload.size() == AssetCompletionWireSize);
	assert(state.sent.back().payload[0] == static_cast<std::uint8_t>(AssetType::Txd));
	assert(state.sent.back().payload[1] == static_cast<std::uint8_t>(txdCrc));
	assert(state.sent.back().payload[2] == static_cast<std::uint8_t>(txdCrc >> 8));
	assert(state.sent.back().payload[3] == static_cast<std::uint8_t>(txdCrc >> 16));
	assert(state.sent.back().payload[4] == static_cast<std::uint8_t>(txdCrc >> 24));
	assert(custom_vehicle_adapter_handle_server_rpc(
		adapter,
		static_cast<std::uint8_t>(RpcId::DownloadCompleted),
		nullptr,
		0) == 1);
	assert(custom_vehicle_adapter_is_model_ready(adapter, 30001) == 1);
	assert(custom_vehicle_adapter_is_model_ready(adapter, 39999) == 0);
	assert(custom_vehicle_adapter_decode_stream_in_vehicle(
		adapter, streamIn.data(), streamIn.size(), &decodedVehicle) == 1);
	assert(decodedVehicle.custom_model_id == 30001 && decodedVehicle.base_model_id == 400);
	rewrittenSize = 0;
	assert(custom_vehicle_adapter_rewrite_stream_in_vehicle(
		adapter,
		streamIn.data(),
		streamIn.size(),
		rewrittenStreamIn.data(),
		rewrittenStreamIn.size(),
		&rewrittenSize,
		&rewrittenCustomModelId,
		&rewrittenBaseModelId) == 1);
	assert(rewrittenSize == StreamInVehicleWireSize);
	assert(rewrittenCustomModelId == 30001 && rewrittenBaseModelId == 400);

	auto unknownCustomStreamIn = makeStreamInVehicle(30002);
	assert(custom_vehicle_adapter_decode_stream_in_vehicle(
		adapter, unknownCustomStreamIn.data(), unknownCustomStreamIn.size(), &decodedVehicle) == -1);
	assert(custom_vehicle_adapter_rewrite_stream_in_vehicle(
		adapter,
		unknownCustomStreamIn.data(),
		unknownCustomStreamIn.size(),
		rewrittenStreamIn.data(),
		rewrittenStreamIn.size(),
		&rewrittenSize,
		&rewrittenCustomModelId,
		&rewrittenBaseModelId) == -1);

	// Native actor/artwork IDs are passed through unchanged.
	for (const std::uint8_t nativeRpc : {
		static_cast<std::uint8_t>(179),
		static_cast<std::uint8_t>(181),
		static_cast<std::uint8_t>(182),
		static_cast<std::uint8_t>(183),
		static_cast<std::uint8_t>(184),
		static_cast<std::uint8_t>(185),
		static_cast<std::uint8_t>(186),
		static_cast<std::uint8_t>(187),
		static_cast<std::uint8_t>(188),
		static_cast<std::uint8_t>(189)
	})
	{
		assert(custom_vehicle_adapter_handle_server_rpc(
			adapter, nativeRpc, nullptr, 0) == 0);
	}

	// Reserved client-to-server extension RPCs are still consumed if received inbound.
	for (const std::uint8_t wrongDirectionRpc : {
		static_cast<std::uint8_t>(221),
		static_cast<std::uint8_t>(223),
		static_cast<std::uint8_t>(224),
		static_cast<std::uint8_t>(226)
	})
	{
		assert(custom_vehicle_adapter_handle_server_rpc(
			adapter, wrongDirectionRpc, nullptr, 0) == 1);
	}
	assert(custom_vehicle_adapter_handle_server_rpc(adapter, 250, nullptr, 0) == 0);
	assert(custom_vehicle_adapter_handle_server_rpc(adapter, 250, nullptr, 1) == -1);
	custom_vehicle_adapter_end_connection(adapter);
	assert(custom_vehicle_adapter_model_count(adapter) == 0);
	custom_vehicle_adapter_destroy(adapter);

	callbacks.abi_version++;
	assert(custom_vehicle_adapter_create(&callbacks) == nullptr);
}

void testAssetCache()
{
	constexpr std::array<std::uint8_t, 9> bytes = { '1','2','3','4','5','6','7','8','9' };
	constexpr std::uint32_t checksum = 0xCBF43926u;
	const auto uniqueSuffix = std::to_string(
		std::chrono::steady_clock::now().time_since_epoch().count());
	const std::filesystem::path root =
		std::filesystem::temp_directory_path() / ("openmp-custom-vehicle-cache-" + uniqueSuffix);
	std::error_code error;
	std::filesystem::remove_all(root, error);

	{
		AssetCache cache(root);
		assert(!cache.contains(AssetType::Dff, checksum, static_cast<std::uint32_t>(bytes.size())));
		assert(cache.pathFor(AssetType::Dff, checksum) != cache.pathFor(AssetType::Txd, checksum));
		assert(cache.store(AssetType::Dff, checksum,
			static_cast<std::uint32_t>(bytes.size()), bytes.data(), bytes.size()));
		assert(cache.contains(AssetType::Dff, checksum, static_cast<std::uint32_t>(bytes.size())));

		// Invalid input cannot replace an already verified cache entry.
		assert(!cache.store(AssetType::Dff, checksum,
			static_cast<std::uint32_t>(bytes.size() - 1), bytes.data(), bytes.size()));
		assert(cache.contains(AssetType::Dff, checksum, static_cast<std::uint32_t>(bytes.size())));

		// Corrupt the on-disk file and make sure the next store repairs it.
		{
			std::ofstream corrupt(cache.pathFor(AssetType::Dff, checksum),
				std::ios::binary | std::ios::trunc);
			constexpr char damaged[] = "12345678X";
			corrupt.write(damaged, sizeof(damaged) - 1);
			assert(corrupt.good());
		}
		assert(!cache.contains(AssetType::Dff, checksum, static_cast<std::uint32_t>(bytes.size())));
		assert(cache.store(AssetType::Dff, checksum,
			static_cast<std::uint32_t>(bytes.size()), bytes.data(), bytes.size()));
		assert(cache.contains(AssetType::Dff, checksum, static_cast<std::uint32_t>(bytes.size())));
	}

	std::filesystem::remove_all(root, error);
	assert(!error);
}

void testAssetIntegrity()
{
	constexpr std::array<std::uint8_t, 9> sample = { '1','2','3','4','5','6','7','8','9' };
	constexpr std::uint32_t expectedCrc = 0xCBF43926u;
	assert(calculateCrc32(sample.data(), sample.size()) == expectedCrc);
	assert(verifyAsset(sample.data(), sample.size(), 9, expectedCrc));
	assert(!verifyAsset(sample.data(), sample.size(), 8, expectedCrc));
	assert(!verifyAsset(sample.data(), sample.size(), 9, 0));
}
} // namespace

int main()
{
	testRpcIdIsolation();
	testNativeVehicleModelSlotRegistry();
	testNativeVehicleModelLifecycle();
	testNativeVehicleModelVehicleBindings();
	testNativeVehicleModelLifecycleCAbi();
	testNativeVehicleModelVehicleBindingsCAbi();
	testNativeVehicleModelSlotRegistryCAbi();
	testVehicleStreamQueue();
	testVehicleStreamQueueCAbi();
	testManifestDecode();
	testStreamInVehicleDecode();
	testStreamInVehicleBaseRewrite();
	testMalformedManifest();
	testModelCountAndPoolIndexValidation();
	testDownloadUrlCodec();
	testAssetRequestEncoding();
	testAssetCompletionEncoding();
	testVehicleValidation();
	testSessionDownloadLifecycle();
	testSessionRejectsMismatchedUrlAndCorruptFile();
	testManifestGrowthAndPoolUniqueness();
	testNetworkAdapterFlow();
	testCAbiBridge();
	testAssetCache();
	testAssetIntegrity();
	return 0;
}
