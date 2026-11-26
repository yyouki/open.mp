cmake_minimum_required(VERSION 3.16)

project(custom_vehicle_protocol LANGUAGES CXX)

add_library(custom_vehicle_protocol STATIC
	src/protocol.cpp
	src/session.cpp
	src/network_adapter.cpp
)

target_include_directories(custom_vehicle_protocol
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(custom_vehicle_protocol PUBLIC cxx_std_17)

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
