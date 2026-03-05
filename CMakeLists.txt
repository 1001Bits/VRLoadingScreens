cmake_minimum_required(VERSION 3.23)

set(NAME "VRLoadingScreens")
set(VERSION 1.0.0)

message(">>> Start build for '${NAME}' v${VERSION}")

set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SOURCE_DIR "${ROOT_DIR}/src")
set(BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}")

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR ">> In-source builds not allowed.")
endif()

# Setup vcpkg BEFORE project()
macro(set_from_environment VARIABLE)
  if(NOT DEFINED ${VARIABLE} AND DEFINED ENV{${VARIABLE}})
    set(${VARIABLE} $ENV{${VARIABLE}})
  endif()
endmacro()

set_from_environment(VCPKG_ROOT)
if(DEFINED VCPKG_ROOT)
  set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
  set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>" CACHE STRING "")
else()
  message(FATAL_ERROR ">> Variable VCPKG_ROOT is not set!")
endif()

# Locate F4VRCommonFramework
set(F4VR_COMMON_FRAMEWORK_PATH "C:/Development/higgs-master/frik/external/F4VRCommonFramework" CACHE STRING "")
if(NOT EXISTS "${F4VR_COMMON_FRAMEWORK_PATH}/src/ModBase.h")
  message(FATAL_ERROR ">> F4VRCommonFramework not found at ${F4VR_COMMON_FRAMEWORK_PATH}")
endif()
message(">>> Using F4VRCommonFramework at: ${F4VR_COMMON_FRAMEWORK_PATH}")

project(${NAME} VERSION ${VERSION} LANGUAGES CXX)
message(">>> Building project '${PROJECT_NAME}' v${PROJECT_VERSION}")

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_DEBUG OFF)

add_subdirectory(${F4VR_COMMON_FRAMEWORK_PATH} "F4VRCommonFramework" EXCLUDE_FROM_ALL)

find_path(SIMPLEINI_INCLUDE_DIRS "SimpleIni.h")

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    "${SOURCE_DIR}/*.cpp"
    "${SOURCE_DIR}/*.h"
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_include_directories(${PROJECT_NAME} PRIVATE
    "${SOURCE_DIR}"
    ${SIMPLEINI_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    F4VRCommonFramework
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    F4SEVR_SUPPORT
)

# Post build
add_custom_command(
    TARGET ${PROJECT_NAME}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ROOT_DIR}/package/Data/F4SE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${ROOT_DIR}/package/Data/MCM/Config/VRLoadingScreens"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${PROJECT_NAME}> "${ROOT_DIR}/package/Data/F4SE/Plugins/"
    COMMAND ${CMAKE_COMMAND} -E copy "${ROOT_DIR}/package/Data/MCM/Config/VRLoadingScreens/config.json" "${ROOT_DIR}/package/Data/MCM/Config/VRLoadingScreens/"
    COMMAND ${CMAKE_COMMAND} -E copy "${ROOT_DIR}/package/Data/MCM/Config/VRLoadingScreens/settings.ini" "${ROOT_DIR}/package/Data/MCM/Config/VRLoadingScreens/"
    COMMENT "Packaging ${PROJECT_NAME}..."
)

message(">>> ${PROJECT_NAME} configured successfully")
