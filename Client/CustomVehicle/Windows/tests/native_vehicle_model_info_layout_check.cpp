#include <gta_sa_vehicle_model_info_layout.hpp>

// This translation unit makes CMake compile the GTA x86 model-info and
// streaming-table assertions on the Win32 ASI target.
static_assert(custom_vehicle::gta_sa::ModelInfoPointerTableAddress == 0x00A9B0C8);
static_assert(custom_vehicle::gta_sa::ModelInfoPointerTableCount == 20000);
static_assert(custom_vehicle::gta_sa::StreamingInfoTableAddress == 0x008E4CC0);
static_assert(custom_vehicle::gta_sa::StreamingInfoTableCount == 26316);

void custom_vehicle_native_vehicle_layout_compile_check() {}
