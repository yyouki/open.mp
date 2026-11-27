#include <custom_vehicle/network_adapter.hpp>

#include <array>
#include <utility>

namespace custom_vehicle::protocol
{
bool ClientNetworkAdapter::beginConnection() noexcept
{
	std::lock_guard lock(mutex_);
	session_.beginNegotiation();
	if (!transport_.sendRpc(RpcId::ClientSupportsCustomVehicleModels, nullptr, 0))
	{
		session_.reset();
		return false;
	}
	return true;
}

void ClientNetworkAdapter::endConnection() noexcept
{
	std::lock_guard lock(mutex_);
	session_.reset();
}

RpcDisposition ClientNetworkAdapter::handleServerRpc(
	std::uint8_t rpcId,
	const std::uint8_t* payload,
	std::size_t size) noexcept
{
	std::lock_guard lock(mutex_);

	switch (static_cast<RpcId>(rpcId))
	{
		case RpcId::ServerSupportsCustomVehicleModels:
		{
			SessionError error = SessionError::None;
			session_.acceptCapabilityAcknowledgement(payload, size, error);
			// RPC 222 belongs only to the custom vehicle extension.
			return RpcDisposition::Consume;
		}
		case RpcId::ModelUrl:
		{
			AssetDownload download;
			DecodeError decodeError = DecodeError::None;
			SessionError sessionError = SessionError::None;
			if (!session_.acceptDownloadUrl(payload, size, download, decodeError, sessionError))
			{
				// RPC 225 is extension-only; malformed custom payloads are always consumed.
				return RpcDisposition::Consume;
			}

			// The sink must copy the URL and enqueue I/O without re-entering this adapter.
			if (!downloader_.beginDownload(download))
			{
				// Leave the asset queued; the platform host applies backoff before retrying.
				session_.retryActiveAsset();
			}
			return RpcDisposition::Consume;
		}
		case RpcId::DownloadCompleted:
		{
			SessionError error = SessionError::None;
			if (!session_.acceptDownloadAcknowledgement(payload, size, error))
			{
				// RPC 227 is extension-only; never pass its malformed payload to the game.
				return RpcDisposition::Consume;
			}
			requestNextAssetUnlocked();
			return RpcDisposition::Consume;
		}
		case RpcId::ClientSupportsCustomVehicleModels:
		case RpcId::RequestDff:
		case RpcId::RequestTxd:
		case RpcId::FinishDownload:
			// These extension IDs are client-to-server only; never forward them to the stock game.
			return RpcDisposition::Consume;
		case RpcId::ModelRequest:
		{
			// RPC 220 is reserved for vehicle manifests; native artwork stays on native RPC 179.
			if (payload == nullptr || size <= 8
				|| payload[8] != static_cast<std::uint8_t>(ModelType::Vehicle))
			{
				return RpcDisposition::Consume;
			}

			try
			{
				DecodeError decodeError = DecodeError::None;
				SessionError sessionError = SessionError::None;
				const ManifestDisposition disposition =
					session_.acceptModelManifest(payload, size, decodeError, sessionError);
				if (disposition == ManifestDisposition::AcceptedVehicle)
				{
					requestNextAssetUnlocked();
				}
			}
			catch (...)
			{
				// Fail closed so an invalid extension record never reaches the stock parser.
			}
			return RpcDisposition::Consume;
		}
		default:
			return RpcDisposition::ForwardToGame;
	}
}

bool ClientNetworkAdapter::requestNextAssetUnlocked() noexcept
{
	AssetRequest request;
	if (!session_.nextAssetRequest(request))
	{
		return true;
	}

	std::array<std::uint8_t, AssetRequestWireSize> payload {};
	std::size_t bytesWritten = 0;
	RpcId rpcId = RpcId::RequestDff;
	if (!encodeAssetRequest(request.type, request.checksum,
		payload.data(), payload.size(), bytesWritten, rpcId))
	{
		session_.retryActiveAsset();
		return false;
	}

	if (!transport_.sendRpc(rpcId, payload.data(), bytesWritten))
	{
		session_.retryActiveAsset();
		return false;
	}
	return true;
}

bool ClientNetworkAdapter::completeAsset(
	AssetType type,
	std::uint32_t checksum,
	const std::uint8_t* data,
	std::size_t size) noexcept
{
	std::lock_guard lock(mutex_);
	SessionError error = SessionError::None;
	if (!session_.completeAsset(type, checksum, data, size, error))
	{
		// An integrity failure re-queues the active asset for another URL request.
		requestNextAssetUnlocked();
		return false;
	}

	std::array<std::uint8_t, AssetCompletionWireSize> completionPayload {};
	std::size_t bytesWritten = 0;
	if (!encodeAssetCompletion(type, checksum, completionPayload.data(),
		completionPayload.size(), bytesWritten)
		|| !transport_.sendRpc(RpcId::FinishDownload, completionPayload.data(), bytesWritten))
	{
		session_.retryActiveAsset();
		requestNextAssetUnlocked();
		return false;
	}
	return true;
}

bool ClientNetworkAdapter::retryActiveAsset() noexcept
{
	std::lock_guard lock(mutex_);

	// Re-queue an in-flight transfer, or dispatch an asset already re-queued after send failure.
	session_.retryActiveAsset();
	return requestNextAssetUnlocked();
}

std::vector<ModelRequest> ClientNetworkAdapter::vehicleModels() const
{
	std::lock_guard lock(mutex_);
	return session_.vehicleModels();
}

bool ClientNetworkAdapter::isVehicleModelReady(std::int32_t modelId) const
{
	std::lock_guard lock(mutex_);
	return session_.isVehicleModelReady(modelId);
}

bool ClientNetworkAdapter::resolveVehicleModel(
	std::int32_t modelId,
	ModelRequest& model,
	bool& ready) const
{
	std::lock_guard lock(mutex_);
	const auto& models = session_.vehicleModels();
	const auto found = std::find_if(models.begin(), models.end(),
		[modelId](const ModelRequest& entry) { return entry.newId == modelId; });
	if (found == models.end())
	{
		return false;
	}

	model = *found;
	ready = session_.isVehicleModelReady(modelId);
	return true;
}
} // namespace custom_vehicle::protocol
