#include <custom_vehicle/native_vehicle_model_backend.hpp>

namespace custom_vehicle::native
{
VehicleModelLifecycleController::VehicleModelLifecycleController(
	std::int32_t first_slot,
	std::int32_t last_slot)
	: slots_(first_slot, last_slot)
{
}

bool VehicleModelLifecycleController::hasValidSlotRange() const noexcept
{
	return slots_.hasValidRange();
}

VehicleModelBackendResult VehicleModelLifecycleController::activate(
	const protocol::ModelRequest& model,
	bool assets_ready,
	const VehicleModelNativeOperations& operations,
	void* user_data,
	std::int32_t& native_model_slot)
{
	if (!isCustomModelId(model.newId)
		|| model.type != protocol::ModelType::Vehicle
		|| !protocol::validateVehicleModelRequest(model))
	{
		return VehicleModelBackendResult::InvalidRequest;
	}
	if (!assets_ready)
	{
		return VehicleModelBackendResult::AssetsPending;
	}
	if (!hasRequiredOperations(operations))
	{
		return VehicleModelBackendResult::MissingOperations;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	Entry& entry = entries_[customIndex(model.newId)];
	if (entry.present)
	{
		if (entry.retiring)
		{
			return VehicleModelBackendResult::ModelRetiring;
		}
		if (entry.stage == VehicleModelResourceStage::ClumpAttached
			&& entry.model.baseId == model.baseId
			&& entry.model.dffChecksum == model.dffChecksum
			&& entry.model.txdChecksum == model.txdChecksum
			&& entry.model.dffSize == model.dffSize
			&& entry.model.txdSize == model.txdSize)
		{
			native_model_slot = entry.native_model_slot;
			return VehicleModelBackendResult::AlreadyActive;
		}
		// Never overwrite an allocated native model in-place. Retire and fully
		// release the existing generation before a replacement can be installed.
		return VehicleModelBackendResult::ModelRetiring;
	}

	std::int32_t slot = -1;
	const auto reserve_result = slots_.reserve(
		model.newId,
		operations.is_slot_vacant,
		user_data,
		slot);
	switch (reserve_result)
	{
		case ModelSlotReserveResult::Reserved:
			break;
		case ModelSlotReserveResult::AlreadyReserved:
			// A slot without a matching lifecycle entry indicates corrupted
			// ownership state. Do not guess which native resources own it.
			return VehicleModelBackendResult::CleanupPending;
		case ModelSlotReserveResult::InvalidCustomModelId:
		case ModelSlotReserveResult::InvalidSlotRange:
			return VehicleModelBackendResult::InvalidRequest;
		case ModelSlotReserveResult::NoFreeSlot:
			return VehicleModelBackendResult::NoFreeSlot;
		case ModelSlotReserveResult::ModelRetiring:
			return VehicleModelBackendResult::ModelRetiring;
		case ModelSlotReserveResult::MissingVacancyQuery:
			return VehicleModelBackendResult::MissingOperations;
	}

	entry.model = model;
	entry.native_model_slot = slot;
	entry.stage = VehicleModelResourceStage::Reserved;
	entry.present = true;
	entry.retiring = false;

	if (!operations.create_model_info(user_data, slot, entry.model))
	{
		return rollbackFailedActivation(
			entry,
			model.newId,
			VehicleModelBackendResult::CreateModelInfoFailed,
			operations,
			user_data);
	}
	entry.stage = VehicleModelResourceStage::ModelInfoCreated;

	if (!operations.load_vehicle_assets(user_data, slot, entry.model))
	{
		return rollbackFailedActivation(
			entry,
			model.newId,
			VehicleModelBackendResult::LoadAssetsFailed,
			operations,
			user_data);
	}
	entry.stage = VehicleModelResourceStage::AssetsLoaded;

	if (!operations.attach_vehicle_clump(user_data, slot, entry.model))
	{
		return rollbackFailedActivation(
			entry,
			model.newId,
			VehicleModelBackendResult::AttachClumpFailed,
			operations,
			user_data);
	}
	entry.stage = VehicleModelResourceStage::ClumpAttached;

	native_model_slot = slot;
	return VehicleModelBackendResult::Activated;
}

bool VehicleModelLifecycleController::retainForVehicle(
	std::int32_t custom_model_id,
	std::int32_t& native_model_slot)
{
	if (!isCustomModelId(custom_model_id))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const Entry& entry = entries_[customIndex(custom_model_id)];
	if (!entry.present || entry.retiring
		|| entry.stage != VehicleModelResourceStage::ClumpAttached)
	{
		return false;
	}
	return slots_.retain(custom_model_id, native_model_slot);
}

bool VehicleModelLifecycleController::releaseVehicleUse(
	std::int32_t custom_model_id,
	std::int32_t native_model_slot)
{
	if (!isCustomModelId(custom_model_id))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const Entry& entry = entries_[customIndex(custom_model_id)];
	return entry.present
		&& slots_.releaseUse(custom_model_id, native_model_slot);
}

VehicleModelBackendResult VehicleModelLifecycleController::retire(
	std::int32_t custom_model_id,
	const VehicleModelNativeOperations& operations,
	void* user_data)
{
	if (!isCustomModelId(custom_model_id))
	{
		return VehicleModelBackendResult::NotFound;
	}
	if (!hasRequiredOperations(operations))
	{
		return VehicleModelBackendResult::MissingOperations;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	Entry& entry = entries_[customIndex(custom_model_id)];
	if (!entry.present)
	{
		return VehicleModelBackendResult::NotFound;
	}
	if (!entry.retiring)
	{
		if (!slots_.retire(custom_model_id))
		{
			return VehicleModelBackendResult::NotFound;
		}
		entry.retiring = true;
	}
	return cleanupRetired(entry, custom_model_id, operations, user_data);
}

VehicleModelBackendResult VehicleModelLifecycleController::retryRetiredCleanup(
	std::int32_t custom_model_id,
	const VehicleModelNativeOperations& operations,
	void* user_data)
{
	if (!isCustomModelId(custom_model_id))
	{
		return VehicleModelBackendResult::NotFound;
	}
	if (!hasRequiredOperations(operations))
	{
		return VehicleModelBackendResult::MissingOperations;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	Entry& entry = entries_[customIndex(custom_model_id)];
	if (!entry.present || !entry.retiring)
	{
		return VehicleModelBackendResult::NotFound;
	}
	return cleanupRetired(entry, custom_model_id, operations, user_data);
}

bool VehicleModelLifecycleController::find(
	std::int32_t custom_model_id,
	std::int32_t& native_model_slot) const
{
	if (!isCustomModelId(custom_model_id))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const Entry& entry = entries_[customIndex(custom_model_id)];
	if (!entry.present)
	{
		return false;
	}
	native_model_slot = entry.native_model_slot;
	return true;
}

bool VehicleModelLifecycleController::isActive(std::int32_t custom_model_id) const
{
	if (!isCustomModelId(custom_model_id))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const Entry& entry = entries_[customIndex(custom_model_id)];
	return entry.present
		&& !entry.retiring
		&& entry.stage == VehicleModelResourceStage::ClumpAttached;
}

std::size_t VehicleModelLifecycleController::size() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::size_t count = 0;
	for (const Entry& entry : entries_)
	{
		if (entry.present)
		{
			++count;
		}
	}
	return count;
}

bool VehicleModelLifecycleController::isCustomModelId(std::int32_t model_id) noexcept
{
	return model_id >= FirstCustomVehicleModelId && model_id <= LastCustomVehicleModelId;
}

std::size_t VehicleModelLifecycleController::customIndex(std::int32_t model_id) noexcept
{
	return static_cast<std::size_t>(model_id - FirstCustomVehicleModelId);
}

bool VehicleModelLifecycleController::hasRequiredOperations(
	const VehicleModelNativeOperations& operations) noexcept
{
	return operations.is_slot_vacant != nullptr
		&& operations.create_model_info != nullptr
		&& operations.load_vehicle_assets != nullptr
		&& operations.attach_vehicle_clump != nullptr
		&& operations.destroy_model != nullptr;
}

VehicleModelBackendResult VehicleModelLifecycleController::rollbackFailedActivation(
	Entry& entry,
	std::int32_t custom_model_id,
	VehicleModelBackendResult original_failure,
	const VehicleModelNativeOperations& operations,
	void* user_data)
{
	entry.retiring = true;
	if (!slots_.retire(custom_model_id))
	{
		return VehicleModelBackendResult::CleanupPending;
	}

	const VehicleModelBackendResult cleanup_result =
		cleanupRetired(entry, custom_model_id, operations, user_data);
	return cleanup_result == VehicleModelBackendResult::Released
		? original_failure
		: cleanup_result;
}

VehicleModelBackendResult VehicleModelLifecycleController::cleanupRetired(
	Entry& entry,
	std::int32_t custom_model_id,
	const VehicleModelNativeOperations& operations,
	void* user_data)
{
	if (!slots_.canRelease(custom_model_id))
	{
		return VehicleModelBackendResult::ModelInUse;
	}

	if (!operations.destroy_model(
		user_data,
		entry.native_model_slot,
		entry.model,
		entry.stage))
	{
		return VehicleModelBackendResult::CleanupPending;
	}

	const auto release_result = slots_.release(
		custom_model_id,
		entry.native_model_slot,
		operations.is_slot_vacant,
		user_data);
	if (release_result != ModelSlotReleaseResult::Released)
	{
		return release_result == ModelSlotReleaseResult::ModelInUse
			? VehicleModelBackendResult::ModelInUse
			: VehicleModelBackendResult::CleanupPending;
	}

	entry = Entry {};
	return VehicleModelBackendResult::Released;
}
} // namespace custom_vehicle::native
