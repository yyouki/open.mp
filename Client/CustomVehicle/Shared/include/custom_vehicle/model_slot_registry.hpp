#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace custom_vehicle::native
{
inline constexpr std::int32_t FirstDynamicVehicleModelSlot = 612;
inline constexpr std::int32_t LastEngineModelInfoSlot = 19999;
inline constexpr std::int32_t FirstCustomVehicleModelId = 30001;
inline constexpr std::int32_t LastCustomVehicleModelId = 32767;

using NativeModelSlotVacancyQuery = bool (*)(void* user_data, std::int32_t model_slot) noexcept;

enum class ModelSlotReserveResult
{
	Reserved,
	AlreadyReserved,
	InvalidCustomModelId,
	InvalidSlotRange,
	NoFreeSlot,
	ModelRetiring,
	MissingVacancyQuery
};

enum class ModelSlotReleaseResult
{
	Released,
	NotFound,
	SlotMismatch,
	NotRetiring,
	ModelInUse,
	NativeSlotStillOccupied,
	MissingVacancyQuery
};

/*
 * Maps server model IDs (30001-32767) to vacant GTA model-info IDs (612-19999).
 *
 * The vacancy callback must only inspect the native model-info table. It is
 * called while this registry is locked and must not call back into this class.
 * A slot may be released only after the native model-info pointer and every
 * resource owned by that model have been torn down. Active vehicle leases keep
 * a retiring mapping alive until the last vehicle using it is removed.
 *
 * This class never dereferences GTA memory and contains no executable
 * addresses. The Windows native backend supplies the table-inspection callback.
 */
class VehicleModelSlotRegistry
{
public:
	VehicleModelSlotRegistry(
		std::int32_t first_slot = FirstDynamicVehicleModelSlot,
		std::int32_t last_slot = LastEngineModelInfoSlot);

	[[nodiscard]] bool hasValidRange() const noexcept;
	[[nodiscard]] std::int32_t firstSlot() const noexcept;
	[[nodiscard]] std::int32_t lastSlot() const noexcept;

	ModelSlotReserveResult reserve(
		std::int32_t custom_model_id,
		NativeModelSlotVacancyQuery is_slot_vacant,
		void* user_data,
		std::int32_t& native_model_slot);

	[[nodiscard]] bool find(
		std::int32_t custom_model_id,
		std::int32_t& native_model_slot) const;

	// Retain once for each live vehicle whose GTA entity uses this model slot.
	bool retain(std::int32_t custom_model_id, std::int32_t& native_model_slot);

	// Release the matching vehicle lease when that vehicle leaves the client pool.
	bool releaseUse(std::int32_t custom_model_id, std::int32_t native_model_slot);

	// Prevent new vehicles from acquiring this mapping; existing vehicles keep it alive.
	bool retire(std::int32_t custom_model_id);

	[[nodiscard]] bool canRelease(std::int32_t custom_model_id) const;

	// Call only after model-info, DFF/TXD, streaming, and other native references are torn down.
	ModelSlotReleaseResult release(
		std::int32_t custom_model_id,
		std::int32_t expected_native_model_slot,
		NativeModelSlotVacancyQuery is_slot_vacant,
		void* user_data);

	[[nodiscard]] std::size_t size() const;

private:
	static constexpr std::size_t CustomModelCount =
		static_cast<std::size_t>(LastCustomVehicleModelId - FirstCustomVehicleModelId + 1);

	[[nodiscard]] static bool isCustomModelId(std::int32_t model_id) noexcept;
	[[nodiscard]] std::size_t customIndex(std::int32_t model_id) const noexcept;

	const std::int32_t first_slot_;
	const std::int32_t last_slot_;
	const bool valid_range_;
	mutable std::mutex mutex_;
	std::array<std::int32_t, CustomModelCount> custom_to_slot_ {};
	std::array<std::size_t, CustomModelCount> active_vehicle_uses_ {};
	std::array<bool, CustomModelCount> retiring_ {};
	std::vector<std::int32_t> slot_owners_;
	std::size_t next_slot_offset_ = 0;
};
} // namespace custom_vehicle::native
