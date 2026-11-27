#pragma once

#include <custom_vehicle/protocol.hpp>

#include <filesystem>
#include <mutex>

namespace custom_vehicle::protocol
{
class AssetCache
{
public:
	explicit AssetCache(std::filesystem::path root);

	// Paths are derived only from validated type and checksum, never from remote URL text.
	std::filesystem::path pathFor(AssetType type, std::uint32_t checksum) const;

	// Store bytes only after validating the exact registered size and CRC32.
	// A temporary file is verified before it is atomically renamed into the cache.
	bool store(
		AssetType type,
		std::uint32_t checksum,
		std::uint32_t expectedSize,
		const std::uint8_t* data,
		std::size_t size);

	// Revalidates the on-disk file before a platform model loader uses it.
	bool contains(AssetType type, std::uint32_t checksum, std::uint32_t expectedSize) const;

private:
	bool isSafeCacheFile(
		const std::filesystem::path& file,
		const std::filesystem::path& canonicalRoot) const;
	bool isValidCacheFile(
		const std::filesystem::path& file,
		const std::filesystem::path& canonicalRoot,
		std::uint32_t expectedSize,
		std::uint32_t expectedChecksum) const;

	std::filesystem::path root_;
	mutable std::mutex mutex_;
};
} // namespace custom_vehicle::protocol
