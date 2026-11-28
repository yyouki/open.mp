#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include <custom_vehicle/model_slot_registry.hpp>

namespace custom_vehicle::native
{
inline constexpr std::int32_t FirstTxdResourceId = 20000;
inline constexpr std::int32_t LastTxdResourceId = 24999;
inline constexpr std::int32_t TxdPoolSlotCount = 5000;

using NativeTxdSlotAllocate = bool (*)(void* user_data, std::int32_t& txd_pool_slot) noexcept;
using NativeTxdSlotRollback = void (*)(void* user_data, std::int32_t txd_pool_slot) noexcept;
using NativeTxdSlotVacancyQuery = bool (*)(void* user_data, std::int32_t txd_pool_slot) noexcept;

enum class TxdSlotReserveResult
{
    Reserved,
    AlreadyReserved,
    InvalidCustomModelId,
    InvalidSlotRange,
    MissingAllocator,
    MissingRollback,
    AllocationFailed,
    InvalidAllocatedSlot,
    SlotAlreadyOwned,
    ModelRetiring
};

enum class TxdSlotReleaseResult
{
    Released,
    NotFound,
    SlotMismatch,
    NotRetiring,
    NativeSlotStillOccupied,
    MissingVacancyQuery
};

/*
 * Maps a custom server model ID to a native CTxdPool slot and its TXD streaming
 * resource ID (20000 + pool slot).
 *
 * The allocator, rollback, and vacancy callbacks run synchronously on the GTA
 * game thread. On GTA SA 1.0 US the allocator must check
 * CTxdPool::GetNoOfFreeSpaces() before calling CTxdStore::AddTxdSlot(), whose
 * stock implementation dereferences CPool::New() without checking null.
 * The rollback callback releases a newly allocated slot rejected by this
 * registry. A slot already owned by another mapping is never rolled back:
 * doing so could destroy the existing owner's dictionary. Callbacks must not
 * re-enter this registry.
 *
 * The DFF model lifecycle owns vehicle leases and TXD reference counts. Retire
 * a TXD mapping only after the DFF owner stops accepting vehicle bindings;
 * release it only after CTxdStore::RemoveTxdSlot has freed the pool entry and
 * the vacancy callback confirms it is free.
 *
 * This portable class has no GTA memory access or executable addresses. The
 * Windows x86 native backend supplies the callbacks.
 */
class TxdResourceSlotRegistry
{
public:
    TxdResourceSlotRegistry(
        std::int32_t first_slot = 0,
        std::int32_t last_slot = TxdPoolSlotCount - 1);

    [[nodiscard]] bool hasValidRange() const noexcept;
    [[nodiscard]] std::int32_t firstSlot() const noexcept;
    [[nodiscard]] std::int32_t lastSlot() const noexcept;

    TxdSlotReserveResult reserve(
        std::int32_t custom_model_id,
        NativeTxdSlotAllocate allocate_slot,
        NativeTxdSlotRollback rollback_slot,
        void* user_data,
        std::int32_t& txd_pool_slot,
        std::int32_t& txd_resource_id);

    [[nodiscard]] bool find(
        std::int32_t custom_model_id,
        std::int32_t& txd_pool_slot,
        std::int32_t& txd_resource_id) const;

    bool retire(std::int32_t custom_model_id);

    TxdSlotReleaseResult release(
        std::int32_t custom_model_id,
        std::int32_t expected_txd_pool_slot,
        NativeTxdSlotVacancyQuery is_slot_vacant,
        void* user_data);

    [[nodiscard]] std::size_t size() const;

private:
    static constexpr std::size_t CustomModelCount =
        static_cast<std::size_t>(LastCustomVehicleModelId - FirstCustomVehicleModelId + 1);

    [[nodiscard]] static bool isCustomModelId(std::int32_t model_id) noexcept;
    [[nodiscard]] std::size_t customIndex(std::int32_t model_id) const noexcept;
    [[nodiscard]] bool isConfiguredSlot(std::int32_t slot) const noexcept;

    const std::int32_t first_slot_;
    const std::int32_t last_slot_;
    const bool valid_range_;
    mutable std::mutex mutex_;
    std::array<std::int32_t, CustomModelCount> custom_to_slot_ {};
    std::array<bool, CustomModelCount> retiring_ {};
    std::vector<std::int32_t> slot_owners_;
};
} // namespace custom_vehicle::native
