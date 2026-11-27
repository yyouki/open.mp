#pragma once

#include <custom_vehicle/protocol.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace custom_vehicle::native
{
inline constexpr std::size_t VehicleStreamQueueVehicleIdLimit = 2000;
inline constexpr std::size_t VehicleStreamQueueCapacity = VehicleStreamQueueVehicleIdLimit - 1;

struct VehicleStreamRequest
{
	protocol::StreamInVehicleData vehicle {};
	std::int32_t baseModelId = 0;
};

enum class VehicleStreamQueueResult
{
	Queued,
	Updated,
	InvalidVehicleId,
	InvalidCustomModelId,
	InvalidBaseModelId,
	InvalidVehicleData,
	Full
};

/*
 * Bounded hand-off between the network/RPC callback and the GTA game thread.
 *
 * Network callbacks enqueue only validated, copied values. The game-thread
 * consumer pops one request at a time and performs native work separately.
 * Repeated stream-in events for a vehicle that is already pending replace its
 * copied data in place without adding another FIFO entry. Vehicle removal can
 * cancel a pending stream-in before the game-thread consumer sees it.
 *
 * This queue has no GTA addresses, native pointers, or RenderWare state.
 */
class VehicleStreamQueue
{
public:
	VehicleStreamQueue() = default;

	VehicleStreamQueue(const VehicleStreamQueue&) = delete;
	VehicleStreamQueue& operator=(const VehicleStreamQueue&) = delete;

	VehicleStreamQueueResult enqueue(const VehicleStreamRequest& request);
	bool peek(VehicleStreamRequest& request) const;
	bool pop(VehicleStreamRequest& request);
	bool cancel(std::uint16_t vehicle_id);
	void clear();
	[[nodiscard]] std::size_t size() const;

private:
	struct Entry
	{
		VehicleStreamRequest request {};
		bool pending = false;
	};

	[[nodiscard]] static VehicleStreamQueueResult validate(
		const VehicleStreamRequest& request) noexcept;

	mutable std::mutex mutex_;
	std::array<Entry, VehicleStreamQueueVehicleIdLimit> entries_ {};
	std::deque<std::uint16_t> order_;
};
} // namespace custom_vehicle::native
