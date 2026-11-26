#include <custom_vehicle/protocol.hpp>

#include <cstring>

namespace custom_vehicle::protocol
{
namespace
{
std::uint32_t readU32LittleEndian(const std::uint8_t* data) noexcept
{
	return static_cast<std::uint32_t>(data[0])
		| (static_cast<std::uint32_t>(data[1]) << 8)
		| (static_cast<std::uint32_t>(data[2]) << 16)
		| (static_cast<std::uint32_t>(data[3]) << 24);
}

std::int32_t readI32LittleEndian(const std::uint8_t* data) noexcept
{
	const std::uint32_t raw = readU32LittleEndian(data);
	std::int32_t value = 0;
	static_assert(sizeof(value) == sizeof(raw), "32-bit integer widths must match");
	std::memcpy(&value, &raw, sizeof(value));
	return value;
}
} // namespace

bool decodeModelRequest(
	const std::uint8_t* data,
	std::size_t size,
	ModelRequest& result,
	DecodeError& error) noexcept
{
	error = DecodeError::None;

	if (data == nullptr)
	{
		error = DecodeError::NullBuffer;
		return false;
	}
	if (size != ModelRequestWireSize)
	{
		error = DecodeError::WrongSize;
		return false;
	}

	ModelRequest decoded;
	decoded.poolId = readU32LittleEndian(data);

	// The server serializes Count as a signed int32; reject negative values before conversion.
	const std::int32_t modelCount = readI32LittleEndian(data + 4);
	if (modelCount <= 0)
	{
		error = DecodeError::InvalidModelCount;
		return false;
	}
	decoded.modelCount = static_cast<std::uint32_t>(modelCount);

	switch (data[8])
	{
		case static_cast<std::uint8_t>(ModelType::Skin):
			decoded.type = ModelType::Skin;
			break;
		case static_cast<std::uint8_t>(ModelType::Object):
			decoded.type = ModelType::Object;
			break;
		case static_cast<std::uint8_t>(ModelType::Vehicle):
			decoded.type = ModelType::Vehicle;
			break;
		default:
			error = DecodeError::InvalidModelType;
			return false;
	}

	decoded.virtualWorld = readI32LittleEndian(data + 9);
	decoded.baseId = readI32LittleEndian(data + 13);
	decoded.newId = readI32LittleEndian(data + 17);
	decoded.dffChecksum = readU32LittleEndian(data + 21);
	decoded.txdChecksum = readU32LittleEndian(data + 25);
	decoded.dffSize = readU32LittleEndian(data + 29);
	decoded.txdSize = readU32LittleEndian(data + 33);
	decoded.timeOn = data[37];
	decoded.timeOff = data[38];

	if (decoded.poolId >= decoded.modelCount)
	{
		error = DecodeError::InvalidPoolIndex;
		return false;
	}

	result = decoded;
	return true;
}

bool isCustomVehicleModelId(std::int32_t modelId) noexcept
{
	return modelId >= MinCustomVehicleModelId && modelId <= MaxCustomVehicleModelId;
}

bool isSupportedVehicleBaseId(std::int32_t modelId) noexcept
{
	return modelId >= 400 && modelId <= 611
		&& modelId != 449
		&& modelId != 537
		&& modelId != 538
		&& modelId != 569
		&& modelId != 570;
}

bool validateVehicleModelRequest(const ModelRequest& request) noexcept
{
	return request.type == ModelType::Vehicle
		&& isCustomVehicleModelId(request.newId)
		&& isSupportedVehicleBaseId(request.baseId)
		&& request.dffSize != 0
		&& request.txdSize != 0
		&& request.dffChecksum != request.txdChecksum;
}

std::uint32_t calculateCrc32(const std::uint8_t* data, std::size_t size) noexcept
{
	if (data == nullptr && size != 0)
	{
		return 0;
	}

	std::uint32_t crc = 0xFFFFFFFFu;
	for (std::size_t index = 0; index < size; ++index)
	{
		crc ^= data[index];
		for (unsigned bit = 0; bit < 8; ++bit)
		{
			const std::uint32_t mask = 0u - (crc & 1u);
			crc = (crc >> 1) ^ (0xEDB88320u & mask);
		}
	}
	return ~crc;
}

bool verifyAsset(
	const std::uint8_t* data,
	std::size_t size,
	std::uint32_t expectedSize,
	std::uint32_t expectedChecksum) noexcept
{
	return data != nullptr
		&& size == expectedSize
		&& calculateCrc32(data, size) == expectedChecksum;
}
} // namespace custom_vehicle::protocol
