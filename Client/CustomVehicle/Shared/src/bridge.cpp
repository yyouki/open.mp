#include <custom_vehicle/bridge.h>
#include <custom_vehicle/network_adapter.hpp>

#include <new>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
using namespace custom_vehicle::protocol;

class CallbackTransport final : public RpcTransport
{
public:
	explicit CallbackTransport(const custom_vehicle_callbacks_v1& callbacks) noexcept
		: callbacks_(callbacks)
	{
	}

	bool sendRpc(RpcId id, const std::uint8_t* payload, std::size_t size) noexcept override
	{
		return callbacks_.send_rpc(
			callbacks_.user_data,
			static_cast<std::uint8_t>(id),
			payload,
			size) != 0;
	}

private:
	const custom_vehicle_callbacks_v1& callbacks_;
};

class CallbackDownloadSink final : public AssetDownloadSink
{
public:
	explicit CallbackDownloadSink(const custom_vehicle_callbacks_v1& callbacks) noexcept
		: callbacks_(callbacks)
	{
	}

	bool beginDownload(const AssetDownload& download) noexcept override
	{
		return callbacks_.begin_download(
			callbacks_.user_data,
			static_cast<std::uint8_t>(download.type),
			download.checksum,
			download.expectedSize,
			download.url.data(),
			download.url.size()) != 0;
	}

private:
	const custom_vehicle_callbacks_v1& callbacks_;
};
} // namespace

struct custom_vehicle_adapter
{
	custom_vehicle_callbacks_v1 callbacks;
	CallbackTransport transport;
	CallbackDownloadSink downloader;
	custom_vehicle::protocol::ClientNetworkAdapter adapter;

	explicit custom_vehicle_adapter(const custom_vehicle_callbacks_v1& value)
		: callbacks(value)
		, transport(callbacks)
		, downloader(callbacks)
		, adapter(transport, downloader)
	{
	}
};

extern "C"
{
custom_vehicle_adapter* custom_vehicle_adapter_create(const custom_vehicle_callbacks_v1* callbacks)
{
	if (callbacks == nullptr
		|| callbacks->struct_size < sizeof(custom_vehicle_callbacks_v1)
		|| callbacks->abi_version != CUSTOM_VEHICLE_BRIDGE_ABI_VERSION
		|| callbacks->send_rpc == nullptr
		|| callbacks->begin_download == nullptr)
	{
		return nullptr;
	}

	try
	{
		return new custom_vehicle_adapter(*callbacks);
	}
	catch (...)
	{
		return nullptr;
	}
}

void custom_vehicle_adapter_destroy(custom_vehicle_adapter* adapter)
{
	// The host must stop and join downloader workers before destroying the adapter.
	delete adapter;
}

int32_t custom_vehicle_adapter_begin_connection(custom_vehicle_adapter* adapter)
{
	return adapter != nullptr && adapter->adapter.beginConnection() ? 1 : 0;
}

void custom_vehicle_adapter_end_connection(custom_vehicle_adapter* adapter)
{
	if (adapter != nullptr)
	{
		adapter->adapter.endConnection();
	}
}

int32_t custom_vehicle_adapter_handle_server_rpc(
	custom_vehicle_adapter* adapter,
	uint8_t rpc_id,
	const uint8_t* payload,
	size_t payload_size)
{
	if (adapter == nullptr || (payload == nullptr && payload_size != 0))
	{
		return -1;
	}

	const auto result = adapter->adapter.handleServerRpc(rpc_id, payload, payload_size);
	return result == custom_vehicle::protocol::RpcDisposition::Consume ? 1 : 0;
}

int32_t custom_vehicle_adapter_complete_asset(
	custom_vehicle_adapter* adapter,
	uint8_t asset_type,
	uint32_t checksum,
	const uint8_t* data,
	size_t data_size)
{
	if (adapter == nullptr || (data == nullptr && data_size != 0))
	{
		return 0;
	}

	custom_vehicle::protocol::AssetType type;
	switch (asset_type)
	{
		case static_cast<uint8_t>(custom_vehicle::protocol::AssetType::Dff):
			type = custom_vehicle::protocol::AssetType::Dff;
			break;
		case static_cast<uint8_t>(custom_vehicle::protocol::AssetType::Txd):
			type = custom_vehicle::protocol::AssetType::Txd;
			break;
		default:
			return 0;
	}

	return adapter->adapter.completeAsset(type, checksum, data, data_size) ? 1 : 0;
}

int32_t custom_vehicle_adapter_retry_active_asset(custom_vehicle_adapter* adapter)
{
	return adapter != nullptr && adapter->adapter.retryActiveAsset() ? 1 : 0;
}

size_t custom_vehicle_adapter_model_count(const custom_vehicle_adapter* adapter)
{
	if (adapter == nullptr)
	{
		return 0;
	}

	try
	{
		return adapter->adapter.vehicleModels().size();
	}
	catch (...)
	{
		return 0;
	}
}

int32_t custom_vehicle_adapter_copy_model(
	const custom_vehicle_adapter* adapter,
	size_t index,
	custom_vehicle_model_request_v1* output)
{
	if (adapter == nullptr || output == nullptr)
	{
		return 0;
	}

	try
	{
		const std::vector<custom_vehicle::protocol::ModelRequest> models =
			adapter->adapter.vehicleModels();
		if (index >= models.size())
		{
			return 0;
		}

		const auto& model = models[index];
		custom_vehicle_model_request_v1 result {};
		result.pool_id = model.poolId;
		result.model_count = model.modelCount;
		result.type = static_cast<std::uint8_t>(model.type);
		result.virtual_world = model.virtualWorld;
		result.base_id = model.baseId;
		result.new_id = model.newId;
		result.dff_checksum = model.dffChecksum;
		result.txd_checksum = model.txdChecksum;
		result.dff_size = model.dffSize;
		result.txd_size = model.txdSize;
		result.time_on = model.timeOn;
		result.time_off = model.timeOff;
		*output = result;
		return 1;
	}
	catch (...)
	{
		return 0;
	}
}
} // extern "C"
