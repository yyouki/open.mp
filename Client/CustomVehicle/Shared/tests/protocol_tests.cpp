#include <custom_vehicle/protocol.hpp>
#include <custom_vehicle/session.hpp>
#include <custom_vehicle/network_adapter.hpp>

#ifdef NDEBUG
#undef NDEBUG // Keep assertions active even in Release test builds.
#endif

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace custom_vehicle::protocol;

namespace
{
void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8));
	bytes.push_back(static_cast<std::uint8_t>(value >> 16));
	bytes.push_back(static_cast<std::uint8_t>(value >> 24));
}

void appendI32(std::vector<std::uint8_t>& bytes, std::int32_t value)
{
	appendU32(bytes, static_cast<std::uint32_t>(value));
}

std::vector<std::uint8_t> makeVehicleRequest(
	std::uint32_t dffChecksum = 0x11223344u,
	std::uint32_t txdChecksum = 0x55667788u,
	std::uint32_t dffSize = 512,
	std::uint32_t txdSize = 1024,
	std::uint32_t poolId = 0,
	std::uint32_t modelCount = 1,
	std::int32_t modelId = 30001)
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve(ModelRequestWireSize);
	appendU32(bytes, poolId);        // Position in the shared model pool.
	appendU32(bytes, modelCount);    // Total entries in this manifest.
	bytes.push_back(3);              // Experimental vehicle model type.
	appendI32(bytes, -1);            // Global virtual world.
	appendI32(bytes, 400);           // Base vehicle model.
	appendI32(bytes, modelId);       // Custom vehicle model ID.
	appendU32(bytes, dffChecksum);   // DFF checksum.
	appendU32(bytes, txdChecksum);   // TXD checksum.
	appendU32(bytes, dffSize);       // DFF size.
	appendU32(bytes, txdSize);       // TXD size.
	bytes.push_back(0);              // Time-on hour.
	bytes.push_back(0);              // Time-off hour.
	return bytes;
}

std::vector<std::uint8_t> makeDownloadUrl(AssetType type, std::uint32_t checksum, std::string_view url)
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve(DownloadUrlHeaderSize + url.size());
	bytes.push_back(ModelUrlTransportMarker);
	bytes.push_back(static_cast<std::uint8_t>(type));
	appendU32(bytes, checksum);
	bytes.push_back(static_cast<std::uint8_t>(url.size()));
	bytes.insert(bytes.end(), url.begin(), url.end());
	return bytes;
}

void testManifestDecode()
{
	const auto bytes = makeVehicleRequest();
	ModelRequest request;
	DecodeError error = DecodeError::WrongSize;

	assert(decodeModelRequest(bytes.data(), bytes.size(), request, error));
	assert(error == DecodeError::None);
	assert(request.poolId == 0);
	assert(request.modelCount == 1);
	assert(request.type == ModelType::Vehicle);
	assert(request.virtualWorld == -1);
	assert(request.baseId == 400);
	assert(request.newId == 30001);
	assert(request.dffChecksum == 0x11223344u);
	assert(request.txdChecksum == 0x55667788u);
	assert(request.dffSize == 512);
	assert(request.txdSize == 1024);
	assert(validateVehicleModelRequest(request));
}

void testMalformedManifest()
{
	auto bytes = makeVehicleRequest();
	ModelRequest request;
	DecodeError error = DecodeError::None;

	assert(!decodeModelRequest(nullptr, bytes.size(), request, error));
	assert(error == DecodeError::NullBuffer);
	assert(!decodeModelRequest(bytes.data(), bytes.size() - 1, request, error));
	assert(error == DecodeError::WrongSize);

	bytes[8] = 99;
	assert(!decodeModelRequest(bytes.data(), bytes.size(), request, error));
	assert(error == DecodeError::InvalidModelType);
}

void testModelCountAndPoolIndexValidation()
{
	auto bytes = makeVehicleRequest();
	ModelRequest request;
	request.newId = 31234; // Failed decodes must not publish partial output.
	DecodeError error = DecodeError::None;

	// A signed -1 count must not become a huge unsigned pool capacity.
	bytes[4] = 0xFF;
	bytes[5] = 0xFF;
	bytes[6] = 0xFF;
	bytes[7] = 0xFF;
	assert(!decodeModelRequest(bytes.data(), bytes.size(), request, error));
	assert(error == DecodeError::InvalidModelCount);
	assert(request.newId == 31234);

	// A zero-entry manifest is invalid too.
	bytes[4] = 0;
	bytes[5] = 0;
	bytes[6] = 0;
	bytes[7] = 0;
	assert(!decodeModelRequest(bytes.data(), bytes.size(), request, error));
	assert(error == DecodeError::InvalidModelCount);
	assert(request.newId == 31234);

	// An entry index equal to the one-entry count is outside the pool.
	bytes[4] = 1;
	bytes[5] = 0;
	bytes[6] = 0;
	bytes[7] = 0;
	bytes[0] = 1;
	assert(!decodeModelRequest(bytes.data(), bytes.size(), request, error));
	assert(error == DecodeError::InvalidPoolIndex);
	assert(request.newId == 31234);
}

void testDownloadUrlCodec()
{
	const auto bytes = makeDownloadUrl(AssetType::Dff, 0xCBF43926u, "https://example.test/asset.dff");
	DownloadUrl url;
	DecodeError error = DecodeError::None;

	assert(decodeDownloadUrl(bytes.data(), bytes.size(), url, error));
	assert(error == DecodeError::None);
	assert(url.type == AssetType::Dff);
	assert(url.checksum == 0xCBF43926u);
	assert(url.url == "https://example.test/asset.dff");

	auto invalid = bytes;
	invalid[0] = 5;
	assert(!decodeDownloadUrl(invalid.data(), invalid.size(), url, error));
	assert(error == DecodeError::InvalidUrlMarker);

	invalid = bytes;
	invalid[1] = 3;
	assert(!decodeDownloadUrl(invalid.data(), invalid.size(), url, error));
	assert(error == DecodeError::InvalidAssetType);

	invalid = bytes;
	invalid[6]--;
	assert(!decodeDownloadUrl(invalid.data(), invalid.size(), url, error));
	assert(error == DecodeError::InvalidUrlLength);

	invalid = makeDownloadUrl(AssetType::Dff, 1, "file:///asset.dff");
	assert(!decodeDownloadUrl(invalid.data(), invalid.size(), url, error));
	assert(error == DecodeError::InvalidUrl);
}

void testAssetRequestEncoding()
{
	std::array<std::uint8_t, AssetRequestWireSize> payload {};
	std::size_t written = 0;
	RpcId rpcId = RpcId::ModelUrl;

	assert(encodeAssetRequest(AssetType::Dff, 0x12345678u, payload.data(), payload.size(), written, rpcId));
	assert(written == AssetRequestWireSize);
	assert(rpcId == RpcId::RequestDff);
	assert((payload == std::array<std::uint8_t, 4> { 0x78, 0x56, 0x34, 0x12 }));

	assert(encodeAssetRequest(AssetType::Txd, 0xAABBCCDDu, payload.data(), payload.size(), written, rpcId));
	assert(rpcId == RpcId::RequestTxd);
	assert((payload == std::array<std::uint8_t, 4> { 0xDD, 0xCC, 0xBB, 0xAA }));

	assert(!encodeAssetRequest(AssetType::Dff, 1, payload.data(), 3, written, rpcId));
	assert(written == 0);
}

void testVehicleValidation()
{
	auto bytes = makeVehicleRequest();
	ModelRequest request;
	DecodeError error = DecodeError::None;
	assert(decodeModelRequest(bytes.data(), bytes.size(), request, error));

	assert(isCustomVehicleModelId(30001));
	assert(isCustomVehicleModelId(32767));
	assert(!isCustomVehicleModelId(30000));
	assert(!isCustomVehicleModelId(32768));
	assert(isSupportedVehicleBaseId(400));
	assert(isSupportedVehicleBaseId(611));
	assert(!isSupportedVehicleBaseId(449));
	assert(!isSupportedVehicleBaseId(537));
	assert(!isSupportedVehicleBaseId(538));
	assert(!isSupportedVehicleBaseId(569));
	assert(!isSupportedVehicleBaseId(570));

	request.baseId = 537;
	assert(!validateVehicleModelRequest(request));
	request.baseId = 400;
	request.newId = 30000;
	assert(!validateVehicleModelRequest(request));
	request.newId = 30001;
	request.dffChecksum = request.txdChecksum;
	assert(!validateVehicleModelRequest(request));
}

void testSessionDownloadLifecycle()
{
	constexpr std::array<std::uint8_t, 9> dff = { '1','2','3','4','5','6','7','8','9' };
	constexpr std::array<std::uint8_t, 4> txd = { 't','x','d','!' };
	constexpr std::uint32_t dffCrc = 0xCBF43926u;
	const std::uint32_t txdCrc = calculateCrc32(txd.data(), txd.size());

	ClientSession session;
	assert(session.beginNegotiation());
	assert(session.capabilityState() == CapabilityState::AwaitingAcknowledgement);

	SessionError sessionError = SessionError::None;
	assert(session.acceptCapabilityAcknowledgement(nullptr, 0, sessionError));
	assert(session.capabilityState() == CapabilityState::Ready);

	auto manifest = makeVehicleRequest(dffCrc, txdCrc, static_cast<std::uint32_t>(dff.size()), static_cast<std::uint32_t>(txd.size()));
	DecodeError decodeError = DecodeError::None;
	assert(session.acceptModelManifest(manifest.data(), manifest.size(), decodeError, sessionError)
		== ManifestDisposition::AcceptedVehicle);
	assert(session.vehicleModels().size() == 1);

	AssetRequest request;
	assert(session.nextAssetRequest(request));
	assert(request.type == AssetType::Dff);
	assert(request.checksum == dffCrc);
	assert(request.expectedSize == dff.size());

	auto urlPayload = makeDownloadUrl(AssetType::Dff, dffCrc, "https://example.test/model.dff");
	AssetDownload download;
	assert(session.acceptDownloadUrl(urlPayload.data(), urlPayload.size(), download, decodeError, sessionError));
	assert(download.expectedSize == dff.size());
	assert(download.url == "https://example.test/model.dff");
	assert(session.completeAsset(download.type, download.checksum, dff.data(), dff.size(), sessionError));

	// Serialize downloads: the next file waits for RPC 185 after RPC 184.
	assert(!session.nextAssetRequest(request));
	assert(session.acceptDownloadAcknowledgement(nullptr, 0, sessionError));
	assert(session.nextAssetRequest(request));
	assert(request.type == AssetType::Txd);
	assert(request.checksum == txdCrc);

	urlPayload = makeDownloadUrl(AssetType::Txd, txdCrc, "https://example.test/model.txd");
	assert(session.acceptDownloadUrl(urlPayload.data(), urlPayload.size(), download, decodeError, sessionError));
	assert(session.completeAsset(download.type, download.checksum, txd.data(), txd.size(), sessionError));
	assert(session.acceptDownloadAcknowledgement(nullptr, 0, sessionError));
	assert(!session.nextAssetRequest(request));
}

void testSessionRejectsMismatchedUrlAndCorruptFile()
{
	constexpr std::array<std::uint8_t, 4> dff = { 'd','f','f','!' };
	constexpr std::array<std::uint8_t, 4> txd = { 't','x','d','!' };
	const std::uint32_t dffCrc = calculateCrc32(dff.data(), dff.size());
	const std::uint32_t txdCrc = calculateCrc32(txd.data(), txd.size());

	ClientSession session;
	session.beginNegotiation();
	SessionError sessionError = SessionError::None;
	assert(session.acceptCapabilityAcknowledgement(nullptr, 0, sessionError));

	auto manifest = makeVehicleRequest(dffCrc, txdCrc, static_cast<std::uint32_t>(dff.size()), static_cast<std::uint32_t>(txd.size()));
	DecodeError decodeError = DecodeError::None;
	assert(session.acceptModelManifest(manifest.data(), manifest.size(), decodeError, sessionError)
		== ManifestDisposition::AcceptedVehicle);

	AssetRequest request;
	assert(session.nextAssetRequest(request));
	auto wrongUrl = makeDownloadUrl(AssetType::Txd, request.checksum, "https://example.test/wrong.txd");
	AssetDownload download;
	assert(!session.acceptDownloadUrl(wrongUrl.data(), wrongUrl.size(), download, decodeError, sessionError));
	assert(sessionError == SessionError::DownloadUrlMismatch);

	auto validUrl = makeDownloadUrl(AssetType::Dff, dffCrc, "https://example.test/model.dff");
	assert(session.acceptDownloadUrl(validUrl.data(), validUrl.size(), download, decodeError, sessionError));
	assert(!session.completeAsset(download.type, download.checksum, dff.data(), dff.size() - 1, sessionError));
	assert(sessionError == SessionError::AssetIntegrityFailure);

	assert(session.nextAssetRequest(request));
	assert(request.type == AssetType::Dff); // Failed bytes are retried, not acknowledged.
	assert(!session.acceptDownloadAcknowledgement(nullptr, 0, sessionError));
}


void testManifestGrowthAndPoolUniqueness()
{
	ClientSession session;
	session.beginNegotiation();
	SessionError sessionError = SessionError::None;
	assert(session.acceptCapabilityAcknowledgement(nullptr, 0, sessionError));
	DecodeError decodeError = DecodeError::None;

	auto first = makeVehicleRequest(11, 12, 20, 21, 1, 3, 30001);
	assert(session.acceptModelManifest(first.data(), first.size(), decodeError, sessionError)
		== ManifestDisposition::AcceptedVehicle);

	// Late model registrations append to the manifest and may increase Count.
	auto second = makeVehicleRequest(13, 14, 22, 23, 2, 3, 30002);
	assert(session.acceptModelManifest(second.data(), second.size(), decodeError, sessionError)
		== ManifestDisposition::AcceptedVehicle);

	// A duplicate pool index or a decreasing Count is not a valid append.
	auto duplicatePool = makeVehicleRequest(15, 16, 24, 25, 1, 4, 30003);
	assert(session.acceptModelManifest(duplicatePool.data(), duplicatePool.size(), decodeError, sessionError)
		== ManifestDisposition::Rejected);
	assert(sessionError == SessionError::InconsistentManifest);

	auto smallerCount = makeVehicleRequest(17, 18, 26, 27, 0, 2, 30004);
	assert(session.acceptModelManifest(smallerCount.data(), smallerCount.size(), decodeError, sessionError)
		== ManifestDisposition::Rejected);
	assert(sessionError == SessionError::InconsistentManifest);
	assert(session.vehicleModels().size() == 2);
}

class FakeTransport final : public RpcTransport
{
public:
	struct SentRpc
	{
		RpcId id;
		std::vector<std::uint8_t> payload;
	};

	bool sendRpc(RpcId id, const std::uint8_t* payload, std::size_t size) noexcept override
	{
		SentRpc sent { id, {} };
		if (payload != nullptr && size != 0)
		{
			sent.payload.assign(payload, payload + size);
		}
		sentCalls.push_back(std::move(sent));
		return true;
	}

	std::vector<SentRpc> sentCalls;
};

class FakeDownloader final : public AssetDownloadSink
{
public:
	struct Job
	{
		AssetType type;
		std::uint32_t checksum;
		std::uint32_t expectedSize;
		std::string url;
	};

	bool beginDownload(const AssetDownload& download) noexcept override
	{
		jobs.push_back({ download.type, download.checksum, download.expectedSize,
			std::string(download.url) });
		return true;
	}

	std::vector<Job> jobs;
};

void testNetworkAdapterFlow()
{
	constexpr std::array<std::uint8_t, 9> dff = { '1','2','3','4','5','6','7','8','9' };
	constexpr std::array<std::uint8_t, 4> txd = { 't','x','d','!' };
	constexpr std::uint32_t dffCrc = 0xCBF43926u;
	const std::uint32_t txdCrc = calculateCrc32(txd.data(), txd.size());

	FakeTransport transport;
	FakeDownloader downloader;
	ClientNetworkAdapter adapter(transport, downloader);

	assert(adapter.beginConnection());
	assert(transport.sentCalls.size() == 1);
	assert(transport.sentCalls.back().id == RpcId::ClientSupportsCustomVehicleModels);
	assert(transport.sentCalls.back().payload.empty());

	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ServerSupportsCustomVehicleModels),
		nullptr, 0) == RpcDisposition::Consume);

	auto manifest = makeVehicleRequest(dffCrc, txdCrc,
		static_cast<std::uint32_t>(dff.size()), static_cast<std::uint32_t>(txd.size()));
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelRequest),
		manifest.data(), manifest.size()) == RpcDisposition::Consume);
	assert(transport.sentCalls.back().id == RpcId::RequestDff);
	assert(transport.sentCalls.back().payload.size() == AssetRequestWireSize);

	auto nativeManifest = manifest;
	nativeManifest[8] = static_cast<std::uint8_t>(ModelType::Skin);
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelRequest),
		nativeManifest.data(), nativeManifest.size()) == RpcDisposition::ForwardToGame);

	auto malformedDffUrl = makeDownloadUrl(AssetType::Dff, dffCrc, "https://example.test/car.dff");
	malformedDffUrl[0] = 5; // Invalid transport marker while a custom DFF URL is expected.
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelUrl),
		malformedDffUrl.data(), malformedDffUrl.size()) == RpcDisposition::Consume);
	assert(downloader.jobs.empty());

	auto dffUrl = makeDownloadUrl(AssetType::Dff, dffCrc, "https://example.test/car.dff");
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelUrl),
		dffUrl.data(), dffUrl.size()) == RpcDisposition::Consume);
	assert(downloader.jobs.size() == 1);
	assert(downloader.jobs.back().url == "https://example.test/car.dff");

	assert(adapter.completeAsset(AssetType::Dff, dffCrc, dff.data(), dff.size()));
	assert(transport.sentCalls.back().id == RpcId::FinishDownload);
	assert(transport.sentCalls.back().payload.empty());

	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::DownloadCompleted),
		nullptr, 0) == RpcDisposition::Consume);
	assert(transport.sentCalls.back().id == RpcId::RequestTxd);

	auto txdUrl = makeDownloadUrl(AssetType::Txd, txdCrc, "https://example.test/car.txd");
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelUrl),
		txdUrl.data(), txdUrl.size()) == RpcDisposition::Consume);
	assert(downloader.jobs.size() == 2);
	assert(adapter.completeAsset(AssetType::Txd, txdCrc, txd.data(), txd.size()));
	assert(transport.sentCalls.back().id == RpcId::FinishDownload);

	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::DownloadCompleted),
		nullptr, 0) == RpcDisposition::Consume);
	assert(adapter.vehicleModels().size() == 1);

	adapter.endConnection();
	assert(adapter.vehicleModels().empty());
	assert(adapter.handleServerRpc(static_cast<std::uint8_t>(RpcId::ModelUrl),
		dffUrl.data(), dffUrl.size()) == RpcDisposition::ForwardToGame);
}

void testAssetIntegrity()
{
	constexpr std::array<std::uint8_t, 9> sample = { '1','2','3','4','5','6','7','8','9' };
	constexpr std::uint32_t expectedCrc = 0xCBF43926u;
	assert(calculateCrc32(sample.data(), sample.size()) == expectedCrc);
	assert(verifyAsset(sample.data(), sample.size(), 9, expectedCrc));
	assert(!verifyAsset(sample.data(), sample.size(), 8, expectedCrc));
	assert(!verifyAsset(sample.data(), sample.size(), 9, 0));
}
} // namespace

int main()
{
	testManifestDecode();
	testMalformedManifest();
	testModelCountAndPoolIndexValidation();
	testDownloadUrlCodec();
	testAssetRequestEncoding();
	testVehicleValidation();
	testSessionDownloadLifecycle();
	testSessionRejectsMismatchedUrlAndCorruptFile();
	testManifestGrowthAndPoolUniqueness();
	testNetworkAdapterFlow();
	testAssetIntegrity();
	return 0;
}
