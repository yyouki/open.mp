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
 * - gta-reversed/gta-reversed, source/game_sa/Models/VehicleModelInfo.h
 *   commit 85bce0be4bd3229eb5ef445d3a7ac2e611a994f4
 * - multitheftauto/mtasa-blue, Client/game_sa/CModelInfoSA.h
 *   commit 002d0274c63740c512ae06daab67e0b6e770745a
 */

#if !defined(_WIN32) || !(defined(_M_IX86) || defined(__i386__))
#error "GTA SA native vehicle model-info layouts are valid only for Windows x86"
#endif

namespace custom_vehicle::gta_sa
{
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
 * The 0x24-byte prefix is CClumpModelInfo (0x20-byte CBaseModelInfo plus
 * its animation-file field). Explicit byte ranges preserve unknown native
 * members without inventing semantics.
 */
struct VehicleModelInfoLayout
{
	std::uint8_t clump_model_info_prefix[0x24];
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
	std::uint8_t trailing_padding[2];
};

static_assert(sizeof(void*) == 4, "GTA SA 1.0 US is a 32-bit process");
static_assert(sizeof(VehicleUpgradePositionLayout) == 0x20);
static_assert(sizeof(VehicleStructureLayout) == 0x314);
static_assert(offsetof(VehicleStructureLayout, upgrade_positions) == 0x0B4);
static_assert(offsetof(VehicleStructureLayout, extras) == 0x2F4);
static_assert(offsetof(VehicleStructureLayout, damageable_component_mask) == 0x310);

static_assert(sizeof(VehicleModelInfoLayout) == 0x308);
static_assert(offsetof(VehicleModelInfoLayout, vehicle_type) == 0x3C);
static_assert(offsetof(VehicleModelInfoLayout, vehicle_structure) == 0x5C);
static_assert(offsetof(VehicleModelInfoLayout, dirt_materials) == 0x230);
static_assert(offsetof(VehicleModelInfoLayout, primary_colours) == 0x2B0);
static_assert(offsetof(VehicleModelInfoLayout, upgrades) == 0x2D6);
static_assert(offsetof(VehicleModelInfoLayout, remap_txd_slots) == 0x2FA);
static_assert(offsetof(VehicleModelInfoLayout, animation_block) == 0x302);
} // namespace custom_vehicle::gta_sa
