#include <custom_vehicle/session.hpp>

#include <algorithm>

namespace custom_vehicle::protocol
{
bool ClientSession::beginNegotiation() noexcept
{
	reset();
	capability_ = CapabilityState::AwaitingAcknowledgement;
	return true;
}

bool ClientSession::acceptCapabilityAcknowledgement(
	const std::uint8_t* data,
	std::size_t size,
	SessionError& error) noexcept
{
	error = SessionError::None;
	if (capability_ != CapabilityState::AwaitingAcknowledgement)
	{
		error = SessionError::InvalidState;
		return false;
	}
	if (size != 0 || (data == nullptr && size != 0))
	{
		error = SessionError::InvalidPayload;
		return false;
	}
	capability_ = CapabilityState::Ready;
	return true;
}

bool ClientSession::hasAssetConflict(
	AssetType type,
	std::uint32_t checksum,
	std::uint32_t size) const noexcept
{
	return std::any_of(assets_.begin(), assets_.end(),
		[type, checksum, size](const PendingAsset& asset)
		{
			return asset.type == type && asset.checksum == checksum && asset.expectedSize != size;
		});
}

void ClientSession::queueAsset(AssetType type, std::uint32_t checksum, std::uint32_t size)
{
	const auto existing = std::find_if(assets_.begin(), assets_.end(),
		[type, checksum](const PendingAsset& asset)
		{
			return asset.type == type && asset.checksum == checksum;
		});
	if (existing == assets_.end())
	{
		assets_.push_back({ type, checksum, size, AssetState::Queued });
	}
}

ManifestDisposition ClientSession::acceptModelManifest(
	const std::uint8_t* data,
	std::size_t size,
	DecodeError& decodeError,
	SessionError& sessionError)
{
	decodeError = DecodeError::None;
	sessionError = SessionError::None;

	ModelRequest request;
	if (!decodeModelRequest(data, size, request, decodeError))
	{
		sessionError = SessionError::MalformedManifest;
		return ManifestDisposition::Rejected;
	}
	if (request.type != ModelType::Vehicle)
	{
		return ManifestDisposition::IgnoredNonVehicle;
	}
	if (capability_ != CapabilityState::Ready)
	{
		sessionError = SessionError::InvalidState;
		return ManifestDisposition::Rejected;
	}
	if (!validateVehicleModelRequest(request))
	{
		sessionError = SessionError::InvalidVehicleModel;
		return ManifestDisposition::Rejected;
	}

	const auto existingModel = std::find_if(vehicleModels_.begin(), vehicleModels_.end(),
		[&request](const ModelRequest& model) { return model.newId == request.newId; });
	if (existingModel != vehicleModels_.end())
	{
		const bool identical =
			existingModel->poolId == request.poolId
			&& existingModel->modelCount == request.modelCount
			&& existingModel->baseId == request.baseId
			&& existingModel->dffChecksum == request.dffChecksum
			&& existingModel->txdChecksum == request.txdChecksum
			&& existingModel->dffSize == request.dffSize
			&& existingModel->txdSize == request.txdSize
			&& existingModel->virtualWorld == request.virtualWorld
			&& existingModel->timeOn == request.timeOn
			&& existingModel->timeOff == request.timeOff;
		if (identical)
		{
			return ManifestDisposition::AcceptedVehicle;
		}
		sessionError = SessionError::DuplicateModelId;
		return ManifestDisposition::Rejected;
	}

	if (hasAssetConflict(AssetType::Dff, request.dffChecksum, request.dffSize)
		|| hasAssetConflict(AssetType::Txd, request.txdChecksum, request.txdSize))
	{
		sessionError = SessionError::ConflictingAssetMetadata;
		return ManifestDisposition::Rejected;
	}

	vehicleModels_.push_back(request);
	queueAsset(AssetType::Dff, request.dffChecksum, request.dffSize);
	queueAsset(AssetType::Txd, request.txdChecksum, request.txdSize);
	return ManifestDisposition::AcceptedVehicle;
}

bool ClientSession::nextAssetRequest(AssetRequest& request) noexcept
{
	if (capability_ != CapabilityState::Ready || hasActiveAsset_)
	{
		return false;
	}

	for (std::size_t index = 0; index < assets_.size(); ++index)
	{
		PendingAsset& asset = assets_[index];
		if (asset.state != AssetState::Queued)
		{
			continue;
		}

		asset.state = AssetState::Requested;
		activeAssetIndex_ = index;
		hasActiveAsset_ = true;
		request = { asset.type, asset.checksum, asset.expectedSize };
		return true;
	}
	return false;
}

bool ClientSession::acceptDownloadUrl(
	const std::uint8_t* data,
	std::size_t size,
	AssetDownload& download,
	DecodeError& decodeError,
	SessionError& sessionError) noexcept
{
	decodeError = DecodeError::None;
	sessionError = SessionError::None;
	if (!hasActiveAsset_ || activeAssetIndex_ >= assets_.size())
	{
		sessionError = SessionError::UnexpectedDownloadUrl;
		return false;
	}

	PendingAsset& asset = assets_[activeAssetIndex_];
	if (asset.state != AssetState::Requested)
	{
		sessionError = SessionError::InvalidState;
		return false;
	}

	DownloadUrl decoded;
	if (!decodeDownloadUrl(data, size, decoded, decodeError))
	{
		sessionError = SessionError::InvalidPayload;
		return false;
	}
	if (decoded.type != asset.type || decoded.checksum != asset.checksum)
	{
		sessionError = SessionError::DownloadUrlMismatch;
		return false;
	}

	asset.state = AssetState::Downloading;
	download = { decoded.type, decoded.checksum, asset.expectedSize, decoded.url };
	return true;
}

bool ClientSession::completeAsset(
	AssetType type,
	std::uint32_t checksum,
	const std::uint8_t* data,
	std::size_t size,
	SessionError& error) noexcept
{
	error = SessionError::None;
	if (!hasActiveAsset_ || activeAssetIndex_ >= assets_.size())
	{
		error = SessionError::InvalidState;
		return false;
	}

	PendingAsset& asset = assets_[activeAssetIndex_];
	if (asset.state != AssetState::Downloading || asset.type != type || asset.checksum != checksum)
	{
		error = SessionError::InvalidState;
		return false;
	}
	if (!verifyAsset(data, size, asset.expectedSize, asset.checksum))
	{
		// Re-queue corrupt/truncated data and do not send RPC 184.
		asset.state = AssetState::Queued;
		hasActiveAsset_ = false;
		error = SessionError::AssetIntegrityFailure;
		return false;
	}

	asset.state = AssetState::AwaitingAcknowledgement;
	return true;
}

bool ClientSession::acceptDownloadAcknowledgement(
	const std::uint8_t* data,
	std::size_t size,
	SessionError& error) noexcept
{
	error = SessionError::None;
	if (!hasActiveAsset_ || activeAssetIndex_ >= assets_.size())
	{
		error = SessionError::UnexpectedDownloadAcknowledgement;
		return false;
	}
	if (assets_[activeAssetIndex_].state != AssetState::AwaitingAcknowledgement)
	{
		error = SessionError::InvalidState;
		return false;
	}
	if (size != 0 || (data == nullptr && size != 0))
	{
		error = SessionError::InvalidPayload;
		return false;
	}

	assets_[activeAssetIndex_].state = AssetState::Complete;
	hasActiveAsset_ = false;
	return true;
}

void ClientSession::reset() noexcept
{
	capability_ = CapabilityState::NotNegotiated;
	vehicleModels_.clear();
	assets_.clear();
	activeAssetIndex_ = 0;
	hasActiveAsset_ = false;
}

CapabilityState ClientSession::capabilityState() const noexcept
{
	return capability_;
}

const std::vector<ModelRequest>& ClientSession::vehicleModels() const noexcept
{
	return vehicleModels_;
}
} // namespace custom_vehicle::protocol
