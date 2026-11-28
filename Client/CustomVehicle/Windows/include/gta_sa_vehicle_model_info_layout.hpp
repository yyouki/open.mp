#pragma once

#include <cstddef>
#include <cstdint>

/*
 * Compile-time layout reference for GTA: San Andreas 1.0 US CVehicleModelInfo.
 *
 * This is evidence-only metadata. It does not provide native addresses, does
 * not dereference game pointers, and must not be used until the loaded GTA
 * executable has been positively identified as the matching 32-bit build.
 *
 * Layout evidence:
 * - gta-reversed/gta-reversed, source/game_sa/Models/VehicleModelInfo.h,
 *   Models/ClumpModelInfo.h, Models/BaseModelInfo.h, StreamingInfo.h,
 *   Streaming.h
 *   commit 85bce0be4bd3229eb5ef445d3a7ac2e611a994f4
 * - multitheftauto/mtasa-blue, Client/game_sa/CModelInfoSA.h
 *   commit 002d0274c63740c512ae06daab67e0b6e770745a
 */

#if !defined(_WIN32) || !(defined(_M_IX86) || defined(__i386__))
#error "GTA SA native vehicle model-info layouts are valid only for Windows x86"
#endif

namespace custom_vehicle::gta_sa
{
// Exact table facts for the referenced GTA SA 1.0 US executable only.
// These constants are compile-time evidence, not permission to dereference memory.
inline constexpr std::uint32_t ModelInfoPointerTableAddress = 0x00A9B0C8;
inline constexpr std::size_t ModelInfoPointerTableCount = 20000;
inline constexpr std::uint32_t StreamingInfoTableAddress = 0x008E4CC0;
inline constexpr std::size_t StreamingInfoTableCount = 26316;

// Separate TXD resource namespace and CTxdPool entry layout.
inline constexpr std::int32_t FirstTxdResourceId = 20000;
inline constexpr std::size_t TxdPoolCapacity = 5000;
inline constexpr std::int32_t LastTxdResourceId = 24999;

constexpr std::int32_t txdResourceIdForPoolSlot(std::int32_t slot) noexcept
{
    return slot >= 0 && slot < static_cast<std::int32_t>(TxdPoolCapacity)
        ? FirstTxdResourceId + slot
        : -1;
}

struct TxdDefLayout
{
    std::uint32_t rw_dictionary;
    std::int16_t reference_count;
    std::int16_t parent_index;
    std::uint32_t name_hash;
};

// 32-bit CStreamingInfo record. Dynamic entries must be initialized separately;
// only archive image/offset/size may be copied from the compatible base model.
struct StreamingInfoLayout
{
	std::int16_t next_index;
	std::int16_t previous_index;
	std::int16_t next_index_on_cd;
	std::uint8_t flags;
	std::uint8_t image_id;
	std::uint32_t cd_offset;
	std::uint32_t cd_size;
	std::uint8_t load_state;
	std::uint8_t alignment_padding[3];
};

// The fixed 0x20-byte CBaseModelInfo prefix of CVehicleModelInfo.
struct BaseModelInfoLayout
{
	std::uint32_t vtable;
	std::uint32_t model_name_hash;
	std::uint16_t reference_count;
	std::int16_t texture_dictionary_index;
	std::uint8_t alpha;
	std::uint8_t two_d_effect_count;
	std::int16_t two_d_effect_index;
	std::int16_t object_info_index;
	std::uint16_t flags;
	std::uint32_t collision_model;
	float draw_distance;
	std::uint32_t renderware_object;
};

// CClumpModelInfo is CBaseModelInfo plus a 32-bit animation-file union
// (char* filename or uint32 animation index) in the pinned x86 build.
struct ClumpModelInfoLayout
{
    BaseModelInfoLayout base;
    std::uint32_t animation_file_or_index;
};

inline constexpr std::uint8_t StreamingLoadStateNotLoaded = 0;
inline constexpr std::uint32_t StreamingSectorSizeInBytes = 2048;

// Creates a fresh streaming record for one file in a registered GTA archive.
// offset_in_blocks and size_in_blocks are IMG sectors, not byte counts.
// next_index_on_cd is the next model ID in the same archive chain, or -1 when
// this file has no following entry in the archive.
constexpr StreamingInfoLayout makeStreamingInfoForArchive(
	std::uint8_t archive_id,
	std::uint32_t offset_in_blocks,
	std::uint32_t size_in_blocks,
	std::int16_t next_index_on_cd = -1) noexcept
{
	return StreamingInfoLayout{
		-1,                         // next_index
		-1,                         // previous_index
		next_index_on_cd,           // next file in this archive
		0,                          // request flags
		archive_id,                 // CStreaming::ms_files image index
		offset_in_blocks,
		size_in_blocks,
		StreamingLoadStateNotLoaded,
		{ 0, 0, 0 }                 // deterministic padding
	};
}

// Matches CStreamingInfo::Init followed by the safe MTA archive-metadata copy.
// No linked-list links, request flags, or load state are shared with the base ID.
constexpr StreamingInfoLayout makeIndependentStreamingInfo(
	const StreamingInfoLayout& base) noexcept
{
	return makeStreamingInfoForArchive(
		base.image_id,
		base.cd_offset,
		base.cd_size);
}

// Created by CVehicleModelInfo::SetupCommonData in the referenced GTA build.
// SetClump allocates one VehicleStructure; DeleteRwObject releases it.
inline constexpr std::size_t VehicleStructurePoolInitialCapacity = 50;

struct VehicleUpgradePositionLayout
{
	float position[3];
	float rotation[4];
	std::int32_t parent_component_id;
};

struct VehicleStructureLayout
{
	float dummy_positions[15][3];
	VehicleUpgradePositionLayout upgrade_positions[18];
	void* extras[6];
	std::uint8_t number_of_extras;
	std::uint8_t alignment_padding[3];
	std::uint32_t damageable_component_mask;
};

/*
 * Unknown native members remain byte ranges; known inheritance prefixes are
 * represented by their verified layout types. This keeps the TXD index at its
 * real CBaseModelInfo offset without treating the whole structure as opaque.
 */
struct VehicleModelInfoLayout
{
    ClumpModelInfoLayout clump_model_info;
	void* plate_material;
	char plate_text[9];
	std::uint8_t plate_type;
	char game_name[8];
	std::uint8_t vehicle_type_alignment[2];
	std::int32_t vehicle_type;
	float front_wheel_size;
	float rear_wheel_size;
	std::int16_t wheel_model_index;
	std::uint16_t handling_id;
	std::uint8_t number_of_doors;
	std::uint8_t vehicle_class;
	std::uint8_t vehicle_flags;
	std::uint8_t wheel_upgrade_class;
	std::uint8_t times_used;
	std::uint8_t field_51;
	std::uint16_t vehicle_frequency;
	std::uint32_t component_rules;
	float bike_steering_angle;
	VehicleStructureLayout* vehicle_structure;
	std::uint8_t unknown_60[464];
	void* dirt_materials[32];
	std::uint8_t primary_colours[8];
	std::uint8_t secondary_colours[8];
	std::uint8_t tertiary_colours[8];
	std::uint8_t quaternary_colours[8];
	std::uint8_t number_of_colour_variations;
	std::uint8_t last_colour_variation;
	std::uint8_t current_primary_colour;
	std::uint8_t current_secondary_colour;
	std::uint8_t current_tertiary_colour;
	std::uint8_t current_quaternary_colour;
	std::int16_t upgrades[18];
	std::int16_t remap_txd_slots[4];
	union
	{
		void* animation_block;
		char* animation_block_file_name;
		std::uint32_t animation_block_index;
	};
};

static_assert(sizeof(void*) == 4, "GTA SA 1.0 US is a 32-bit process");
static_assert(sizeof(StreamingInfoLayout) == 0x14);
static_assert(offsetof(StreamingInfoLayout, next_index) == 0x00);
static_assert(offsetof(StreamingInfoLayout, previous_index) == 0x02);
static_assert(offsetof(StreamingInfoLayout, next_index_on_cd) == 0x04);
static_assert(offsetof(StreamingInfoLayout, flags) == 0x06);
static_assert(offsetof(StreamingInfoLayout, image_id) == 0x07);
static_assert(offsetof(StreamingInfoLayout, cd_offset) == 0x08);
static_assert(offsetof(StreamingInfoLayout, cd_size) == 0x0C);
static_assert(offsetof(StreamingInfoLayout, load_state) == 0x10);
static_assert(sizeof(TxdDefLayout) == 0x0C);
static_assert(offsetof(TxdDefLayout, rw_dictionary) == 0x00);
static_assert(offsetof(TxdDefLayout, reference_count) == 0x04);
static_assert(offsetof(TxdDefLayout, parent_index) == 0x06);
static_assert(offsetof(TxdDefLayout, name_hash) == 0x08);
static_assert(FirstTxdResourceId == 20000);
static_assert(TxdPoolCapacity == 5000);
static_assert(LastTxdResourceId == 24999);
static_assert(txdResourceIdForPoolSlot(0) == 20000);
static_assert(txdResourceIdForPoolSlot(4999) == 24999);
static_assert(txdResourceIdForPoolSlot(-1) == -1);
static_assert(txdResourceIdForPoolSlot(5000) == -1);
static_assert(sizeof(BaseModelInfoLayout) == 0x20);
static_assert(sizeof(ClumpModelInfoLayout) == 0x24);
static_assert(offsetof(ClumpModelInfoLayout, base) == 0x00);
static_assert(offsetof(ClumpModelInfoLayout, animation_file_or_index) == 0x20);
static_assert(offsetof(BaseModelInfoLayout, reference_count) == 0x08);
static_assert(offsetof(BaseModelInfoLayout, texture_dictionary_index) == 0x0A);
static_assert(offsetof(BaseModelInfoLayout, alpha) == 0x0C);
static_assert(offsetof(BaseModelInfoLayout, two_d_effect_count) == 0x0D);
static_assert(offsetof(BaseModelInfoLayout, two_d_effect_index) == 0x0E);
static_assert(offsetof(BaseModelInfoLayout, object_info_index) == 0x10);
static_assert(offsetof(BaseModelInfoLayout, flags) == 0x12);
static_assert(offsetof(BaseModelInfoLayout, collision_model) == 0x14);
static_assert(offsetof(BaseModelInfoLayout, draw_distance) == 0x18);
static_assert(offsetof(BaseModelInfoLayout, renderware_object) == 0x1C);
static_assert(sizeof(VehicleUpgradePositionLayout) == 0x20);
static_assert(sizeof(VehicleStructureLayout) == 0x314);
static_assert(offsetof(VehicleStructureLayout, upgrade_positions) == 0x0B4);
static_assert(offsetof(VehicleStructureLayout, extras) == 0x2F4);
static_assert(offsetof(VehicleStructureLayout, damageable_component_mask) == 0x310);

static_assert(sizeof(VehicleModelInfoLayout) == 0x308);
static_assert(offsetof(VehicleModelInfoLayout, clump_model_info) == 0x00);
static_assert(
    offsetof(VehicleModelInfoLayout, clump_model_info) +
        offsetof(ClumpModelInfoLayout, base) +
        offsetof(BaseModelInfoLayout, texture_dictionary_index) == 0x0A);
static_assert(offsetof(VehicleModelInfoLayout, vehicle_type) == 0x3C);
static_assert(offsetof(VehicleModelInfoLayout, vehicle_structure) == 0x5C);
static_assert(offsetof(VehicleModelInfoLayout, dirt_materials) == 0x230);
static_assert(offsetof(VehicleModelInfoLayout, primary_colours) == 0x2B0);
static_assert(offsetof(VehicleModelInfoLayout, upgrades) == 0x2D6);
static_assert(offsetof(VehicleModelInfoLayout, remap_txd_slots) == 0x2FA);
static_assert(offsetof(VehicleModelInfoLayout, animation_block) == 0x304);
} // namespace custom_vehicle::gta_sa
