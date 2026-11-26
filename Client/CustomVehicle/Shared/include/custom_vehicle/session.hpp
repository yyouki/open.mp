#pragma once

#include <custom_vehicle/protocol.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace custom_vehicle::protocol
{
enum class CapabilityState { NotNegotiated, AwaitingAcknowledgement, Ready };
enum class SessionError
{
	None,
	InvalidState,
	InvalidPayload,
	MalformedManifest,
	InvalidVehicleModel,
	DuplicateModelId,
	ConflictingAssetMetadata,
	InconsistentManifest,
	UnexpectedDownloadUrl,
	DownloadUrlMismatch,
	AssetIntegrityFailure,
	UnexpectedDownloadAcknowledgement
};
enum class ManifestDisposition { IgnoredNonVehicle, AcceptedVehicle, Rejected };

struct AssetRequest
{
	AssetType type = AssetType::Dff;
	std::uint32_t checksum = 0;
	std::uint32_t expectedSize = 0;
};

struct AssetDownload
{
	AssetType type = AssetType::Dff;
	std::uint32_t checksum = 0;
	std::uint32_t expectedSize = 0;
	// Borrowed from the RPC callback buffer; copy/use it before callback return.
	std::string_view url;
};

class ClientSession
{
public:
	bool beginNegotiation() noexcept;
	bool acceptCapabilityAcknowledgement(const std::uint8_t* data, std::size_t size,
		SessionError& error) noexcept;
	ManifestDisposition acceptModelManifest(const std::uint8_t* data, std::size_t size,
		DecodeError& decodeError, SessionError& sessionError);
	bool nextAssetRequest(AssetRequest& request) noexcept;
	bool acceptDownloadUrl(const std::uint8_t* data, std::size_t size, AssetDownload& download,
		DecodeError& decodeError, SessionError& sessionError) noexcept;
	bool completeAsset(AssetType type, std::uint32_t checksum, const std::uint8_t* data,
		std::size_t size, SessionError& error) noexcept;
	bool acceptDownloadAcknowledgement(const std::uint8_t* data, std::size_t size,
		SessionError& error) noexcept;
	bool retryActiveAsset() noexcept;
	bool awaitingDownloadUrl() const noexcept;
	void reset() noexcept;

	CapabilityState capabilityState() const noexcept;
	const std::vector<ModelRequest>& vehicleModels() const noexcept;

private:
	enum class AssetState { Queued, Requested, Downloading, AwaitingAcknowledgement, Complete };
	struct PendingAsset
	{
		AssetType type;
		std::uint32_t checksum;
		std::uint32_t expectedSize;
		AssetState state;
	};

	bool hasAssetConflict(AssetType type, std::uint32_t checksum, std::uint32_t size) const noexcept;
	void queueAsset(AssetType type, std::uint32_t checksum, std::uint32_t size);

	CapabilityState capability_ = CapabilityState::NotNegotiated;
	std::vector<ModelRequest> vehicleModels_;
	std::vector<PendingAsset> assets_;
	std::uint32_t manifestCount_ = 0;
	std::size_t activeAssetIndex_ = 0;
	bool hasActiveAsset_ = false;
};
} // namespace custom_vehicle::protocol
