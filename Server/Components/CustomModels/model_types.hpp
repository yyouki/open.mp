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
  return modelId >= MIN_CUSTOM_VEHICLE_MODEL_ID &&
         modelId <= MAX_CUSTOM_VEHICLE_MODEL_ID;
}
} // namespace CustomModels
