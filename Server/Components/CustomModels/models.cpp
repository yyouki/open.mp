/*
 *  This Source Code Form is subject to the terms of the Mozilla Public License,
 *  v. 2.0. If a copy of the MPL was not distributed with this file, You can
 *  obtain one at http://mozilla.org/MPL/2.0/.
 *
 *  The original code is copyright (c) 2022, open.mp team and contributors.
 */

#include <Impl/pool_impl.hpp>
#include <Server/Components/CustomModels/custommodels.hpp>
#include "model_types.hpp"
#include <sdk.hpp>
#include <netcode.hpp>
#include <httplib.h>
#include <ghc/filesystem.hpp>
#include "crc32.hpp"
#include <regex>
#include <array>
#include <fstream>
#include <shared_mutex>
#include <algorithm>
#include <cctype>
#include <limits>
#include "utils.hpp"

static auto rAddCharModel = std::regex(R"(AddCharModel\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*\"(.+)\"\s*,\s*\"(.+)\"\s*\)\s*;*)");
static auto rAddSimpleModel = std::regex(R"(AddSimpleModel\s*\(\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(-\d+)\s*,\s*\"(.+)\"\s*,\s*\"(.+)\"\s*\)\s*;*)");
static auto rAddSimpleModelTimed = std::regex(R"(AddSimpleModelTimed\s*\(\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(-\d+)\s*,\s*\"(.+)\"\s*,\s*\"(.+)\"\s*,\s*(\d+)\s*,\s*(\d+)\s*\)\s*;*)");
static auto rAddVehicleModel = std::regex(R"(AddVehicleModel\s*\(\s*(\d+)\s*,\s*(\d+)\s*,\s*\"(.+)\"\s*,\s*\"(.+)\"\s*\)\s*;*)");
static auto rUri = std::regex("[A-Za-z0-9-._~:/?#\\[\\]@!$&'()*+,;=]+"); // very loose interpretation of rfc3986

using namespace Impl;

// Keep registered asset paths inside the artwork directory and reject names the HTTP server cannot serve.
static bool isSafeModelFilePath(StringView modelsPath, StringView fileName)
{
	if (fileName.empty())
	{
		return false;
	}

	const String fileNameString(fileName.data(), fileName.size());
	if (fileNameString.find_first_of("\\:?#") != String::npos)
	{
		return false;
	}

	if (fileNameString.front() == '/' || fileNameString.back() == '/')
	{
		return false;
	}

	// Reject empty URL path segments, which the HTTP server refuses to serve.
	for (size_t segmentStart = 0; segmentStart < fileNameString.size();)
	{
		const size_t separator = fileNameString.find('/', segmentStart);
		const size_t segmentEnd = separator == String::npos ? fileNameString.size() : separator;
		if (segmentEnd == segmentStart)
		{
			return false;
		}

		const StringView segment(fileNameString.data() + segmentStart, segmentEnd - segmentStart);
		if (segment == "." || segment == ".." || segment.back() == '.' || segment.back() == ' ')
		{
			return false;
		}

		if (separator == String::npos)
		{
			break;
		}
		segmentStart = separator + 1;
	}

	for (unsigned char character : fileNameString)
	{
		if (character < 0x20 || character == 0x7F)
		{
			return false;
		}
	}

	const ghc::filesystem::path relativeFilePath(fileNameString);
	if (relativeFilePath.empty() || relativeFilePath.is_absolute()
		|| relativeFilePath.has_root_name() || relativeFilePath.has_root_directory())
	{
		return false;
	}

	for (const auto& component : relativeFilePath)
	{
		if (component.empty() || component == "." || component == "..")
		{
			return false;
		}
	}

	std::error_code error;
	const ghc::filesystem::path canonicalRoot = ghc::filesystem::canonical(
		ghc::filesystem::path(String(modelsPath.data(), modelsPath.size())), error);
	if (error)
	{
		return false;
	}

	const ghc::filesystem::path canonicalFile = ghc::filesystem::canonical(canonicalRoot / relativeFilePath, error);
	if (error || !ghc::filesystem::is_regular_file(canonicalFile, error) || error)
	{
		return false;
	}

	const ghc::filesystem::path relativeCanonicalFile = canonicalFile.lexically_relative(canonicalRoot);
	if (relativeCanonicalFile.empty() || relativeCanonicalFile.is_absolute())
	{
		return false;
	}

	for (const auto& component : relativeCanonicalFile)
	{
		if (component == "..")
		{
			return false;
		}
	}

	return true;
}

static bool hasExpectedFileExtension(StringView fileName, StringView extension)
{
	auto equalsIgnoreCase = [](unsigned char left, unsigned char right)
	{
		return std::tolower(left) == std::tolower(right);
	};

	return fileName.size() > extension.size()
		&& std::equal(extension.begin(), extension.end(), fileName.end() - extension.size(), equalsIgnoreCase);
}

struct ModelFile
{
	String name;
	uint32_t checksum;
	size_t size;

	ModelFile(StringView modelsPath, StringView fileName)
		: name(fileName)
		, size(GetFileCRC32Checksum(String(modelsPath) + "/" + String(fileName), checksum))
	{
	}
};

static bool haveSameFileContents(const String& leftPath, const String& rightPath)
{
	std::ifstream leftFile(leftPath, std::ios::binary);
	std::ifstream rightFile(rightPath, std::ios::binary);
	if (!leftFile.is_open() || !rightFile.is_open())
	{
		return false;
	}

	std::array<char, 4096> leftBuffer;
	std::array<char, 4096> rightBuffer;
	for (;;)
	{
		leftFile.read(leftBuffer.data(), static_cast<std::streamsize>(leftBuffer.size()));
		rightFile.read(rightBuffer.data(), static_cast<std::streamsize>(rightBuffer.size()));

		const std::streamsize leftBytes = leftFile.gcount();
		const std::streamsize rightBytes = rightFile.gcount();
		if (leftBytes != rightBytes
			|| !std::equal(leftBuffer.begin(), leftBuffer.begin() + leftBytes, rightBuffer.begin()))
		{
			return false;
		}

		if (leftFile.bad() || rightFile.bad())
		{
			return false;
		}

		if (leftFile.eof() || rightFile.eof())
		{
			return leftFile.eof() && rightFile.eof();
		}

		if (leftFile.fail() || rightFile.fail())
		{
			return false;
		}
	}
}

class ModelInfo final
{
private:
	ModelType type_;
	int32_t baseId_;
	int32_t newId_;
	ModelFile dff_;
	ModelFile txd_;
	int32_t worldId_;
	uint8_t timeOn_;
	uint8_t timeOff_;

public:
	ModelInfo(ModelType type, int32_t id, int32_t baseId, ModelFile dff, ModelFile txd, int32_t worldId, uint8_t timeOn, uint8_t timeOff)
		: type_(type)
		, baseId_(baseId)
		, newId_(id)
		, dff_(dff)
		, txd_(txd)
		, worldId_(worldId)
		, timeOn_(timeOn)
		, timeOff_(timeOff)
	{
	}

	const ModelFile& getTXD() { return txd_; }
	const ModelFile& getDFF() { return dff_; }
	ModelType getType() const { return type_; }

	void write(NetCode::RPC::ModelRequest& modelInfo) const
	{
		modelInfo.type = static_cast<uint8_t>(type_);
		modelInfo.virtualWorld = worldId_;
		modelInfo.baseId = baseId_;
		modelInfo.newId = newId_;
		modelInfo.dffChecksum = dff_.checksum;
		modelInfo.txdChecksum = txd_.checksum;
		modelInfo.dffSize = dff_.size;
		modelInfo.txdSize = txd_.size;
		modelInfo.timeOn = timeOn_;
		modelInfo.timeOff = timeOff_;
	}

	void write(NetCode::RPC::VehicleModelRequest& modelInfo) const
	{
		modelInfo.type = static_cast<uint8_t>(type_);
		modelInfo.virtualWorld = worldId_;
		modelInfo.baseId = baseId_;
		modelInfo.newId = newId_;
		modelInfo.dffChecksum = dff_.checksum;
		modelInfo.txdChecksum = txd_.checksum;
		modelInfo.dffSize = dff_.size;
		modelInfo.txdSize = txd_.size;
		modelInfo.timeOn = timeOn_;
		modelInfo.timeOff = timeOff_;
	}

	const int32_t getId() { return newId_; }
};

class RegisteredModelAssets
{
public:
	struct Asset
	{
		uint32_t checksum;
		size_t size;
	};

private:
	FlatHashMap<String, Asset> assets_;
	mutable std::shared_mutex mutex_;

public:
	bool registerAssets(const ModelFile& dff, const ModelFile& txd)
	{
		std::unique_lock lock(mutex_);

		auto isCompatible = [this](const ModelFile& file)
		{
			auto itr = assets_.find(file.name);
			return itr == assets_.end()
				|| (itr->second.checksum == file.checksum && itr->second.size == file.size);
		};

		// A registered URL must identify the same bytes in both local and CDN modes.
		if (!isCompatible(dff) || !isCompatible(txd))
		{
			return false;
		}

		assets_.emplace(dff.name, Asset { dff.checksum, dff.size });
		assets_.emplace(txd.name, Asset { txd.checksum, txd.size });
		return true;
	}

	bool find(StringView fileName, Asset& asset) const
	{
		const String key(fileName.data(), fileName.size());
		std::shared_lock lock(mutex_);
		auto itr = assets_.find(key);
		if (itr == assets_.end())
		{
			return false;
		}

		asset = itr->second;
		return true;
	}
};

class WebServer
{
private:
	httplib::Server svr;
	std::thread thread;

	String address_ = "";
	uint16_t port_ = 0;
	String modelsPath_;

	String url = "";

	FlatHashMap<uint32_t, uint16_t> allowedIPs_;
	std::shared_mutex mutex_;

	RegisteredModelAssets& registeredAssets_;

	bool isRegisteredAssetCurrent(StringView requestPath)
	{
		if (requestPath.size() < 2 || requestPath.front() != '/')
		{
			return true;
		}

		const String fileName(requestPath.data() + 1, requestPath.size() - 1);
		RegisteredModelAssets::Asset expectedAsset {};
		if (!registeredAssets_.find(fileName, expectedAsset))
		{
			return true;
		}

		// Re-resolve the path so a replaced symlink cannot redirect validation outside the models directory.
		if (!isSafeModelFilePath(modelsPath_, fileName))
		{
			return false;
		}

		uint32_t currentChecksum = 0;
		const size_t currentSize = GetFileCRC32Checksum(modelsPath_ + "/" + fileName, currentChecksum);
		return currentSize == expectedAsset.size && currentChecksum == expectedAsset.checksum;
}

public:
	WebServer(ICore* core, StringView modelsPath, StringView bind, uint16_t port, StringView publicAddr, uint16_t threadsCount, RegisteredModelAssets& registeredAssets)
		: port_(port)
		, modelsPath_(modelsPath)
		, registeredAssets_(registeredAssets)
	{

		if (!bind.empty())
		{
			address_ = String(bind.data());
			url = "http://" + (publicAddr.empty() ? address_ : String(publicAddr.data())) + ':' + std::to_string(port) + '/';

			if (!publicAddr.empty())
			{
				core->logLn(LogLevel::Message, "Using public address %.*s:%d (local: %.*s:%d)", PRINT_VIEW(publicAddr), port, PRINT_VIEW(address_), port);
			}
			else
			{
				core->logLn(LogLevel::Warning, "No public address provided. If your public address differs from bind one please provide it in server config.");
			}
		}
		else
		{
			address_ = "127.0.0.1";
			url = "http://127.0.0.1:" + std::to_string(port) + '/';

			core->logLn(LogLevel::Warning, "No bind address provided. Attempting to start webserver on 127.0.0.1:%d", port);
		}

		svr.new_task_queue = [threadsCount]
		{
			return new httplib::ThreadPool(threadsCount);
		};

		svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res)
			{
				auto isModelFile = [](StringView path)
				{
					auto hasExtension = [path](StringView ext)
					{
						auto equalsIgnoreCase = [](unsigned char a, unsigned char b)
						{
							return a == std::tolower(b);
						};

						return path.size() > ext.size() && std::equal(ext.begin(), ext.end(), path.end() - ext.size(), equalsIgnoreCase);
					};

					return hasExtension(".dff") || hasExtension(".txd");
				};

				if (req.method != "GET" || !req.has_header("User-Agent") || req.get_header_value("User-Agent") != "SAMP/0.3" || !isModelFile(req.path))
				{
					res.status = 401;
					return httplib::Server::HandlerResponse::Handled;
				}

				// TODO: Add IPv6 support.
				if (req.sockaddr.ss_family != AF_INET)
				{
					res.status = 401;
					return httplib::Server::HandlerResponse::Handled;
				}

				auto ip = reinterpret_cast<const struct sockaddr_in*>(&req.sockaddr)->sin_addr.s_addr;
				{
					std::shared_lock<std::shared_mutex> lock(mutex_);
					if (allowedIPs_.find(ip) == allowedIPs_.end())
					{
						res.status = 401;
						return httplib::Server::HandlerResponse::Handled;
					}
				}

				if (!isRegisteredAssetCurrent(req.path))
				{
					res.status = 409;
					res.set_content("Registered model asset changed after registration.", "text/plain");
					return httplib::Server::HandlerResponse::Handled;
				}

				return httplib::Server::HandlerResponse::Unhandled;
			});

		// Recheck after the static file is opened to catch a file replacement during request routing.
		svr.set_file_request_handler([this](const httplib::Request& req, httplib::Response& res)
			{
				if (!isRegisteredAssetCurrent(req.path))
				{
					res.status = 409;
					res.set_content("Registered model asset changed after registration.", "text/plain");
				}
			});

		if (!svr.set_mount_point("/", modelsPath.data()))
		{
			core->logLn(LogLevel::Error, "Failed to start webserver: Unable to mount models path (%.*s).", PRINT_VIEW(modelsPath));
			return;
		}

		if (address_.find(':') != String::npos)
		{
			core->logLn(LogLevel::Error, "Failed to start webserver: Only IPv4 bind addresses are supported (%.*s).", PRINT_VIEW(address_));
			return;
		}

		svr.set_address_family(AF_INET);
		if (!svr.bind_to_port(address_.c_str(), port_))
		{
			core->logLn(LogLevel::Error, "Failed to start webserver: Unable to bind to %.*s:%d.", PRINT_VIEW(address_), port_);
			return;
		}

		thread = std::thread(&WebServer::run, this);
		svr.wait_until_ready();
	}

	~WebServer()
	{
		stop();
		if (thread.joinable())
		{
			thread.join();
		}
	}

	void run()
	{
		svr.listen_after_bind();
	}

	bool is_running()
	{
		return svr.is_running();
	}

	void stop()
	{
		svr.stop();
	}

	StringView getUrl() const
	{
		return url;
	}

	void allowIPAddress(uint32_t ipAddress)
	{
		std::unique_lock lock(mutex_);

		auto itr = allowedIPs_.find(ipAddress);
		if (itr == allowedIPs_.end())
		{
			allowedIPs_.insert({ ipAddress, 1 });
		}
		else
		{
			++itr->second;
		}
	}

	void removeIPAddress(uint32_t ipAddress)
	{
		std::unique_lock lock(mutex_);

		auto itr = allowedIPs_.find(ipAddress);
		if (itr == allowedIPs_.end())
		{
			return;
		}

		if (itr->second > 1)
		{
			--itr->second;
		}
		else
		{
			allowedIPs_.erase(itr);
		}
	}
};

class PlayerCustomModelsData final : public IPlayerCustomModelsData
{
private:
	IPlayer& player;
	uint32_t skin_ = 0;
	std::pair<ModelDownloadType, uint32_t> requestedFile_;
	bool clientInitialized_ = false;

public:
	PlayerCustomModelsData(IPlayer& player)
		: player(player)
		, requestedFile_ { ModelDownloadType::NONE, 0 }
	{
	}

	bool hasClientInitialized() const
	{
		return clientInitialized_;
	}

	void markClientInitialized()
	{
		clientInitialized_ = true;
	}

	uint32_t getCustomSkin() const override
	{
		return skin_;
	}

	void setCustomSkin(const uint32_t skinModel) override
	{
		skin_ = skinModel;
	}

	void setRequestedFile(ModelDownloadType type, uint32_t checksum)
	{
		requestedFile_ = { type, checksum };
	}

	virtual bool sendDownloadUrl(StringView url) const override
	{
		if (requestedFile_.first == ModelDownloadType::NONE)
		{
			return false;
		}
		NetCode::RPC::ModelUrl downloadLink(url, static_cast<uint8_t>(requestedFile_.first), requestedFile_.second);
		PacketHelper::send(downloadLink, player);
		return true;
	}

	void reset() override
	{
		skin_ = 0;
		requestedFile_ = { ModelDownloadType::NONE, 0 };
		clientInitialized_ = false;
	}

	void freeExtension() override
	{
		delete this;
	}
};

class CustomModelsComponent final : public ICustomModelsComponent, public PlayerConnectEventHandler
{
private:
	ICore* core = nullptr;
	IPlayerPool* players = nullptr;

	WebServer* webServer = nullptr;
	RegisteredModelAssets registeredAssets;

	std::vector<ModelInfo*> storage;
	FlatHashMap<uint32_t, uint16_t> baseModels;
	FlatHashMap<uint32_t, std::pair<ModelDownloadType, ModelInfo*>> checksums;

	bool enabled = true;
	uint16_t modelsPort = 7777;
	String modelsPath = "models";
	String webServerBindAddress = "";
	String cdn = "";
	bool usingCdn = false;
	uint16_t httpThreads = 50; // default max_players is 50
	bool showCRCLogs = false;
	bool vehicleModelsEnabled = false;

	DefaultEventDispatcher<PlayerModelsEventHandler> eventDispatcher;

	class PlayerCustomVehicleModelsData final : public IPlayerCustomVehicleModelsData
	{
		bool supported_ = false;
		bool hasPendingDownload_ = false;
		ModelDownloadType pendingType_ = ModelDownloadType::NONE;
		uint32_t pendingChecksum_ = 0;

	public:
		bool supportsCustomVehicleModels() const override
		{
			return supported_;
		}

		void setSupported(bool supported)
		{
			supported_ = supported;
			if (!supported_)
			{
				clearPendingDownload();
			}
		}

		bool beginPendingDownload(ModelDownloadType type, uint32_t checksum)
		{
			// Retries may request a fresh URL for the same asset, but a different
			// asset must wait until the current transfer has been acknowledged.
			if (hasPendingDownload_ && (pendingType_ != type || pendingChecksum_ != checksum))
			{
				return false;
			}

			hasPendingDownload_ = true;
			pendingType_ = type;
			pendingChecksum_ = checksum;
			return true;
		}

		bool completePendingDownload(ModelDownloadType type, uint32_t checksum)
		{
			if (!hasPendingDownload_ || pendingType_ != type || pendingChecksum_ != checksum)
			{
				return false;
			}

			clearPendingDownload();
			return true;
		}

		void clearPendingDownload()
		{
			hasPendingDownload_ = false;
			pendingType_ = ModelDownloadType::NONE;
			pendingChecksum_ = 0;
		}

		void reset() override
		{
			supported_ = false;
			clearPendingDownload();
		}

		void freeExtension() override
		{
			delete this;
		}
	};

	bool hasDownloadEndpoint()
	{
		return usingCdn || (webServer && webServer->is_running());
	}

	bool supportsCustomVehicles(IPlayer& player)
	{
		const auto* extension = queryExtension<IPlayerCustomVehicleModelsData>(player);
		return enabled && vehicleModelsEnabled && hasDownloadEndpoint()
			&& extension && extension->supportsCustomVehicleModels();
	}

	void sendCustomModels(IPlayer& player)
	{
		if (!hasDownloadEndpoint()
			|| player.getClientVersion() != ClientVersion::ClientVersion_SAMP_03DL)
		{
			return;
		}

		// Native DL handles only its original skin/object artwork records.
		// Custom vehicles use the isolated RPC 220-227 extension below.
		const uint32_t modelCount = static_cast<uint32_t>(std::count_if(
			storage.begin(), storage.end(),
			[](const ModelInfo* model)
			{
				return model->getType() != CustomModels::Vehicle;
			}));

		uint32_t poolId = 0;
		for (const ModelInfo* model : storage)
		{
			if (model->getType() == CustomModels::Vehicle)
			{
				continue;
			}

			NetCode::RPC::ModelRequest request(static_cast<int32_t>(poolId++), modelCount);
			model->write(request);
			PacketHelper::send(request, player);
		}
	}

	uint32_t sendCustomVehicleModels(IPlayer& player)
	{
		if (!hasDownloadEndpoint() || !supportsCustomVehicles(player))
		{
			return 0;
		}

		const uint32_t vehicleModelCount = static_cast<uint32_t>(std::count_if(
			storage.begin(), storage.end(),
			[](const ModelInfo* model)
			{
				return model->getType() == CustomModels::Vehicle;
			}));

		uint32_t poolId = 0;
		for (const ModelInfo* model : storage)
		{
			if (model->getType() != CustomModels::Vehicle)
			{
				continue;
			}

			NetCode::RPC::VehicleModelRequest request(static_cast<int32_t>(poolId++), vehicleModelCount);
			model->write(request);
			PacketHelper::send(request, player);
		}
		return vehicleModelCount;
	}

	void sendNewCustomModel(const ModelInfo& model)
	{
		const bool isVehicleModel = model.getType() == CustomModels::Vehicle;
		for (IPlayer* player : players->entries())
		{
			const bool is03DL = player->getClientVersion() == ClientVersion::ClientVersion_SAMP_03DL;
			const bool includeVehicles = supportsCustomVehicles(*player);
			if ((isVehicleModel && !includeVehicles) || (!isVehicleModel && !is03DL))
			{
				continue;
			}

			PlayerCustomModelsData* data = queryExtension<PlayerCustomModelsData>(*player);
			if (!data || !data->hasClientInitialized())
			{
				continue;
			}

			// Vehicle records use their own manifest/pool and never enter the native DL artwork RPC.
			// Native skin/object records retain the original 0.3.DL pool and packet format.
			uint32_t poolId = 0;
			for (size_t i = 0; i + 1 < storage.size(); ++i)
			{
				const bool previousIsVehicle = storage[i]->getType() == CustomModels::Vehicle;
				if (isVehicleModel ? previousIsVehicle : (!previousIsVehicle && is03DL))
				{
					++poolId;
				}
			}

			if (isVehicleModel)
			{
				NetCode::RPC::VehicleModelRequest request(static_cast<int32_t>(poolId), poolId + 1);
				model.write(request);
				PacketHelper::send(request, *player);
			}
			else
			{
				NetCode::RPC::ModelRequest request(static_cast<int32_t>(poolId), poolId + 1);
				model.write(request);
				PacketHelper::send(request, *player);
				// Native 0.3.DL refreshes its artwork cache after a world change.
				forceCustomModelDownload(*player);
			}
		}
	}

	void forceCustomModelDownload(IPlayer& player)
	{
		NetCode::RPC::SetPlayerVirtualWorld setWorld;
		setWorld.worldId = player.getVirtualWorld() + 1;
		PacketHelper::send(setWorld, player);
		setWorld.worldId--;
		PacketHelper::send(setWorld, player);
	}

	struct ClientSupportsCustomVehicleModelsHandler : public SingleNetworkInEventHandler
	{
		CustomModelsComponent& self;

		ClientSupportsCustomVehicleModelsHandler(CustomModelsComponent& component)
			: self(component)
		{
		}

		bool onReceive(IPlayer& peer, NetworkBitStream& bs) override
		{
			if (!self.enabled || !self.vehicleModelsEnabled)
			{
				return true;
			}

			const bool isFirstCapability
				= queryExtension<IPlayerCustomVehicleModelsData>(peer) == nullptr;
			if (isFirstCapability)
			{
				peer.addExtension(new PlayerCustomVehicleModelsData(), true);
			}

			PlayerCustomVehicleModelsData* vehicleData
				= queryExtension<PlayerCustomVehicleModelsData>(peer);
			if (vehicleData)
			{
				vehicleData->setSupported(self.hasDownloadEndpoint());
			}

			// The legacy artwork HTTP server normally whitelists only 0.3DL clients.
			// Capability opt-in also authorizes this compatible 0.3.7 extension client.
			if (isFirstCapability && self.webServer && self.webServer->is_running()
				&& peer.getClientVersion() != ClientVersion::ClientVersion_SAMP_03DL)
			{
				// 0.3DL clients were already whitelisted in onPlayerConnect.
				self.webServer->allowIPAddress(peer.getNetworkData().networkID.address.v4);
			}

			NetCode::RPC::ServerSupportsCustomVehicleModels response;
			PacketHelper::send(response, peer);

			// Send the isolated vehicle manifest if the native client-init phase has already passed.
			PlayerCustomModelsData* playerData = queryExtension<PlayerCustomModelsData>(peer);
			if (isFirstCapability && playerData && playerData->hasClientInitialized())
			{
				self.sendCustomVehicleModels(peer);
			}
			return true;
		}
	} clientSupportsCustomVehicleModelsHandler;

	struct RequestDownloadLinkEventHandler : public SingleNetworkInEventHandler
	{
		CustomModelsComponent& self;

		RequestDownloadLinkEventHandler(CustomModelsComponent& modelsComponent)
			: self(modelsComponent)
		{
		}

		bool onReceive(IPlayer& peer, NetworkBitStream& bs) override
		{
			uint32_t checksum;

			if (!bs.readUINT32(checksum))
				return false;

			const auto itr = self.checksums.find(checksum);

			if (itr == self.checksums.end())
				return true;

			// Copy values before invoking extension callbacks, which may register models and rehash checksums.
			const ModelDownloadType type = itr->second.first;
			ModelInfo* const model = itr->second.second;

			// Native 0.3.DL request IDs remain reserved for the stock skin/object artwork pipeline.
			if (model->getType() == CustomModels::Vehicle)
			{
				return true;
			}

			PlayerCustomModelsData* data = queryExtension<PlayerCustomModelsData>(peer);
			if (data != nullptr)
			{
				data->setRequestedFile(type, checksum);
			}
			if (self.hasDownloadEndpoint()
				&& self.eventDispatcher.stopAtFalse(
					[&peer, type, checksum](PlayerModelsEventHandler* handler)
					{
						return handler->onPlayerRequestDownload(peer, type, checksum);
					}))
			{
				const auto& file = type == ModelDownloadType::DFF ? model->getDFF() : model->getTXD();
				const StringView baseUrl = self.getWebUrl();
				if (!baseUrl.empty())
				{
					NetCode::RPC::ModelUrl urlRPC(httplib::encode_uri(String(baseUrl.data()) + file.name), static_cast<uint8_t>(type), file.checksum);
					PacketHelper::send(urlRPC, peer);
				}
			}
			if (data != nullptr)
			{
				data->setRequestedFile(ModelDownloadType::NONE, 0);
			}
			return true;
		}
	} requestDownloadLinkHandler;

	struct FinishDownloadHandler : public SingleNetworkInEventHandler
	{
		CustomModelsComponent& self;

		FinishDownloadHandler(CustomModelsComponent& component)
			: self(component)
		{
		}

		bool onReceive(IPlayer& peer, NetworkBitStream& bs) override
		{
			self.eventDispatcher.dispatch(
				&PlayerModelsEventHandler::onPlayerFinishedDownloading,
				peer);

			NetCode::RPC::DownloadCompleted dlcompleted;
			PacketHelper::send(dlcompleted, peer);
			return true;
		}
	} finishDownloadHandler;

	struct VehicleRequestDownloadLinkEventHandler : public SingleNetworkInEventHandler
	{
		CustomModelsComponent& self;
		ModelDownloadType expectedType;

		VehicleRequestDownloadLinkEventHandler(CustomModelsComponent& component, ModelDownloadType type)
			: self(component)
			, expectedType(type)
		{
		}

		bool onReceive(IPlayer& peer, NetworkBitStream& bs) override
		{
			uint32_t checksum;
			if (!bs.readUINT32(checksum))
			{
				return false;
			}

			if (!self.supportsCustomVehicles(peer))
			{
				return true;
			}

			PlayerCustomVehicleModelsData* vehicleData
				= queryExtension<PlayerCustomVehicleModelsData>(peer);
			if (vehicleData == nullptr)
			{
				return true;
			}

			const auto itr = self.checksums.find(checksum);
			if (itr == self.checksums.end())
			{
				return true;
			}

			// Only registered vehicle assets may enter the extension download channel.
			const ModelDownloadType type = itr->second.first;
			ModelInfo* const model = itr->second.second;
			if (type != expectedType || model->getType() != CustomModels::Vehicle)
			{
				return true;
			}

			PlayerCustomModelsData* data = queryExtension<PlayerCustomModelsData>(peer);
			if (data != nullptr)
			{
				data->setRequestedFile(type, checksum);
			}
			if (self.hasDownloadEndpoint()
				&& self.eventDispatcher.stopAtFalse(
					[&peer, type, checksum](PlayerModelsEventHandler* handler)
					{
						return handler->onPlayerRequestDownload(peer, type, checksum);
					}))
			{
				const auto& file = type == ModelDownloadType::DFF ? model->getDFF() : model->getTXD();
				const StringView baseUrl = self.getWebUrl();
				if (!baseUrl.empty())
				{
					const String downloadUrl = httplib::encode_uri(String(baseUrl.data()) + file.name);
					const bool validScheme
						= downloadUrl.compare(0, 7, "http://") == 0
						|| downloadUrl.compare(0, 8, "https://") == 0;
					if (!validScheme || downloadUrl.size() > std::numeric_limits<uint8_t>::max())
					{
						self.core->logLn(LogLevel::Error, "[artwork:error] Vehicle download URL must be HTTP(S) and no longer than 255 bytes.");
						if (data != nullptr)
						{
							data->setRequestedFile(ModelDownloadType::NONE, 0);
						}
						return true;
					}

					if (vehicleData->beginPendingDownload(type, file.checksum))
					{
						NetCode::RPC::VehicleModelUrl urlRPC(
							downloadUrl,
							static_cast<uint8_t>(type),
							file.checksum);
						PacketHelper::send(urlRPC, peer);
					}
				}
			}
			if (data != nullptr)
			{
				data->setRequestedFile(ModelDownloadType::NONE, 0);
			}
			return true;
		}
	} vehicleRequestDffDownloadHandler;

	VehicleRequestDownloadLinkEventHandler vehicleRequestTxdDownloadHandler;

	struct FinishVehicleDownloadHandler : public SingleNetworkInEventHandler
	{
		CustomModelsComponent& self;

		FinishVehicleDownloadHandler(CustomModelsComponent& component)
			: self(component)
		{
		}

		bool onReceive(IPlayer& peer, NetworkBitStream& bs) override
		{
			uint8_t assetType = 0;
			uint32_t checksum = 0;
			if (!bs.readUINT8(assetType) || !bs.readUINT32(checksum)
				|| (assetType != static_cast<uint8_t>(ModelDownloadType::DFF)
					&& assetType != static_cast<uint8_t>(ModelDownloadType::TXD)))
			{
				return false;
			}

			if (!self.enabled || !self.vehicleModelsEnabled)
			{
				return true;
			}

			const auto type = static_cast<ModelDownloadType>(assetType);
			PlayerCustomVehicleModelsData* vehicleData
				= queryExtension<PlayerCustomVehicleModelsData>(peer);
			if (vehicleData == nullptr || !vehicleData->supportsCustomVehicleModels()
				|| !vehicleData->completePendingDownload(type, checksum))
			{
				// Ignore duplicate, mismatched, or unsolicited completions. A valid in-flight
				// transfer may finish after the local HTTP endpoint has stopped.
				return true;
			}

			self.eventDispatcher.dispatch(
				&PlayerModelsEventHandler::onPlayerFinishedDownloading,
				peer);

			NetCode::RPC::VehicleDownloadCompleted completed;
			PacketHelper::send(completed, peer);
			return true;
		}
	} finishVehicleDownloadHandler;

public:
	StringView componentName() const override
	{
		return "CustomModels";
	}

	SemanticVersion componentVersion() const override
	{
		return SemanticVersion(OMP_VERSION_MAJOR, OMP_VERSION_MINOR, OMP_VERSION_PATCH, BUILD_NUMBER);
	}

	CustomModelsComponent()
		: clientSupportsCustomVehicleModelsHandler(*this)
		, requestDownloadLinkHandler(*this)
		, finishDownloadHandler(*this)
		, vehicleRequestDffDownloadHandler(*this, ModelDownloadType::DFF)
		, vehicleRequestTxdDownloadHandler(*this, ModelDownloadType::TXD)
		, finishVehicleDownloadHandler(*this)
	{
	}

	~CustomModelsComponent()
	{
		if (core)
		{
			NetCode::RPC::RequestTXD::removeEventHandler(*core, &requestDownloadLinkHandler);
			NetCode::RPC::RequestDFF::removeEventHandler(*core, &requestDownloadLinkHandler);
			NetCode::RPC::FinishDownload::removeEventHandler(*core, &finishDownloadHandler);
			NetCode::RPC::RequestVehicleDFF::removeEventHandler(*core, &vehicleRequestDffDownloadHandler);
			NetCode::RPC::RequestVehicleTXD::removeEventHandler(*core, &vehicleRequestTxdDownloadHandler);
			NetCode::RPC::FinishVehicleDownload::removeEventHandler(*core, &finishVehicleDownloadHandler);
			NetCode::RPC::ClientSupportsCustomVehicleModels::removeEventHandler(*core, &clientSupportsCustomVehicleModelsHandler);
			players->getPlayerConnectDispatcher().removeEventHandler(this);
		}

		// Stop the HTTP worker before releasing the model metadata it serves.
		delete webServer;
		webServer = nullptr;

		// storage owns each ModelInfo allocated by addCustomModel().
		for (ModelInfo* model : storage)
		{
			delete model;
		}
		storage.clear();
	}

	void provideConfiguration(ILogger& logger, IEarlyConfig& config, bool defaults) override
	{
		if (defaults)
		{
			config.setBool("artwork.enable", enabled);
			config.setString("artwork.cdn", cdn);
			config.setString("artwork.models_path", modelsPath);
			config.setInt("network.http_threads", httpThreads);
			config.setInt("artwork.port", modelsPort);
			config.setString("artwork.web_server_bind", webServerBindAddress);
			config.setBool("artwork.show_crc_logs", showCRCLogs);
			config.setBool("artwork.experimental_vehicle_models", vehicleModelsEnabled);
		}
		else
		{
			// Set default values if options are not set.
			// We don't want to crash.
			if (config.getType("artwork.enable") == ConfigOptionType_None)
			{
				config.setBool("artwork.enable", enabled);
			}
			if (config.getType("artwork.cdn") == ConfigOptionType_None)
			{
				config.setString("artwork.cdn", cdn);
			}
			if (config.getType("artwork.models_path") == ConfigOptionType_None)
			{
				config.setString("artwork.models_path", modelsPath);
			}
			if (config.getType("network.http_threads") == ConfigOptionType_None)
			{
				config.setInt("network.http_threads", httpThreads);
			}
			// Add the web server port to support some old TCP plugin like Incognito's audio plugin
			if (config.getType("artwork.port") == ConfigOptionType_None)
			{
				config.setInt("artwork.port", modelsPort);
			}
			// We provide a way for users to set webserver bind address, sometimes they want it
			// To be different rather than 127.0.0.1 or server's public IP. For example, in some
			// Situations you want to bind it to 0.0.0.0, like in a docker container.
			if (config.getType("artwork.web_server_bind") == ConfigOptionType_None)
			{
				config.setString("artwork.web_server_bind", webServerBindAddress);
			}
			if (config.getType("artwork.show_crc_logs") == ConfigOptionType_None)
			{
				config.setBool("artwork.show_crc_logs", showCRCLogs);
			}
			if (config.getType("artwork.experimental_vehicle_models") == ConfigOptionType_None)
			{
				config.setBool("artwork.experimental_vehicle_models", vehicleModelsEnabled);
			}
		}
	}

	void onLoad(ICore* core) override
	{
		this->core = core;
		players = &core->getPlayers();
		players->getPlayerConnectDispatcher().addEventHandler(this);

		enabled = *core->getConfig().getBool("artwork.enable");
		modelsPath = String(trim(core->getConfig().getString("artwork.models_path")));
		cdn = String(trim(core->getConfig().getString("artwork.cdn")));
		httpThreads = *core->getConfig().getInt("network.http_threads");
		webServerBindAddress = String(core->getConfig().getString("artwork.web_server_bind"));
		showCRCLogs = *core->getConfig().getBool("artwork.show_crc_logs");
		vehicleModelsEnabled = *core->getConfig().getBool("artwork.experimental_vehicle_models");

		NetCode::RPC::RequestTXD::addEventHandler(*core, &requestDownloadLinkHandler);
		NetCode::RPC::RequestDFF::addEventHandler(*core, &requestDownloadLinkHandler);
		NetCode::RPC::FinishDownload::addEventHandler(*core, &finishDownloadHandler);
		NetCode::RPC::RequestVehicleDFF::addEventHandler(*core, &vehicleRequestDffDownloadHandler);
		NetCode::RPC::RequestVehicleTXD::addEventHandler(*core, &vehicleRequestTxdDownloadHandler);
		NetCode::RPC::FinishVehicleDownload::addEventHandler(*core, &finishVehicleDownloadHandler);
		NetCode::RPC::ClientSupportsCustomVehicleModels::addEventHandler(*core, &clientSupportsCustomVehicleModelsHandler);

		if (!enabled)
			return;

		// Attempt to create models folder.
		try
		{
			ghc::filesystem::create_directory(modelsPath);
		}
		catch (ghc::filesystem::filesystem_error exception)
		{
			enabled = false;
			core->logLn(LogLevel::Error, "[artwork:error] Unable to create models path (%.*s).", PRINT_VIEW(modelsPath));
			core->logLn(LogLevel::Error, "%s", exception.what());
			return;
		}

		if (!cdn.empty())
		{
			if (!std::regex_match(cdn, rUri))
			{
				core->logLn(LogLevel::Warning, "[artwork:warn] CDN URL '%.*s' seems to be invalid, falling back to the local web server.", PRINT_VIEW(cdn));
				cdn.clear();
			}
			else
			{
				if (cdn.back() != '/')
				{
					cdn.push_back('/');
				}
				core->logLn(LogLevel::Message, "[artwork:info] Using CDN '%.*s'", PRINT_VIEW(cdn));
				usingCdn = true;
			}
		}

		// Configure CDN mode before loading models so addCustomModel() does not start a local HTTP server unnecessarily.
		loadArtConfig();
	}

	void loadArtConfig()
	{
		std::ifstream artconfig { modelsPath + "/artconfig.txt" };

		if (artconfig.is_open())
		{
			core->logLn(LogLevel::Message, "[artwork:info] Loading artconfig.txt");
			std::string line;
			std::smatch match;
			while (std::getline(artconfig, line))
			{
				if (std::regex_match(line, match, rAddCharModel))
				{
					addCustomModel(ModelType::Skin, std::atoi(match[2].str().c_str()), std::atoi(match[1].str().c_str()), match[3].str(), match[4].str());
				}
				else if (std::regex_match(line, match, rAddSimpleModel))
				{
					addCustomModel(ModelType::Object, std::atoi(match[3].str().c_str()), std::atoi(match[2].str().c_str()), match[4].str(), match[5].str(), std::atoi(match[1].str().c_str()));
				}
				else if (std::regex_match(line, match, rAddSimpleModelTimed))
				{
					addCustomModel(ModelType::Object, std::atoi(match[3].str().c_str()), std::atoi(match[2].str().c_str()), match[4].str(), match[5].str(), std::atoi(match[1].str().c_str()), std::atoi(match[6].str().c_str()), std::atoi(match[7].str().c_str()));
				}
				else if (std::regex_match(line, match, rAddVehicleModel))
				{
					addCustomModel(CustomModels::Vehicle, std::atoi(match[2].str().c_str()), std::atoi(match[1].str().c_str()), match[3].str(), match[4].str());
				}
			}
		}
	}

	void startWebServer()
	{
		if (usingCdn || (webServer && webServer->is_running()))
		{
			return;
		}

		StringView bindAddress;
		StringView networkBindAddress = core->getConfig().getString("network.bind");
		if (webServerBindAddress.size())
		{
			bindAddress = StringView(webServerBindAddress.c_str(), webServerBindAddress.size());
		}
		else
		{
			if (!networkBindAddress.empty())
			{
				bindAddress = networkBindAddress;
			}
		}

		WebServer* candidate = new WebServer(core, modelsPath, bindAddress,
			*core->getConfig().getInt("artwork.port"),
			core->getConfig().getString("network.public_addr"), httpThreads, registeredAssets);

		if (!candidate->is_running())
		{
			delete candidate;
			core->logLn(LogLevel::Error, "Failed to start web server");
			return;
		}

		webServer = candidate;

		// The web server may be created after players have already connected.
		// Rebuild its allow-list and re-enable clients that negotiated vehicle support.
		for (IPlayer* player : players->entries())
		{
			PlayerCustomVehicleModelsData* vehicleData
				= queryExtension<PlayerCustomVehicleModelsData>(*player);
			if (vehicleData)
			{
				// This extension exists only for players that explicitly opted in earlier.
				vehicleData->setSupported(vehicleModelsEnabled);
			}

			if (player->getClientVersion() == ClientVersion::ClientVersion_SAMP_03DL
				|| (vehicleData && vehicleData->supportsCustomVehicleModels()))
			{
				webServer->allowIPAddress(player->getNetworkData().networkID.address.v4);
			}

			// If the endpoint was unavailable during capability negotiation, resume the
			// opted-in client's vehicle manifest now that downloads can actually succeed.
			if (vehicleData && vehicleData->supportsCustomVehicleModels())
			{
				PlayerCustomModelsData* playerData
					= queryExtension<PlayerCustomModelsData>(*player);
				if (playerData && playerData->hasClientInitialized())
				{
					sendCustomVehicleModels(*player);
				}
			}
		}

		core->logLn(LogLevel::Message, "Web server is running on %.*s", PRINT_VIEW(webServer->getUrl()));

		const auto maxPlayers = *core->getConfig().getInt("max_players");
		if (httpThreads < maxPlayers / 2)
		{
			core->logLn(LogLevel::Warning, "HTTP threads count value (%d) is much lower than MAX_PLAYERS (%d). Players may encounter slow download times.", httpThreads, maxPlayers);
		}
	}

	void free() override
	{
		delete this;
	}

	void reset() override
	{
		// Don't reset pool here - SAMP client can't handle it and will crash.
		// Why can't we have nice things? :(

		// Someone complained about this. Let's parse artconfig again.
		loadArtConfig();
	}

	StringView getWebUrl()
	{
		if (usingCdn)
		{
			return cdn;
		}
		return webServer && webServer->is_running() ? webServer->getUrl() : StringView();
	}

	bool addCustomModel(ModelType type, int32_t id, int32_t baseId, StringView dffName, StringView txdName, int32_t virtualWorld = -1, uint8_t timeOn = 0, uint8_t timeOff = 0) override
	{
		if (!enabled)
		{
			return false;
		}

		if (type == ModelType::Skin && !(id >= MIN_CUSTOM_SKIN_ID && id <= MAX_CUSTOM_SKIN_ID))
			return false;
		else if (type == ModelType::Object && !(id >= MIN_CUSTOM_OBJECT_ID && id <= MAX_CUSTOM_OBJECT_ID))
			return false;
		else if (type == CustomModels::Vehicle
			&& (!CustomModels::isCustomVehicleModelId(id)
				|| !CustomModels::isSupportedBaseVehicleModel(baseId)
				|| !vehicleModelsEnabled))
		{
			if (!vehicleModelsEnabled)
			{
				core->logLn(LogLevel::Warning, "[artwork:warn] Custom vehicle models are experimental and disabled. Set artwork.experimental_vehicle_models to true to enable them.");
			}
			return false;
		}
		else if (type != ModelType::Skin && type != ModelType::Object && type != CustomModels::Vehicle)
			return false;
		else if (baseModels.find(id) != baseModels.end())
		{
			// Sadly this error will be displayed on gmx. Dunno what do about it.
			core->logLn(LogLevel::Error, "[artwork:error] Model %d is already in use", id);
			return false;
		}

		if (!isSafeModelFilePath(modelsPath, dffName) || !hasExpectedFileExtension(dffName, ".dff")
			|| !isSafeModelFilePath(modelsPath, txdName) || !hasExpectedFileExtension(txdName, ".txd"))
		{
			core->logLn(LogLevel::Error, "[artwork:error] Cannot add custom model. DFF/TXD files must use the matching extension and resolve inside the models directory.");
			return false;
		}

		ModelFile dff(modelsPath, dffName);
		ModelFile txd(modelsPath, txdName);

		if (!dff.size)
		{
			core->logLn(LogLevel::Error, "[artwork:error] Cannot add custom model. %.*s doesn't exist", PRINT_VIEW(dffName));
			return false;
		}

		if (!txd.size)
		{
			core->logLn(LogLevel::Error, "[artwork:error] Cannot add custom model. %.*s doesn't exist", PRINT_VIEW(txdName));
			return false;
		}

		// The wire format stores each asset size in an unsigned 32-bit field.
		if (dff.size > std::numeric_limits<uint32_t>::max()
			|| txd.size > std::numeric_limits<uint32_t>::max())
		{
			core->logLn(LogLevel::Error, "[artwork:error] Cannot add custom model. DFF/TXD size exceeds the supported 32-bit transfer limit.");
			return false;
		}

		// Checksums are download lookup keys. Reject cross-type collisions and
		// same-type CRC32 collisions with different contents, or clients may
		// receive an asset that does not match the model manifest.
		if (dff.checksum == txd.checksum)
		{
			core->logLn(LogLevel::Error, "[artwork:error] Cannot add custom model. DFF and TXD have the same CRC32 checksum.");
			return false;
		}
		const auto existingDffChecksum = checksums.find(dff.checksum);
		const auto existingTxdChecksum = checksums.find(txd.checksum);
		auto hasCompatibleChecksum = [&](const auto& existing, ModelDownloadType expectedType, const ModelFile& candidate)
		{
			if (existing == checksums.end())
			{
				return true;
			}
			if (existing->second.first != expectedType)
			{
				return false;
			}

			ModelInfo* registeredModel = existing->second.second;
			// A checksum cannot be shared across native artwork and the separate vehicle protocol.
			if (registeredModel->getType() != type)
			{
				return false;
			}
			const ModelFile& registeredFile = expectedType == ModelDownloadType::DFF
				? registeredModel->getDFF()
				: registeredModel->getTXD();
			if (registeredFile.size != candidate.size)
			{
				return false;
			}
			if (registeredFile.name == candidate.name)
			{
				return true;
			}

			return haveSameFileContents(
				String(modelsPath) + "/" + candidate.name,
				String(modelsPath) + "/" + registeredFile.name);
		};
		if (!hasCompatibleChecksum(existingDffChecksum, ModelDownloadType::DFF, dff))
		{
			core->logLn(LogLevel::Error, "[artwork:error] Cannot add custom model. DFF CRC32 checksum is already registered for a different asset or file type.");
			return false;
		}
		if (!hasCompatibleChecksum(existingTxdChecksum, ModelDownloadType::TXD, txd))
		{
			core->logLn(LogLevel::Error, "[artwork:error] Cannot add custom model. TXD CRC32 checksum is already registered for a different asset or file type.");
			return false;
		}

		// Do not register or advertise assets until clients can actually fetch them.
		startWebServer();
		if (!hasDownloadEndpoint())
		{
			core->logLn(LogLevel::Error, "[artwork:error] Cannot add custom model because no download endpoint is available.");
			return false;
		}

		if (type == CustomModels::Vehicle)
		{
			const StringView baseUrl = getWebUrl();
			const String encodedDffUrl = httplib::encode_uri(String(baseUrl.data()) + dff.name);
			const String encodedTxdUrl = httplib::encode_uri(String(baseUrl.data()) + txd.name);
			const size_t maxUrlLength = std::numeric_limits<uint8_t>::max();
			const auto isSupportedVehicleUrl = [maxUrlLength](const String& url)
			{
				return (url.compare(0, 7, "http://") == 0
					|| url.compare(0, 8, "https://") == 0)
					&& !url.empty() && url.size() <= maxUrlLength;
			};
			if (!isSupportedVehicleUrl(encodedDffUrl) || !isSupportedVehicleUrl(encodedTxdUrl))
			{
				core->logLn(LogLevel::Error, "[artwork:error] Cannot add custom vehicle model. Encoded DFF/TXD URLs must use HTTP(S) and fit in 255 bytes.");
				return false;
			}
		}

		if (!registeredAssets.registerAssets(dff, txd))
		{
			core->logLn(LogLevel::Error, "[artwork:error] Cannot add custom model. A registered filename is already associated with different asset contents.");
			return false;
		}

		if (showCRCLogs)
		{
			core->logLn(LogLevel::Message, "[artwork:crc] %.*s CRC = 0x%X", PRINT_VIEW(txdName), txd.checksum);
			core->logLn(LogLevel::Message, "[artwork:crc] %.*s CRC = 0x%X", PRINT_VIEW(dffName), dff.checksum);
		}

		auto model = storage.emplace_back(new ModelInfo(type, id, baseId, dff, txd, virtualWorld, timeOn, timeOff));

		if (!model)
			return false;

		baseModels.emplace(id, baseId);
		checksums.emplace(dff.checksum, std::make_pair(ModelDownloadType::DFF, model));
		checksums.emplace(txd.checksum, std::make_pair(ModelDownloadType::TXD, model));

		sendNewCustomModel(*model);
		return true;
	}

	bool getBaseModel(uint32_t& baseModelIdOrInput, uint32_t& customModel) override
	{
		// Check if model is default one (base).
		// If so, there's no custom model to be returned.
		if (baseModelIdOrInput < MIN_CUSTOM_SKIN_ID)
			return false;

		// Check if input is valid custom model.
		// If not treat is as base model.
		auto itr = baseModels.find(baseModelIdOrInput);
		if (itr == baseModels.end())
		{
			return false;
		}

		// Input is a custom model.
		customModel = baseModelIdOrInput;
		baseModelIdOrInput = itr->second;
		return true;
	}

	StringView getModelNameFromChecksum(uint32_t checksum) const override
	{
		const auto& itr = checksums.find(checksum);

		if (itr == checksums.end())
			return StringView();

		const auto& [type, model] = itr->second;
		const auto& file = type == ModelDownloadType::DFF ? model->getDFF() : model->getTXD();
		return file.name;
	}

	void onPlayerClientInit(IPlayer& player) override
	{
		PlayerCustomModelsData* data = queryExtension<PlayerCustomModelsData>(player);
		if (data)
		{
			data->markClientInitialized();
		}

		// Native 0.3DL receives skins/objects on its stock artwork RPCs.
		sendCustomModels(player);

		// Custom vehicles use their independent extension manifest (RPC 220).
		// Capability may arrive before or after client initialization.
		if (supportsCustomVehicles(player))
		{
			sendCustomVehicleModels(player);
		}

		// Only native DL artwork needs the virtual-world refresh workaround.
		if (player.getClientVersion() == ClientVersion::ClientVersion_SAMP_03DL)
		{
			forceCustomModelDownload(player);
		}
	}

	IEventDispatcher<PlayerModelsEventHandler>& getEventDispatcher() override
	{
		return eventDispatcher;
	}

	void onPlayerConnect(IPlayer& player) override
	{
		player.addExtension(new PlayerCustomModelsData(player), true);

		if (player.getClientVersion() != ClientVersion::ClientVersion_SAMP_03DL || !webServer)
		{
			return;
		}

		webServer->allowIPAddress(player.getNetworkData().networkID.address.v4);
	}

	void onPlayerDisconnect(IPlayer& player, PeerDisconnectReason reason) override
	{
		const bool supportsCustomVehicles = queryExtension<IPlayerCustomVehicleModelsData>(player) != nullptr;
		if ((player.getClientVersion() != ClientVersion::ClientVersion_SAMP_03DL && !supportsCustomVehicles) || !webServer)
		{
			return;
		}

		webServer->removeIPAddress(player.getNetworkData().networkID.address.v4);
	}

	bool isValidCustomModel(int32_t modelId) const override
	{
		return baseModels.find(modelId) != baseModels.end();
	}

	bool getCustomModelPath(int32_t modelId, StringView& dffPath, StringView& txdPath) const override
	{
		auto model = std::find_if(storage.begin(), storage.end(), [&](const auto& model)
			{
				return model->getId() == modelId;
			});

		if (model != storage.end())
		{
			dffPath = (*model)->getDFF().name;
			txdPath = (*model)->getTXD().name;
			return true;
		}
		return false;
	}
};

COMPONENT_ENTRY_POINT()
{
	return new CustomModelsComponent();
}
