cmake_minimum_required(VERSION 3.16)

project(custom_vehicle_protocol LANGUAGES C CXX)

add_library(custom_vehicle_protocol STATIC
	src/protocol.cpp
	src/session.cpp
	src/network_adapter.cpp
	src/asset_cache.cpp
	src/model_slot_registry.cpp
	src/bridge.cpp
)

target_include_directories(custom_vehicle_protocol
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(custom_vehicle_protocol PUBLIC cxx_std_17)

# Compile the public bridge header as C to catch accidental C++ ABI leakage.
add_library(custom_vehicle_bridge_c_header_check OBJECT
	tests/bridge_header_c_test.c
)
target_include_directories(custom_vehicle_bridge_c_header_check
	PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}/include
		${CMAKE_CURRENT_SOURCE_DIR}/../Windows/include
)
target_compile_features(custom_vehicle_bridge_c_header_check PRIVATE c_std_99)

if(MSVC)
	target_compile_options(custom_vehicle_bridge_c_header_check PRIVATE /W4 /WX)
else()
	target_compile_options(custom_vehicle_bridge_c_header_check PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

if(MSVC)
	target_compile_options(custom_vehicle_protocol PRIVATE /W4 /WX)
else()
	target_compile_options(custom_vehicle_protocol PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

include(CTest)

if(BUILD_TESTING)
	add_executable(custom_vehicle_protocol_tests
		tests/protocol_tests.cpp
	)
	target_link_libraries(custom_vehicle_protocol_tests PRIVATE custom_vehicle_protocol)
	target_compile_features(custom_vehicle_protocol_tests PRIVATE cxx_std_17)

	if(MSVC)
		target_compile_options(custom_vehicle_protocol_tests PRIVATE /W4 /WX)
	else()
		target_compile_options(custom_vehicle_protocol_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
	endif()

	add_test(NAME custom_vehicle_protocol_tests COMMAND custom_vehicle_protocol_tests)
endif()

if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
	# Compile-time GTA SA 1.0 US native vehicle layout evidence for the x86 ASI.
	# This target performs no native calls and links no game addresses.
	add_library(custom_vehicle_native_layout_check OBJECT
		${CMAKE_CURRENT_SOURCE_DIR}/../Windows/tests/native_vehicle_model_info_layout_check.cpp
	)
	target_include_directories(custom_vehicle_native_layout_check
		PRIVATE
			${CMAKE_CURRENT_SOURCE_DIR}/../Windows/include
	)
	target_compile_features(custom_vehicle_native_layout_check PRIVATE cxx_std_17)
	if(MSVC)
		target_compile_options(custom_vehicle_native_layout_check PRIVATE /W4 /WX)
	else()
		target_compile_options(custom_vehicle_native_layout_check PRIVATE -Wall -Wextra -Wpedantic -Werror)
	endif()
endif()

if(ANDROID)
	# The Android JNI host links the same portable protocol/session/cache implementation.
	set_target_properties(custom_vehicle_protocol PROPERTIES POSITION_INDEPENDENT_CODE ON)

	add_library(custom_vehicle_android SHARED
		${CMAKE_CURRENT_SOURCE_DIR}/../Android/src/jni_bridge.cpp
	)
	target_link_libraries(custom_vehicle_android PRIVATE custom_vehicle_protocol)
	target_compile_features(custom_vehicle_android PRIVATE cxx_std_17)
	target_compile_options(custom_vehicle_android PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
