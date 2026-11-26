#pragma once

#include <cstddef>
#include <cstdint>

namespace custom_vehicle::protocol
{
enum class ModelType : std::uint8_t
{
	Skin = 1,
	Object = 2,
	Vehicle = 3,
};

inline constexpr std::int32_t MinCustomVehicleModelId = 30001;
inline constexpr std::int32_t MaxCustomVehicleModelId = 32767;
inline constexpr std::size_t ModelRequestWireSize = 39;

struct ModelRequest
{
	std::uint32_t poolId = 0;
	std::uint32_t modelCount = 0;
	ModelType type = ModelType::Skin;
	std::int32_t virtualWorld = 0;
	std::int32_t baseId = 0;
	std::int32_t newId = 0;
	std::uint32_t dffChecksum = 0;
	std::uint32_t txdChecksum = 0;
	std::uint32_t dffSize = 0;
	std::uint32_t txdSize = 0;
	std::uint8_t timeOn = 0;
	std::uint8_t timeOff = 0;
};

enum class DecodeError
{
	None,
	NullBuffer,
	WrongSize,
	InvalidModelType,
	InvalidPoolIndex,
};

bool decodeModelRequest(
	const std::uint8_t* data,
	std::size_t size,
	ModelRequest& result,
	DecodeError& error) noexcept;

bool isCustomVehicleModelId(std::int32_t modelId) noexcept;
bool isSupportedVehicleBaseId(std::int32_t modelId) noexcept;
bool validateVehicleModelRequest(const ModelRequest& request) noexcept;

std::uint32_t calculateCrc32(const std::uint8_t* data, std::size_t size) noexcept;
bool verifyAsset(
	const std::uint8_t* data,
	std::size_t size,
	std::uint32_t expectedSize,
	std::uint32_t expectedChecksum) noexcept;
} // namespace custom_vehicle::protocol
