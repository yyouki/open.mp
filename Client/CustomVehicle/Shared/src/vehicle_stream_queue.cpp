#include <custom_vehicle/vehicle_stream_queue.hpp>

#include <cmath>

namespace custom_vehicle::native
{
VehicleStreamQueueResult VehicleStreamQueue::enqueue(const VehicleStreamRequest& request)
{
	const VehicleStreamQueueResult validation = validate(request);
	if (validation != VehicleStreamQueueResult::Queued)
	{
		return validation;
	}

	const std::uint16_t vehicle_id = request.vehicle.vehicleId;
	std::lock_guard<std::mutex> lock(mutex_);
	Entry& entry = entries_[vehicle_id];
	if (entry.pending)
	{
		entry.request = request;
		return VehicleStreamQueueResult::Updated;
	}

	if (order_.size() >= VehicleStreamQueueCapacity)
	{
		return VehicleStreamQueueResult::Full;
	}

	entry.request = request;
	entry.pending = true;
	order_.push_back(vehicle_id);
	return VehicleStreamQueueResult::Queued;
}

bool VehicleStreamQueue::pop(VehicleStreamRequest& request)
{
	std::lock_guard<std::mutex> lock(mutex_);
	while (!order_.empty())
	{
		const std::uint16_t vehicle_id = order_.front();
		order_.pop_front();

		Entry& entry = entries_[vehicle_id];
		if (!entry.pending)
		{
			continue;
		}

		request = entry.request;
		entry.pending = false;
		return true;
	}
	return false;
}

bool VehicleStreamQueue::cancel(std::uint16_t vehicle_id)
{
	if (vehicle_id == 0 || vehicle_id >= VehicleStreamQueueVehicleIdLimit)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(mutex_);
	Entry& entry = entries_[vehicle_id];
	if (!entry.pending)
	{
		return false;
	}

	entry.pending = false;
	for (auto iterator = order_.begin(); iterator != order_.end();)
	{
		if (*iterator == vehicle_id)
		{
			iterator = order_.erase(iterator);
		}
		else
		{
			++iterator;
		}
	}
	return true;
}

void VehicleStreamQueue::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (Entry& entry : entries_)
	{
		entry.pending = false;
	}
	order_.clear();
}

std::size_t VehicleStreamQueue::size() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return order_.size();
}

VehicleStreamQueueResult VehicleStreamQueue::validate(
	const VehicleStreamRequest& request) noexcept
{
	if (request.vehicle.vehicleId == 0
		|| request.vehicle.vehicleId >= VehicleStreamQueueVehicleIdLimit)
	{
		return VehicleStreamQueueResult::InvalidVehicleId;
	}
	if (!protocol::isCustomVehicleModelId(request.vehicle.modelId))
	{
		return VehicleStreamQueueResult::InvalidCustomModelId;
	}
	if (!protocol::isSupportedVehicleBaseId(request.baseModelId))
	{
		return VehicleStreamQueueResult::InvalidBaseModelId;
	}

	const auto& vehicle = request.vehicle;
	if (!std::isfinite(vehicle.position[0])
		|| !std::isfinite(vehicle.position[1])
		|| !std::isfinite(vehicle.position[2])
		|| !std::isfinite(vehicle.angle)
		|| !std::isfinite(vehicle.health))
	{
		return VehicleStreamQueueResult::InvalidVehicleData;
	}
	return VehicleStreamQueueResult::Queued;
}
} // namespace custom_vehicle::native
