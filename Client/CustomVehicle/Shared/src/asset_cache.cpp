#include <custom_vehicle/asset_cache.hpp>

#include <array>
#include <fstream>
#include <limits>
#include <system_error>

namespace custom_vehicle::protocol
{
namespace
{
bool calculateFileCrc32(
	const std::filesystem::path& filePath,
	std::uint32_t expectedSize,
	std::uint32_t& checksum)
{
	std::error_code error;
	const std::uintmax_t actualSize = std::filesystem::file_size(filePath, error);
	if (error || actualSize != expectedSize)
	{
		return false;
	}

	std::ifstream stream(filePath, std::ios::binary);
	if (!stream.is_open())
	{
		return false;
	}

	std::array<char, 8192> buffer {};
	std::uint32_t crc = 0xFFFFFFFFu;
	std::uint64_t bytesRead = 0;

	for (;;)
	{
		stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
		const std::streamsize count = stream.gcount();
		if (count < 0)
		{
			return false;
		}

		for (std::streamsize index = 0; index < count; ++index)
		{
			crc ^= static_cast<std::uint8_t>(buffer[static_cast<std::size_t>(index)]);
			for (unsigned bit = 0; bit < 8; ++bit)
			{
				const std::uint32_t mask = 0u - (crc & 1u);
				crc = (crc >> 1) ^ (0xEDB88320u & mask);
			}
		}
		bytesRead += static_cast<std::uint64_t>(count);

		if (stream.eof())
		{
			break;
		}
		if (stream.bad() || stream.fail())
		{
			return false;
		}
	}

	if (bytesRead != expectedSize)
	{
		return false;
	}

	checksum = ~crc;
	return true;
}
} // namespace

AssetCache::AssetCache(std::filesystem::path root)
	: root_(std::move(root))
{
}

std::filesystem::path AssetCache::pathFor(AssetType type, std::uint32_t checksum) const
{
	const char* prefix = nullptr;
	const char* extension = nullptr;
	switch (type)
	{
		case AssetType::Dff:
			prefix = "dff-";
			extension = ".dff";
			break;
		case AssetType::Txd:
			prefix = "txd-";
			extension = ".txd";
			break;
		default:
			return {};
	}

	// The checksum is rendered as a fixed numeric token; remote URL paths are never used.
	char digits[9] {};
	static constexpr char hex[] = "0123456789abcdef";
	for (unsigned index = 0; index < 8; ++index)
	{
		const unsigned shift = (7u - index) * 4u;
		digits[index] = hex[(checksum >> shift) & 0x0Fu];
	}
	return root_ / (std::string(prefix) + digits + extension);
}

bool AssetCache::isSafeCacheFile(
	const std::filesystem::path& file,
	const std::filesystem::path& canonicalRoot) const
{
	std::error_code error;
	const std::filesystem::path canonicalFile = std::filesystem::canonical(file, error);
	if (error || !std::filesystem::is_regular_file(canonicalFile, error) || error)
	{
		return false;
	}

	const std::filesystem::path relativeFile = canonicalFile.lexically_relative(canonicalRoot);
	if (relativeFile.empty() || relativeFile.is_absolute())
	{
		return false;
	}
	for (const auto& component : relativeFile)
	{
		if (component == "..")
		{
			return false;
		}
	}
	return true;
}

bool AssetCache::isValidCacheFile(
	const std::filesystem::path& file,
	const std::filesystem::path& canonicalRoot,
	std::uint32_t expectedSize,
	std::uint32_t expectedChecksum) const
{
	if (!isSafeCacheFile(file, canonicalRoot))
	{
		return false;
	}

	std::uint32_t actualChecksum = 0;
	return calculateFileCrc32(file, expectedSize, actualChecksum)
		&& actualChecksum == expectedChecksum;
}

bool AssetCache::store(
	AssetType type,
	std::uint32_t checksum,
	std::uint32_t expectedSize,
	const std::uint8_t* data,
	std::size_t size)
{
	if (expectedSize == 0 || !verifyAsset(data, size, expectedSize, checksum)
		|| size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
	{
		return false;
	}

	std::lock_guard lock(mutex_);
	std::error_code error;
	std::filesystem::create_directories(root_, error);
	if (error)
	{
		return false;
	}

	const std::filesystem::path canonicalRoot = std::filesystem::canonical(root_, error);
	if (error)
	{
		return false;
	}

	const std::filesystem::path destination = pathFor(type, checksum);
	if (destination.empty())
	{
		return false;
	}

	if (isValidCacheFile(destination, canonicalRoot, expectedSize, checksum))
	{
		return true;
	}

	// Remove stale/corrupt files and symlinks before writing a new verified cache entry.
	std::filesystem::remove(destination, error);
	error.clear();

	std::filesystem::path temporary = destination;
	temporary += ".part";
	std::filesystem::remove(temporary, error);
	error.clear();

	std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
	if (!stream.is_open())
	{
		return false;
	}

	stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
	stream.flush();
	const bool writeSucceeded = stream.good();
	stream.close();
	if (!writeSucceeded || stream.fail()
		|| !isValidCacheFile(temporary, canonicalRoot, expectedSize, checksum))
	{
		std::filesystem::remove(temporary, error);
		return false;
	}

	std::filesystem::rename(temporary, destination, error);
	if (error)
	{
		std::filesystem::remove(temporary, error);
		return false;
	}

	if (!isValidCacheFile(destination, canonicalRoot, expectedSize, checksum))
	{
		std::filesystem::remove(destination, error);
		return false;
	}
	return true;
}

bool AssetCache::contains(
	AssetType type,
	std::uint32_t checksum,
	std::uint32_t expectedSize) const
{
	if (expectedSize == 0)
	{
		return false;
	}

	std::lock_guard lock(mutex_);
	std::error_code error;
	const std::filesystem::path canonicalRoot = std::filesystem::canonical(root_, error);
	if (error)
	{
		return false;
	}

	const std::filesystem::path file = pathFor(type, checksum);
	return !file.empty() && isValidCacheFile(file, canonicalRoot, expectedSize, checksum);
}
} // namespace custom_vehicle::protocol
