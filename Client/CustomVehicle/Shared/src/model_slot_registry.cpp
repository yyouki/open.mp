#include <custom_vehicle/model_slot_registry.hpp>

#include <limits>

namespace custom_vehicle::native
{
VehicleModelSlotRegistry::VehicleModelSlotRegistry(std::int32_t first_slot, std::int32_t last_slot)
	: first_slot_(first_slot)
	, last_slot_(last_slot)
	, valid_range_(
		first_slot >= FirstDynamicVehicleModelSlot
		&& last_slot <= LastEngineModelInfoSlot
		&& first_slot <= last_slot)
	, slot_owners_(valid_range_
		? static_cast<std::size_t>(last_slot - first_slot + 1)
		: 0, -1)
{
	custom_to_slot_.fill(-1);
}

bool VehicleModelSlotRegistry::hasValidRange() const noexcept
{
	return valid_range_;
}

std::int32_t VehicleModelSlotRegistry::firstSlot() const noexcept
{
	return first_slot_;
}

std::int32_t VehicleModelSlotRegistry::lastSlot() const noexcept
{
	return last_slot_;
}

ModelSlotReserveResult VehicleModelSlotRegistry::reserve(
	std::int32_t custom_model_id,
	NativeModelSlotVacancyQuery is_slot_vacant,
	void* user_data,
	std::int32_t& native_model_slot)
{
	if (!isCustomModelId(custom_model_id))
	{
		return ModelSlotReserveResult::InvalidCustomModelId;
	}
	if (!valid_range_)
	{
		return ModelSlotReserveResult::InvalidSlotRange;
	}
	if (is_slot_vacant == nullptr)
	{
		return ModelSlotReserveResult::MissingVacancyQuery;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const std::size_t model_index = customIndex(custom_model_id);
	const std::int32_t existing_slot = custom_to_slot_[model_index];
	if (existing_slot >= 0)
	{
		if (retiring_[model_index])
		{
			return ModelSlotReserveResult::ModelRetiring;
		}
		native_model_slot = existing_slot;
		return ModelSlotReserveResult::AlreadyReserved;
	}

	const std::size_t capacity = slot_owners_.size();
	for (std::size_t scanned = 0; scanned < capacity; ++scanned)
	{
		const std::size_t offset = (next_slot_offset_ + scanned) % capacity;
		if (slot_owners_[offset] >= 0)
		{
			continue;
		}

		const std::int32_t candidate = first_slot_ + static_cast<std::int32_t>(offset);
		if (!is_slot_vacant(user_data, candidate))
		{
			continue;
		}

		slot_owners_[offset] = custom_model_id;
		custom_to_slot_[model_index] = candidate;
		active_vehicle_uses_[model_index] = 0;
		retiring_[model_index] = false;
		next_slot_offset_ = (offset + 1) % capacity;
		native_model_slot = candidate;
		return ModelSlotReserveResult::Reserved;
	}

	return ModelSlotReserveResult::NoFreeSlot;
}

bool VehicleModelSlotRegistry::find(
	std::int32_t custom_model_id,
	std::int32_t& native_model_slot) const
{
	if (!isCustomModelId(custom_model_id))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const std::int32_t slot = custom_to_slot_[customIndex(custom_model_id)];
	if (slot < 0)
	{
		return false;
	}
	native_model_slot = slot;
	return true;
}

bool VehicleModelSlotRegistry::retain(
	std::int32_t custom_model_id,
	std::int32_t& native_model_slot)
{
	if (!isCustomModelId(custom_model_id))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const std::size_t index = customIndex(custom_model_id);
	const std::int32_t slot = custom_to_slot_[index];
	if (slot < 0 || retiring_[index]
		|| active_vehicle_uses_[index] == std::numeric_limits<std::size_t>::max())
	{
		return false;
	}

	++active_vehicle_uses_[index];
	native_model_slot = slot;
	return true;
}

bool VehicleModelSlotRegistry::releaseUse(
	std::int32_t custom_model_id,
	std::int32_t native_model_slot)
{
	if (!isCustomModelId(custom_model_id))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const std::size_t index = customIndex(custom_model_id);
	if (custom_to_slot_[index] != native_model_slot || active_vehicle_uses_[index] == 0)
	{
		return false;
	}

	--active_vehicle_uses_[index];
	return true;
}

bool VehicleModelSlotRegistry::retire(std::int32_t custom_model_id)
{
	if (!isCustomModelId(custom_model_id))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const std::size_t index = customIndex(custom_model_id);
	if (custom_to_slot_[index] < 0)
	{
		return false;
	}
	retiring_[index] = true;
	return true;
}

bool VehicleModelSlotRegistry::canRelease(std::int32_t custom_model_id) const
{
	if (!isCustomModelId(custom_model_id))
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const std::size_t index = customIndex(custom_model_id);
	return custom_to_slot_[index] >= 0
		&& retiring_[index]
		&& active_vehicle_uses_[index] == 0;
}

ModelSlotReleaseResult VehicleModelSlotRegistry::release(
	std::int32_t custom_model_id,
	std::int32_t expected_native_model_slot,
	NativeModelSlotVacancyQuery is_slot_vacant,
	void* user_data)
{
	if (!isCustomModelId(custom_model_id))
	{
		return ModelSlotReleaseResult::NotFound;
	}
	if (is_slot_vacant == nullptr)
	{
		return ModelSlotReleaseResult::MissingVacancyQuery;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	const std::size_t model_index = customIndex(custom_model_id);
	const std::int32_t current_slot = custom_to_slot_[model_index];
	if (current_slot < 0)
	{
		return ModelSlotReleaseResult::NotFound;
	}
	if (current_slot != expected_native_model_slot)
	{
		return ModelSlotReleaseResult::SlotMismatch;
	}
	if (!retiring_[model_index])
	{
		return ModelSlotReleaseResult::NotRetiring;
	}
	if (active_vehicle_uses_[model_index] != 0)
	{
		return ModelSlotReleaseResult::ModelInUse;
	}

	const std::size_t slot_offset = static_cast<std::size_t>(current_slot - first_slot_);
	if (slot_offset >= slot_owners_.size() || slot_owners_[slot_offset] != custom_model_id)
	{
		return ModelSlotReleaseResult::SlotMismatch;
	}
	if (!is_slot_vacant(user_data, current_slot))
	{
		return ModelSlotReleaseResult::NativeSlotStillOccupied;
	}

	slot_owners_[slot_offset] = -1;
	custom_to_slot_[model_index] = -1;
	active_vehicle_uses_[model_index] = 0;
	retiring_[model_index] = false;
	next_slot_offset_ = slot_offset;
	return ModelSlotReleaseResult::Released;
}

std::size_t VehicleModelSlotRegistry::size() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::size_t count = 0;
	for (const std::int32_t slot : custom_to_slot_)
	{
		if (slot >= 0)
		{
			++count;
		}
	}
	return count;
}

bool VehicleModelSlotRegistry::isCustomModelId(std::int32_t model_id) noexcept
{
	return model_id >= FirstCustomVehicleModelId && model_id <= LastCustomVehicleModelId;
}

std::size_t VehicleModelSlotRegistry::customIndex(std::int32_t model_id) const noexcept
{
	return static_cast<std::size_t>(model_id - FirstCustomVehicleModelId);
}
} // namespace custom_vehicle::native
