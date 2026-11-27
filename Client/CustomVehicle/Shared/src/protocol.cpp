#include <custom_vehicle/protocol.hpp>

#include <cmath>
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

float readFloatLittleEndian(const std::uint8_t* data) noexcept
{
	const std::uint32_t raw = readU32LittleEndian(data);
	float value = 0.0f;
	static_assert(sizeof(value) == sizeof(raw), "32-bit float widths must match");
	std::memcpy(&value, &raw, sizeof(value));
	return value;
}

void writeU32LittleEndian(std::uint8_t* output, std::uint32_t value) noexcept
{
	output[0] = static_cast<std::uint8_t>(value);
	output[1] = static_cast<std::uint8_t>(value >> 8);
	output[2] = static_cast<std::uint8_t>(value >> 16);
	output[3] = static_cast<std::uint8_t>(value >> 24);
}
} // namespace

bool decodeModelRequest(const std::uint8_t* data, std::size_t size,
	ModelRequest& result, DecodeError& error) noexcept
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

	// The server serializes Count as a signed int32; reject negatives before conversion.
	const std::int32_t modelCount = readI32LittleEndian(data + 4);
	if (modelCount <= 0)
	{
		error = DecodeError::InvalidModelCount;
		return false;
	}
	decoded.modelCount = static_cast<std::uint32_t>(modelCount);

	switch (data[8])
	{
		case static_cast<std::uint8_t>(ModelType::Skin): decoded.type = ModelType::Skin; break;
		case static_cast<std::uint8_t>(ModelType::Object): decoded.type = ModelType::Object; break;
		case static_cast<std::uint8_t>(ModelType::Vehicle): decoded.type = ModelType::Vehicle; break;
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

bool decodeStreamInVehicle(
	const std::uint8_t* data,
	std::size_t size,
	StreamInVehicleData& result,
	DecodeError& error) noexcept
{
	error = DecodeError::None;
	if (data == nullptr)
	{
		error = DecodeError::NullBuffer;
		return false;
	}
	if (size != StreamInVehicleWireSize)
	{
		error = DecodeError::WrongSize;
		return false;
	}

	StreamInVehicleData decoded;
	decoded.vehicleId = static_cast<std::uint16_t>(
		static_cast<std::uint32_t>(data[0])
		| (static_cast<std::uint32_t>(data[1]) << 8));
	decoded.modelId = readI32LittleEndian(data + 2);
	if (!((decoded.modelId >= 400 && decoded.modelId <= 611)
		|| isCustomVehicleModelId(decoded.modelId)))
	{
		error = DecodeError::InvalidVehicleModel;
		return false;
	}

	// The first 40 bytes match the packed native VehiclePoolQueueEntry record.
	decoded.position = {
		readFloatLittleEndian(data + 6),
		readFloatLittleEndian(data + 10),
		readFloatLittleEndian(data + 14)
	};
	decoded.angle = readFloatLittleEndian(data + 18);
	decoded.colour1 = data[22];
	decoded.colour2 = data[23];
	decoded.health = readFloatLittleEndian(data + 24);
	decoded.interior = data[28];
	decoded.doorDamage = readU32LittleEndian(data + 29);
	decoded.panelDamage = readU32LittleEndian(data + 33);
	decoded.lightDamage = data[37];
	decoded.tyreDamage = data[38];
	decoded.siren = data[39];

	// The 23-byte tail is 14 one-byte mods, paintjob, and two 32-bit body colours.
	for (std::size_t index = 0; index < StreamInVehicleModCount; ++index)
	{
		decoded.mods[index] = data[40 + index];
	}
	decoded.paintjob = data[54];
	decoded.bodyColour1 = readI32LittleEndian(data + 55);
	decoded.bodyColour2 = readI32LittleEndian(data + 59);

	if (!std::isfinite(decoded.position[0])
		|| !std::isfinite(decoded.position[1])
		|| !std::isfinite(decoded.position[2])
		|| !std::isfinite(decoded.angle)
		|| !std::isfinite(decoded.health))
	{
		error = DecodeError::InvalidVehicleData;
		return false;
	}

	result = decoded;
	return true;
}

bool isValidHttpUrl(std::string_view url) noexcept
{
	constexpr std::string_view http = "http://";
	constexpr std::string_view https = "https://";
	std::size_t authorityStart = 0;

	if (url.empty() || url.size() > MaxDownloadUrlBytes)
	{
		return false;
	}
	if (url.substr(0, http.size()) == http)
	{
		authorityStart = http.size();
	}
	else if (url.substr(0, https.size()) == https)
	{
		authorityStart = https.size();
	}
	else
	{
		return false;
	}

	const std::size_t authorityEnd = url.find_first_of("/?#", authorityStart);
	const std::size_t hostEnd = authorityEnd == std::string_view::npos ? url.size() : authorityEnd;
	if (hostEnd == authorityStart)
	{
		return false;
	}

	for (unsigned char character : url)
	{
		// RPC URLs cross JNI through NewStringUTF; require URI-safe ASCII bytes.
		if (character <= 0x20 || character > 0x7E || character == '\\')
		{
			return false;
		}
	}
	return true;
}

bool decodeDownloadUrl(const std::uint8_t* data, std::size_t size,
	DownloadUrl& result, DecodeError& error) noexcept
{
	error = DecodeError::None;
	if (data == nullptr)
	{
		error = DecodeError::NullBuffer;
		return false;
	}
	if (size < DownloadUrlHeaderSize)
	{
		error = DecodeError::WrongSize;
		return false;
	}
	if (data[0] != ModelUrlTransportMarker)
	{
		error = DecodeError::InvalidUrlMarker;
		return false;
	}

	DownloadUrl decoded;
	switch (data[1])
	{
		case static_cast<std::uint8_t>(AssetType::Dff): decoded.type = AssetType::Dff; break;
		case static_cast<std::uint8_t>(AssetType::Txd): decoded.type = AssetType::Txd; break;
		default:
			error = DecodeError::InvalidAssetType;
			return false;
	}

	decoded.checksum = readU32LittleEndian(data + 2);
	const std::size_t urlLength = data[6];
	if (urlLength == 0 || size != DownloadUrlHeaderSize + urlLength)
	{
		error = DecodeError::InvalidUrlLength;
		return false;
	}
	decoded.url = std::string_view(reinterpret_cast<const char*>(data + DownloadUrlHeaderSize), urlLength);
	if (!isValidHttpUrl(decoded.url))
	{
		error = DecodeError::InvalidUrl;
		return false;
	}

	result = decoded;
	return true;
}

bool encodeAssetRequest(AssetType type, std::uint32_t checksum, std::uint8_t* output,
	std::size_t capacity, std::size_t& bytesWritten, RpcId& rpcId) noexcept
{
	bytesWritten = 0;
	if (output == nullptr || capacity < AssetRequestWireSize)
	{
		return false;
	}

	switch (type)
	{
		case AssetType::Dff: rpcId = RpcId::RequestDff; break;
		case AssetType::Txd: rpcId = RpcId::RequestTxd; break;
		default: return false;
	}

	writeU32LittleEndian(output, checksum);
	bytesWritten = AssetRequestWireSize;
	return true;
}

bool encodeAssetCompletion(AssetType type, std::uint32_t checksum, std::uint8_t* output,
	std::size_t capacity, std::size_t& bytesWritten) noexcept
{
	bytesWritten = 0;
	if (output == nullptr || capacity < AssetCompletionWireSize)
	{
		return false;
	}
	if (type != AssetType::Dff && type != AssetType::Txd)
	{
		return false;
	}

	output[0] = static_cast<std::uint8_t>(type);
	writeU32LittleEndian(output + 1, checksum);
	bytesWritten = AssetCompletionWireSize;
	return true;
}

bool isCustomVehicleModelId(std::int32_t modelId) noexcept
{
	return modelId >= MinCustomVehicleModelId && modelId <= MaxCustomVehicleModelId;
}

bool isSupportedVehicleBaseId(std::int32_t modelId) noexcept
{
	return modelId >= 400 && modelId <= 611
		&& modelId != 449 && modelId != 537 && modelId != 538
		&& modelId != 569 && modelId != 570;
}

bool validateVehicleModelRequest(const ModelRequest& request) noexcept
{
	return request.type == ModelType::Vehicle
		&& isCustomVehicleModelId(request.newId)
		&& isSupportedVehicleBaseId(request.baseId)
		&& request.dffSize != 0 && request.txdSize != 0
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

bool verifyAsset(const std::uint8_t* data, std::size_t size,
	std::uint32_t expectedSize, std::uint32_t expectedChecksum) noexcept
{
	return data != nullptr && size == expectedSize
		&& calculateCrc32(data, size) == expectedChecksum;
}
} // namespace custom_vehicle::protocol
