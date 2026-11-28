#include <gta_sa_vehicle_model_info_layout.hpp>

// This translation unit makes CMake compile the GTA x86 model-info and
// streaming-table assertions on the Win32 ASI target.
static_assert(custom_vehicle::gta_sa::ModelInfoPointerTableAddress == 0x00A9B0C8);
static_assert(custom_vehicle::gta_sa::ModelInfoPointerTableCount == 20000);
static_assert(custom_vehicle::gta_sa::StreamingInfoTableAddress == 0x008E4CC0);
static_assert(custom_vehicle::gta_sa::StreamingInfoTableCount == 26316);
static_assert(custom_vehicle::gta_sa::TxdPoolCapacity == 5000);
static_assert(sizeof(custom_vehicle::gta_sa::ClumpModelInfoLayout) == 0x24);
static_assert(offsetof(custom_vehicle::gta_sa::ClumpModelInfoLayout, animation_file_or_index) == 0x20);
static_assert(
    offsetof(custom_vehicle::gta_sa::VehicleModelInfoLayout, clump_model_info) +
        offsetof(custom_vehicle::gta_sa::ClumpModelInfoLayout, base) +
        offsetof(custom_vehicle::gta_sa::BaseModelInfoLayout, texture_dictionary_index) == 0x0A);
static_assert(custom_vehicle::gta_sa::FirstTxdResourceId == 20000);
static_assert(custom_vehicle::gta_sa::LastTxdResourceId == 24999);
static_assert(sizeof(custom_vehicle::gta_sa::TxdDefLayout) == 0x0C);
static_assert(offsetof(custom_vehicle::gta_sa::TxdDefLayout, reference_count) == 0x04);
static_assert(offsetof(custom_vehicle::gta_sa::TxdDefLayout, parent_index) == 0x06);
static_assert(offsetof(custom_vehicle::gta_sa::TxdDefLayout, name_hash) == 0x08);

namespace
{
using custom_vehicle::gta_sa::StreamingInfoLayout;
using custom_vehicle::gta_sa::makeIndependentStreamingInfo;
using custom_vehicle::gta_sa::makeStreamingInfoForArchive;

constexpr auto StandaloneArchiveInfo =
	makeStreamingInfoForArchive(6, 0x12345678, 0x2345);
constexpr auto ChainedArchiveInfo =
	makeStreamingInfoForArchive(6, 0x12345678, 0x2345, 30002);

static_assert(custom_vehicle::gta_sa::StreamingSectorSizeInBytes == 2048);
static_assert(StandaloneArchiveInfo.next_index == -1);
static_assert(StandaloneArchiveInfo.previous_index == -1);
static_assert(StandaloneArchiveInfo.next_index_on_cd == -1);
static_assert(StandaloneArchiveInfo.flags == 0);
static_assert(StandaloneArchiveInfo.image_id == 6);
static_assert(StandaloneArchiveInfo.cd_offset == 0x12345678);
static_assert(StandaloneArchiveInfo.cd_size == 0x2345);
static_assert(StandaloneArchiveInfo.load_state == 0);
static_assert(ChainedArchiveInfo.next_index_on_cd == 30002);

constexpr StreamingInfoLayout BaseStreamingInfo{
	7, 8, 9, 0x3F, 4, 0x12345678, 0x2345, 1, { 0xAA, 0xBB, 0xCC }
};
constexpr StreamingInfoLayout IndependentStreamingInfo =
	makeIndependentStreamingInfo(BaseStreamingInfo);

static_assert(IndependentStreamingInfo.next_index == -1);
static_assert(IndependentStreamingInfo.previous_index == -1);
static_assert(IndependentStreamingInfo.next_index_on_cd == -1);
static_assert(IndependentStreamingInfo.flags == 0);
static_assert(IndependentStreamingInfo.image_id == BaseStreamingInfo.image_id);
static_assert(IndependentStreamingInfo.cd_offset == BaseStreamingInfo.cd_offset);
static_assert(IndependentStreamingInfo.cd_size == BaseStreamingInfo.cd_size);
static_assert(IndependentStreamingInfo.load_state == 0);
static_assert(IndependentStreamingInfo.alignment_padding[0] == 0);
static_assert(IndependentStreamingInfo.alignment_padding[1] == 0);
static_assert(IndependentStreamingInfo.alignment_padding[2] == 0);
} // namespace

void custom_vehicle_native_vehicle_layout_compile_check() {}
