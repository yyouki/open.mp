#include <custom_vehicle/protocol.hpp>

#ifdef NDEBUG
#undef NDEBUG // Keep assertions active even in Release test builds.
#endif

#include <array>
#include <cassert>
#include <cstdint>
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

std::vector<std::uint8_t> makeVehicleRequest()
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve(ModelRequestWireSize);
	appendU32(bytes, 0);             // First entry in the model pool.
	appendU32(bytes, 1);             // Total entries in this manifest.
	bytes.push_back(3);              // Experimental vehicle model type.
	appendI32(bytes, -1);            // Global virtual world.
	appendI32(bytes, 400);           // Base vehicle model.
	appendI32(bytes, 30001);         // Custom vehicle model ID.
	appendU32(bytes, 0x11223344u);   // DFF checksum.
	appendU32(bytes, 0x55667788u);   // TXD checksum.
	appendU32(bytes, 512);           // DFF size.
	appendU32(bytes, 1024);          // TXD size.
	bytes.push_back(0);              // Time-on hour.
	bytes.push_back(0);              // Time-off hour.
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

void testAssetIntegrity()
{
	constexpr std::array<std::uint8_t, 9> sample = {
		'1', '2', '3', '4', '5', '6', '7', '8', '9'
	};
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
	testVehicleValidation();
	testAssetIntegrity();
	return 0;
}
