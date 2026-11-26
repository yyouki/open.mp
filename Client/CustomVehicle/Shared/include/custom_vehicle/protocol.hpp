#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace custom_vehicle::protocol
{
enum class ModelType : std::uint8_t { Skin = 1, Object = 2, Vehicle = 3 };
enum class RpcId : std::uint8_t
{
	ModelRequest = 220,
	ClientSupportsCustomVehicleModels = 221,
	ServerSupportsCustomVehicleModels = 222,
	RequestDff = 223,
	RequestTxd = 224,
	ModelUrl = 225,
	FinishDownload = 226,
	DownloadCompleted = 227,
};
enum class AssetType : std::uint8_t { Dff = 1, Txd = 2 };

inline constexpr std::int32_t MinCustomVehicleModelId = 30001;
inline constexpr std::int32_t MaxCustomVehicleModelId = 32767;
inline constexpr std::size_t ModelRequestWireSize = 39;
inline constexpr std::uint8_t ModelUrlTransportMarker = 6;
inline constexpr std::size_t DownloadUrlHeaderSize = 7;
inline constexpr std::size_t MaxDownloadUrlBytes = 255;
inline constexpr std::size_t AssetRequestWireSize = 4;

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

struct DownloadUrl
{
	AssetType type = AssetType::Dff;
	std::uint32_t checksum = 0;
	// Borrowed from the RPC payload; consume or copy it before callback return.
	std::string_view url;
};

enum class DecodeError
{
	None, NullBuffer, WrongSize, InvalidModelType, InvalidModelCount,
	InvalidPoolIndex, InvalidUrlMarker, InvalidAssetType, InvalidUrlLength,
	InvalidUrl, InvalidOutputBuffer
};

bool decodeModelRequest(const std::uint8_t* data, std::size_t size, ModelRequest& result, DecodeError& error) noexcept;
bool decodeDownloadUrl(const std::uint8_t* data, std::size_t size, DownloadUrl& result, DecodeError& error) noexcept;
bool encodeAssetRequest(AssetType type, std::uint32_t checksum, std::uint8_t* output,
	std::size_t capacity, std::size_t& bytesWritten, RpcId& rpcId) noexcept;
bool isValidHttpUrl(std::string_view url) noexcept;
bool isCustomVehicleModelId(std::int32_t modelId) noexcept;
bool isSupportedVehicleBaseId(std::int32_t modelId) noexcept;
bool validateVehicleModelRequest(const ModelRequest& request) noexcept;
std::uint32_t calculateCrc32(const std::uint8_t* data, std::size_t size) noexcept;
bool verifyAsset(const std::uint8_t* data, std::size_t size,
	std::uint32_t expectedSize, std::uint32_t expectedChecksum) noexcept;
} // namespace custom_vehicle::protocol
