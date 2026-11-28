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

namespace
{
constexpr auto makeVehicleModelInfoFixture() noexcept
{
    custom_vehicle::gta_sa::VehicleModelInfoLayout model{};
    model.clump_model_info.base.vtable = custom_vehicle::gta_sa::VehicleModelInfoVTableAddress;
    model.clump_model_info.base.reference_count = 23;
    model.clump_model_info.base.texture_dictionary_index = 17;
    model.clump_model_info.base.two_d_effect_index = 42;
    model.clump_model_info.base.object_info_index = 43;
    model.clump_model_info.base.renderware_object = 0x12345678;
    model.vehicle_type = 4;
    model.front_wheel_size = 0.75F;
    return model;
}
constexpr auto BaseVehicleModelInfo = makeVehicleModelInfoFixture();
constexpr auto makePreparedVehicleModelInfo() noexcept
{
    custom_vehicle::gta_sa::VehicleModelInfoLayout output{};
    const bool prepared = custom_vehicle::gta_sa::prepareVehicleModelInfoClone(BaseVehicleModelInfo, 123, output);
    return prepared ? output : custom_vehicle::gta_sa::VehicleModelInfoLayout{};
}
constexpr auto PreparedVehicleModelInfo = makePreparedVehicleModelInfo();
static_assert(PreparedVehicleModelInfo.clump_model_info.base.vtable == custom_vehicle::gta_sa::VehicleModelInfoVTableAddress);
static_assert(PreparedVehicleModelInfo.clump_model_info.base.reference_count == 0);
static_assert(PreparedVehicleModelInfo.clump_model_info.base.texture_dictionary_index == 123);
static_assert(PreparedVehicleModelInfo.clump_model_info.base.two_d_effect_index == -1);
static_assert(PreparedVehicleModelInfo.clump_model_info.base.object_info_index == -1);
static_assert(PreparedVehicleModelInfo.clump_model_info.base.renderware_object == 0);
static_assert(PreparedVehicleModelInfo.vehicle_structure == nullptr);
static_assert(PreparedVehicleModelInfo.vehicle_type == BaseVehicleModelInfo.vehicle_type);
static_assert(PreparedVehicleModelInfo.front_wheel_size == BaseVehicleModelInfo.front_wheel_size);
constexpr bool rejectsInvalidTxdSlot() noexcept
{
    custom_vehicle::gta_sa::VehicleModelInfoLayout output{};
    output.vehicle_type = 99;
    return !custom_vehicle::gta_sa::prepareVehicleModelInfoClone(BaseVehicleModelInfo, 5000, output) && output.vehicle_type == 99;
}
constexpr bool rejectsWrongVtable() noexcept
{
    auto invalid_base = BaseVehicleModelInfo;
    invalid_base.clump_model_info.base.vtable = 0x12345678;
    custom_vehicle::gta_sa::VehicleModelInfoLayout output{};
    output.vehicle_type = 77;
    return !custom_vehicle::gta_sa::prepareVehicleModelInfoClone(invalid_base, 123, output) && output.vehicle_type == 77;
}
static_assert(rejectsInvalidTxdSlot());
static_assert(rejectsWrongVtable());
} // namespace

void custom_vehicle_native_vehicle_layout_compile_check() {}
