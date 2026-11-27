#pragma once

#include <Server/Components/CustomModels/custommodels.hpp>

namespace CustomModels
{
// Wire value 3 is reserved for the experimental vehicle model type.
// The public SDK enum extension will be delivered as a separate change.
constexpr ModelType Vehicle = static_cast<ModelType>(3);

// Keep custom vehicle IDs outside the current skin and object ID ranges.
constexpr int MIN_CUSTOM_VEHICLE_MODEL_ID = 30001;
constexpr int MAX_CUSTOM_VEHICLE_MODEL_ID = 32767;

inline bool isCustomVehicleModelId(int modelId)
{
	return modelId >= MIN_CUSTOM_VEHICLE_MODEL_ID
		&& modelId <= MAX_CUSTOM_VEHICLE_MODEL_ID;
}

// Train locomotives, rail vehicles, and train carriages need dedicated
// consist/streaming behavior and are not valid replacement bases yet.
inline bool isSupportedBaseVehicleModel(int modelId)
{
	return modelId >= 400 && modelId <= 611
		&& modelId != 449 && modelId != 537 && modelId != 538
		&& modelId != 569 && modelId != 570;
}
} // namespace CustomModels

// Per-player opt-in state for the open.mp custom vehicle model extension.
struct IPlayerCustomVehicleModelsData : public IExtension
{
	PROVIDE_EXT_UID(0x4B9E2D31A765C80F)

	virtual bool supportsCustomVehicleModels() const = 0;

	// Reserve the requested asset before a redirected vehicle URL is sent.
	virtual bool beginPendingDownload(ModelDownloadType type, uint32_t checksum) = 0;
};
