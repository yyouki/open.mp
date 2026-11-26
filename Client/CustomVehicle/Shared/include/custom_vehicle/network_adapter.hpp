#pragma once

#include <custom_vehicle/session.hpp>

#include <mutex>

namespace custom_vehicle::protocol
{
class RpcTransport
{
public:
	virtual ~RpcTransport() = default;
	virtual bool sendRpc(RpcId id, const std::uint8_t* payload, std::size_t size) noexcept = 0;
};

class AssetDownloadSink
{
public:
	virtual ~AssetDownloadSink() = default;

	// Must copy the borrowed URL before returning and perform I/O off the RPC callback thread.
	virtual bool beginDownload(const AssetDownload& download) noexcept = 0;
};

enum class RpcDisposition
{
	ForwardToGame,
	Consume,
};

class ClientNetworkAdapter
{
public:
	ClientNetworkAdapter(RpcTransport& transport, AssetDownloadSink& downloader) noexcept
		: transport_(transport)
		, downloader_(downloader)
	{
	}

	// Sends the empty RPC 186 capability opt-in and resets state for this connection.
	bool beginConnection() noexcept;

	// Routes server RPCs. Vehicle manifests and matching download RPCs are consumed;
	// native SA-MP/DL artwork records remain available to the original client.
	RpcDisposition handleServerRpc(std::uint8_t rpcId,
		const std::uint8_t* payload, std::size_t size) noexcept;

	// Called by the platform downloader after a file is fully received.
	// A valid asset triggers the empty RPC 184 finish notification.
	bool completeAsset(AssetType type, std::uint32_t checksum,
		const std::uint8_t* data, std::size_t size) noexcept;

	// Called after transport failure, timeout, or an interrupted local file write.
	bool retryActiveAsset() noexcept;

	std::vector<ModelRequest> vehicleModels() const;

private:
	bool requestNextAssetUnlocked() noexcept;

	RpcTransport& transport_;
	AssetDownloadSink& downloader_;
	mutable std::mutex mutex_;
	ClientSession session_;
};
} // namespace custom_vehicle::protocol
