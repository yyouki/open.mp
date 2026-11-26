#include <custom_vehicle/network_adapter.hpp>

#include <array>

namespace custom_vehicle::protocol
{
bool ClientNetworkAdapter::beginConnection() noexcept
{
	session_.beginNegotiation();
	if (!transport_.sendRpc(RpcId::ClientSupportsCustomVehicleModels, nullptr, 0))
	{
		session_.reset();
		return false;
	}
	return true;
}

RpcDisposition ClientNetworkAdapter::handleServerRpc(
	std::uint8_t rpcId,
	const std::uint8_t* payload,
	std::size_t size) noexcept
{
	switch (static_cast<RpcId>(rpcId))
	{
		case RpcId::ServerSupportsCustomVehicleModels:
		{
			SessionError error = SessionError::None;
			session_.acceptCapabilityAcknowledgement(payload, size, error);
			// RPC 187 belongs to this extension and must not reach the stock client.
			return RpcDisposition::Consume;
		}
		case RpcId::ModelUrl:
		{
			AssetDownload download;
			DecodeError decodeError = DecodeError::None;
			SessionError sessionError = SessionError::None;
			if (!session_.acceptDownloadUrl(payload, size, download, decodeError, sessionError))
			{
				return RpcDisposition::ForwardToGame;
			}

			// The sink copies the callback-scoped URL and schedules the download asynchronously.
			if (!downloader_.beginDownload(download))
			{
				retryActiveAsset();
			}
			return RpcDisposition::Consume;
		}
		case RpcId::DownloadCompleted:
		{
			SessionError error = SessionError::None;
			if (!session_.acceptDownloadAcknowledgement(payload, size, error))
			{
				// This may be the acknowledgement for a native 0.3.DL asset transfer.
				return RpcDisposition::ForwardToGame;
			}
			requestNextAsset();
			return RpcDisposition::Consume;
		}
		case RpcId::ModelRequest:
		{
			// Keep native skin/object records on the original 0.3.DL artwork path.
			if (payload == nullptr || size <= 8
				|| payload[8] != static_cast<std::uint8_t>(ModelType::Vehicle))
			{
				return RpcDisposition::ForwardToGame;
			}

			try
			{
				DecodeError decodeError = DecodeError::None;
				SessionError sessionError = SessionError::None;
				const ManifestDisposition disposition =
					session_.acceptModelManifest(payload, size, decodeError, sessionError);
				if (disposition == ManifestDisposition::AcceptedVehicle)
				{
					requestNextAsset();
				}
			}
			catch (...)
			{
				// Fail closed for type 3 so the stock game parser never sees an unknown model kind.
			}
			return RpcDisposition::Consume;
		}
		default:
			return RpcDisposition::ForwardToGame;
	}
}

bool ClientNetworkAdapter::requestNextAsset() noexcept
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
	SessionError error = SessionError::None;
	if (!session_.completeAsset(type, checksum, data, size, error))
	{
		// An integrity failure re-queues the active asset for another URL request.
		requestNextAsset();
		return false;
	}

	if (!transport_.sendRpc(RpcId::FinishDownload, nullptr, 0))
	{
		session_.retryActiveAsset();
		requestNextAsset();
		return false;
	}
	return true;
}

bool ClientNetworkAdapter::retryActiveAsset() noexcept
{
	if (!session_.retryActiveAsset())
	{
		return false;
	}
	return requestNextAsset();
}
} // namespace custom_vehicle::protocol
