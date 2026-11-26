/*
 *  This Source Code Form is subject to the terms of the Mozilla Public License,
 *  v. 2.0. If a copy of the MPL was not distributed with this file, You can
 *  obtain one at http://mozilla.org/MPL/2.0/.
 *
 *  The original code is copyright (c) 2022, open.mp team and contributors.
 */

#pragma once

#include <bitstream.hpp>
#include <network.hpp>
#include <packet.hpp>
#include <types.hpp>

namespace NetCode
{
namespace RPC
{
// Experimental custom-vehicle extension RPCs use a separate ID range.
// Native SA-MP/0.3.DL artwork RPCs 179 and 181-185 remain unchanged.
struct ClientSupportsCustomVehicleModels : NetworkPacketBase<221, NetworkPacketType::RPC, OrderingChannel_SyncRPC>
{
	bool read(NetworkBitStream& bs)
	{
		return true;
	}
};

struct ServerSupportsCustomVehicleModels : NetworkPacketBase<222, NetworkPacketType::RPC, OrderingChannel_SyncRPC>
{
	bool read(NetworkBitStream& bs)
	{
		return false;
	}

	void write(NetworkBitStream& bs) const
	{
	}
};

struct VehicleModelRequest : NetworkPacketBase<220, NetworkPacketType::RPC, OrderingChannel_SyncRPC>
{
	uint32_t poolID;
	uint8_t type;
	int32_t virtualWorld;
	int32_t baseId;
	int32_t newId;
	uint32_t dffChecksum;
	uint32_t txdChecksum;
	uint32_t dffSize;
	uint32_t txdSize;
	uint8_t timeOn;
	uint8_t timeOff;
	uint32_t Count;

	VehicleModelRequest(int32_t id, uint32_t count)
		: poolID(id)
		, Count(count)
	{
	}

	void write(NetworkBitStream& bs) const
	{
		bs.writeUINT32(poolID);
		bs.writeINT32(Count);
		bs.writeUINT8(type);
		bs.writeINT32(virtualWorld);
		bs.writeINT32(baseId);
		bs.writeINT32(newId);
		bs.writeUINT32(dffChecksum);
		bs.writeUINT32(txdChecksum);
		bs.writeUINT32(dffSize);
		bs.writeUINT32(txdSize);
		bs.writeUINT8(timeOn);
		bs.writeUINT8(timeOff);
	}
};

struct VehicleModelUrl : NetworkPacketBase<225, NetworkPacketType::RPC, OrderingChannel_DownloadRequest>
{
	String downloadLink;
	uint8_t fileType;
	uint32_t fileChecksum;

	VehicleModelUrl(StringView link, uint8_t type, uint32_t checksum)
		: downloadLink(link)
		, fileType(type)
		, fileChecksum(checksum)
	{
	}

	void write(NetworkBitStream& bs) const
	{
		bs.writeUINT8(6);
		bs.writeUINT8(fileType);
		bs.writeUINT32(fileChecksum);
		bs.writeDynStr8(downloadLink);
	}
};

struct VehicleDownloadCompleted : NetworkPacketBase<227, NetworkPacketType::RPC, OrderingChannel_DownloadRequest>
{
	void write(NetworkBitStream& bs) const { }
};

struct FinishVehicleDownload : NetworkPacketBase<226, NetworkPacketType::RPC, OrderingChannel_SyncRPC>
{
	bool read(NetworkBitStream& bs)
	{
		return true;
	}
};

struct RequestVehicleTXD : NetworkPacketBase<224, NetworkPacketType::RPC, OrderingChannel_SyncRPC>
{
	uint32_t checksum;
	bool read(NetworkBitStream& bs)
	{
		return bs.readUINT32(checksum);
	}
};

struct RequestVehicleDFF : NetworkPacketBase<223, NetworkPacketType::RPC, OrderingChannel_SyncRPC>
{
	uint32_t checksum;
	bool read(NetworkBitStream& bs)
	{
		return bs.readUINT32(checksum);
	}
};

// Native SA-MP/0.3.DL artwork protocol. These IDs and payloads are intentionally untouched.
struct ModelRequest : NetworkPacketBase<179, NetworkPacketType::RPC, OrderingChannel_SyncRPC>
{
	uint32_t poolID;
	uint8_t type;
	int32_t virtualWorld;
	int32_t baseId;
	int32_t newId;
	uint32_t dffChecksum;
	uint32_t txdChecksum;
	uint32_t dffSize;
	uint32_t txdSize;
	uint8_t timeOn;
	uint8_t timeOff;
	uint32_t Count;

	ModelRequest(int32_t id, uint32_t count)
		: poolID(id)
		, Count(count)
	{
	}

	void write(NetworkBitStream& bs) const
	{
		bs.writeUINT32(poolID);
		bs.writeINT32(Count);
		bs.writeUINT8(type);
		bs.writeINT32(virtualWorld);
		bs.writeINT32(baseId);
		bs.writeINT32(newId);
		bs.writeUINT32(dffChecksum);
		bs.writeUINT32(txdChecksum);
		bs.writeUINT32(dffSize);
		bs.writeUINT32(txdSize);
		bs.writeUINT8(timeOn);
		bs.writeUINT8(timeOff);
	}
};

struct ModelUrl : NetworkPacketBase<183, NetworkPacketType::RPC, OrderingChannel_DownloadRequest>
{
	String downloadLink;
	uint8_t fileType;
	uint32_t fileChecksum;

	ModelUrl(StringView link, uint8_t type, uint32_t checksum)
		: downloadLink(link)
		, fileType(type)
		, fileChecksum(checksum)
	{
	}

	void write(NetworkBitStream& bs) const
	{
		bs.writeUINT8(6);
		bs.writeUINT8(fileType);
		bs.writeUINT32(fileChecksum);
		bs.writeDynStr8(downloadLink);
	}
};

struct DownloadCompleted : NetworkPacketBase<185, NetworkPacketType::RPC, OrderingChannel_DownloadRequest>
{
	void write(NetworkBitStream& bs) const { }
};

struct FinishDownload : NetworkPacketBase<184, NetworkPacketType::RPC, OrderingChannel_SyncRPC>
{
	bool read(NetworkBitStream& bs)
	{
		return true;
	}
};

struct RequestTXD : NetworkPacketBase<182, NetworkPacketType::RPC, OrderingChannel_SyncRPC>
{
	uint32_t checksum;
	bool read(NetworkBitStream& bs)
	{
		return bs.readUINT32(checksum);
	}
};

struct RequestDFF : NetworkPacketBase<181, NetworkPacketType::RPC, OrderingChannel_SyncRPC>
{
	uint32_t checksum;
	bool read(NetworkBitStream& bs)
	{
		return bs.readUINT32(checksum);
	}
};

}
}
