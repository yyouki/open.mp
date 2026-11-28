#include <custom_vehicle/txd_slot_registry.hpp>

namespace custom_vehicle::native
{
TxdResourceSlotRegistry::TxdResourceSlotRegistry(
    std::int32_t first_slot,
    std::int32_t last_slot)
    : first_slot_(first_slot)
    , last_slot_(last_slot)
    , valid_range_(
        first_slot >= 0
        && last_slot < TxdPoolSlotCount
        && first_slot <= last_slot)
    , slot_owners_(valid_range_
        ? static_cast<std::size_t>(last_slot - first_slot + 1)
        : 0, -1)
{
    custom_to_slot_.fill(-1);
}

bool TxdResourceSlotRegistry::hasValidRange() const noexcept { return valid_range_; }
std::int32_t TxdResourceSlotRegistry::firstSlot() const noexcept { return first_slot_; }
std::int32_t TxdResourceSlotRegistry::lastSlot() const noexcept { return last_slot_; }

TxdSlotReserveResult TxdResourceSlotRegistry::reserve(
    std::int32_t custom_model_id,
    NativeTxdSlotAllocate allocate_slot,
    NativeTxdSlotRollback rollback_slot,
    void* user_data,
    std::int32_t& txd_pool_slot,
    std::int32_t& txd_resource_id)
{
    if (!isCustomModelId(custom_model_id)) return TxdSlotReserveResult::InvalidCustomModelId;
    if (!valid_range_) return TxdSlotReserveResult::InvalidSlotRange;

    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t model_index = customIndex(custom_model_id);
    const std::int32_t existing_slot = custom_to_slot_[model_index];
    if (existing_slot >= 0) {
        if (retiring_[model_index]) return TxdSlotReserveResult::ModelRetiring;
        txd_pool_slot = existing_slot;
        txd_resource_id = FirstTxdResourceId + existing_slot;
        return TxdSlotReserveResult::AlreadyReserved;
    }
    if (allocate_slot == nullptr) return TxdSlotReserveResult::MissingAllocator;
    if (rollback_slot == nullptr) return TxdSlotReserveResult::MissingRollback;

    std::int32_t allocated_slot = -1;
    if (!allocate_slot(user_data, allocated_slot)) return TxdSlotReserveResult::AllocationFailed;
    if (!isConfiguredSlot(allocated_slot)) {
        if (allocated_slot >= 0 && allocated_slot < TxdPoolSlotCount) rollback_slot(user_data, allocated_slot);
        return TxdSlotReserveResult::InvalidAllocatedSlot;
    }

    const std::size_t offset = static_cast<std::size_t>(allocated_slot - first_slot_);
    if (slot_owners_[offset] >= 0) {
        // Never free a slot that an existing mapping already owns.
        return TxdSlotReserveResult::SlotAlreadyOwned;
    }

    slot_owners_[offset] = custom_model_id;
    custom_to_slot_[model_index] = allocated_slot;
    retiring_[model_index] = false;
    txd_pool_slot = allocated_slot;
    txd_resource_id = FirstTxdResourceId + allocated_slot;
    return TxdSlotReserveResult::Reserved;
}

bool TxdResourceSlotRegistry::find(
    std::int32_t custom_model_id,
    std::int32_t& txd_pool_slot,
    std::int32_t& txd_resource_id) const
{
    if (!isCustomModelId(custom_model_id)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::int32_t slot = custom_to_slot_[customIndex(custom_model_id)];
    if (slot < 0) return false;
    txd_pool_slot = slot;
    txd_resource_id = FirstTxdResourceId + slot;
    return true;
}

bool TxdResourceSlotRegistry::retire(std::int32_t custom_model_id)
{
    if (!isCustomModelId(custom_model_id)) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t index = customIndex(custom_model_id);
    if (custom_to_slot_[index] < 0) return false;
    retiring_[index] = true;
    return true;
}

TxdSlotReleaseResult TxdResourceSlotRegistry::release(
    std::int32_t custom_model_id,
    std::int32_t expected_txd_pool_slot,
    NativeTxdSlotVacancyQuery is_slot_vacant,
    void* user_data)
{
    if (!isCustomModelId(custom_model_id)) return TxdSlotReleaseResult::NotFound;
    if (is_slot_vacant == nullptr) return TxdSlotReleaseResult::MissingVacancyQuery;

    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t model_index = customIndex(custom_model_id);
    const std::int32_t current_slot = custom_to_slot_[model_index];
    if (current_slot < 0) return TxdSlotReleaseResult::NotFound;
    if (current_slot != expected_txd_pool_slot) return TxdSlotReleaseResult::SlotMismatch;
    if (!retiring_[model_index]) return TxdSlotReleaseResult::NotRetiring;

    const std::size_t offset = static_cast<std::size_t>(current_slot - first_slot_);
    if (offset >= slot_owners_.size() || slot_owners_[offset] != custom_model_id)
        return TxdSlotReleaseResult::SlotMismatch;
    if (!is_slot_vacant(user_data, current_slot))
        return TxdSlotReleaseResult::NativeSlotStillOccupied;

    slot_owners_[offset] = -1;
    custom_to_slot_[model_index] = -1;
    retiring_[model_index] = false;
    return TxdSlotReleaseResult::Released;
}

std::size_t TxdResourceSlotRegistry::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t count = 0;
    for (const std::int32_t slot : custom_to_slot_) if (slot >= 0) ++count;
    return count;
}

bool TxdResourceSlotRegistry::isCustomModelId(std::int32_t model_id) noexcept
{
    return model_id >= FirstCustomVehicleModelId && model_id <= LastCustomVehicleModelId;
}
std::size_t TxdResourceSlotRegistry::customIndex(std::int32_t model_id) const noexcept
{
    return static_cast<std::size_t>(model_id - FirstCustomVehicleModelId);
}
bool TxdResourceSlotRegistry::isConfiguredSlot(std::int32_t slot) const noexcept
{
    return slot >= first_slot_ && slot <= last_slot_;
}
} // namespace custom_vehicle::native
