cmake_minimum_required(VERSION 3.17)

set(LIBNAME "barista_mcp")
set(GODOT_PROJECT_DIR "project" CACHE STRING "Godot project directory")
set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Godot API version")
set(GODOTCPP_BUILD_PROFILE "${CMAKE_CURRENT_LIST_DIR}/build_profile.json" CACHE FILEPATH "godot-cpp build profile")

find_package(Python3 3.8 REQUIRED)
find_program(GIT git REQUIRED)

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/godot-cpp/src")
    message(FATAL_ERROR "godot-cpp is unavailable; initialize Git submodules first")
endif()

add_subdirectory(godot-cpp SYSTEM)

project(
    BaristaMCP
    VERSION 0.1.0
    DESCRIPTION "A stock-compatible MCP server for the Godot editor"
    LANGUAGES CXX
)

add_library(${LIBNAME} SHARED)
file(GLOB BARISTA_MCP_SOURCES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.cpp")
file(GLOB BARISTA_MCP_HEADERS CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/src/*.h")
target_sources(${LIBNAME} PRIVATE ${BARISTA_MCP_SOURCES} ${BARISTA_MCP_HEADERS})
target_link_libraries(${LIBNAME} PRIVATE godot-cpp)
target_compile_features(${LIBNAME} PRIVATE cxx_std_17)

get_target_property(GODOTCPP_SUFFIX godot::cpp GODOTCPP_SUFFIX)
get_target_property(GODOTCPP_PLATFORM godot::cpp GODOTCPP_PLATFORM)
set_target_properties(${LIBNAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${PROJECT_SOURCE_DIR}/bin/${GODOTCPP_PLATFORM}>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${PROJECT_SOURCE_DIR}/bin/${GODOTCPP_PLATFORM}>"
    OUTPUT_NAME "${LIBNAME}${GODOTCPP_SUFFIX}"
)

set(PROJECT_BINARY_DIR_PATH "${PROJECT_SOURCE_DIR}/${GODOT_PROJECT_DIR}/bin/${GODOTCPP_PLATFORM}")
add_custom_command(TARGET ${LIBNAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROJECT_BINARY_DIR_PATH}"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${LIBNAME}>" "${PROJECT_BINARY_DIR_PATH}/$<TARGET_FILE_NAME:${LIBNAME}>"
)
