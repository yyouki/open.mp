#pragma once

#include <custom_vehicle/model_slot_registry.hpp>
#include <custom_vehicle/protocol.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace custom_vehicle::native
{
enum class VehicleModelResourceStage : std::uint8_t
{
	Reserved,
	ModelInfoCreated,
	AssetsRequested,
	AssetsLoaded,
	ClumpAttached
};

enum class VehicleAssetLoadResult : std::uint8_t
{
	Failed,
	Pending,
	Ready
};

enum class VehicleModelBackendResult
{
	Activated,
	AlreadyActive,
	AssetsPending,
	NativeAssetsPending,
	InvalidRequest,
	MissingOperations,
	NoFreeSlot,
	ModelRetiring,
	ModelReplacementRequired,
	CreateModelInfoFailed,
	LoadAssetsFailed,
	AttachClumpFailed,
	ModelInUse,
	CleanupPending,
	Released,
	NotFound
};

inline constexpr std::size_t VehicleModelBindingVehicleIdLimit = 2000;

enum class VehicleModelBindingResult
{
	Bound,
	AlreadyBound,
	InvalidVehicleId,
	InvalidCustomModelId,
	ModelNotActive,
	VehicleAlreadyBound,
	GenerationExhausted
};

/*
 * A generation-tagged mapping from an SA-MP vehicle-pool ID to one active
 * custom model. The generation prevents a delayed removal for a reused pool
 * ID from releasing a newer vehicle's lease.
 */
struct VehicleModelBinding
{
	std::uint16_t vehicle_id = 0;
	std::int32_t custom_model_id = -1;
	std::int32_t native_model_slot = -1;
	std::uint64_t generation = 0;
};

/*
 * Platform callbacks used by the model lifecycle controller.
 *
 * Every callback is invoked synchronously on the caller's thread. The Windows
 * backend must call this controller only from a positively identified GTA game
 * thread after the original CGame::Process has completed. These callbacks must
 * not re-enter VehicleModelLifecycleController.
 *
 * create_model_info and attach_vehicle_clump are synchronous. The asset callback
 * is called once to request streaming and again on later game ticks to poll it;
 * Pending keeps the model-info slot reserved without blocking the game thread.
 * Failed callbacks must not leave resources from their failed operation behind.
 * destroy_model must be idempotent and clear every resource acquired up to
 * completed_stage, including pending streaming requests, model-info table entry,
 * RenderWare clump, vehicle structure, streaming metadata, and TXD references.
 * It returns true only when teardown is complete; false keeps the slot retired
 * and reserved so a later retry can finish cleanup without reusing it.
 *
 * is_slot_vacant must inspect the native model-info table and must be safe to
 * call while the controller holds its registry lock.
 */
struct VehicleModelNativeOperations
{
	NativeModelSlotVacancyQuery is_slot_vacant = nullptr;
	bool (*create_model_info)(
		void* user_data,
		std::int32_t native_model_slot,
		const protocol::ModelRequest& model) noexcept = nullptr;
	VehicleAssetLoadResult (*request_or_poll_vehicle_assets)(
		void* user_data,
		std::int32_t native_model_slot,
		const protocol::ModelRequest& model) noexcept = nullptr;
	bool (*attach_vehicle_clump)(
		void* user_data,
		std::int32_t native_model_slot,
		const protocol::ModelRequest& model) noexcept = nullptr;
	bool (*destroy_model)(
		void* user_data,
		std::int32_t native_model_slot,
		const protocol::ModelRequest& model,
		VehicleModelResourceStage completed_stage) noexcept = nullptr;
};

class VehicleModelLifecycleController
{
public:
	explicit VehicleModelLifecycleController(
		std::int32_t first_slot = FirstDynamicVehicleModelSlot,
		std::int32_t last_slot = LastEngineModelInfoSlot);

	[[nodiscard]] bool hasValidSlotRange() const noexcept;

	VehicleModelBackendResult activate(
		const protocol::ModelRequest& model,
		bool assets_ready,
		const VehicleModelNativeOperations& operations,
		void* user_data,
		std::int32_t& native_model_slot);

	bool retainForVehicle(
		std::int32_t custom_model_id,
		std::int32_t& native_model_slot);

	bool releaseVehicleUse(
		std::int32_t custom_model_id,
		std::int32_t native_model_slot);

	VehicleModelBindingResult bindVehicle(
		std::uint16_t vehicle_id,
		std::int32_t custom_model_id,
		VehicleModelBinding& binding);

	bool releaseVehicleBinding(const VehicleModelBinding& binding);

	[[nodiscard]] bool findVehicleBinding(
		std::uint16_t vehicle_id,
		VehicleModelBinding& binding) const;

	VehicleModelBackendResult retire(
		std::int32_t custom_model_id,
		const VehicleModelNativeOperations& operations,
		void* user_data);

	VehicleModelBackendResult retryRetiredCleanup(
		std::int32_t custom_model_id,
		const VehicleModelNativeOperations& operations,
		void* user_data);

	[[nodiscard]] bool find(
		std::int32_t custom_model_id,
		std::int32_t& native_model_slot) const;

	[[nodiscard]] bool isActive(std::int32_t custom_model_id) const;

	[[nodiscard]] std::size_t size() const;

private:
	static constexpr std::size_t CustomModelCount =
		static_cast<std::size_t>(LastCustomVehicleModelId - FirstCustomVehicleModelId + 1);

	struct Entry
	{
		protocol::ModelRequest model {};
		std::int32_t native_model_slot = -1;
		VehicleModelResourceStage stage = VehicleModelResourceStage::Reserved;
		std::size_t anonymous_vehicle_uses = 0;
		bool present = false;
		bool retiring = false;
	};

	[[nodiscard]] static bool isCustomModelId(std::int32_t model_id) noexcept;
	[[nodiscard]] static std::size_t customIndex(std::int32_t model_id) noexcept;
	[[nodiscard]] static bool hasRequiredOperations(
		const VehicleModelNativeOperations& operations) noexcept;

	VehicleModelBackendResult rollbackFailedActivation(
		Entry& entry,
		std::int32_t custom_model_id,
		VehicleModelBackendResult original_failure,
		const VehicleModelNativeOperations& operations,
		void* user_data);

	VehicleModelBackendResult cleanupRetired(
		Entry& entry,
		std::int32_t custom_model_id,
		const VehicleModelNativeOperations& operations,
		void* user_data);

	mutable std::mutex mutex_;
	VehicleModelSlotRegistry slots_;
	std::array<Entry, CustomModelCount> entries_ {};
	std::array<VehicleModelBinding, VehicleModelBindingVehicleIdLimit> vehicle_bindings_ {};
	std::uint64_t next_binding_generation_ = 1;
};
} // namespace custom_vehicle::native
