cmake_minimum_required(VERSION 3.10)

set(BUILD_NAME HyperionMCP)

project(${BUILD_NAME} CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

message(STATUS "Building ${BUILD_NAME} plugin")

# ---------------------------------------------------------------------------
# Hyperion engine SDK location
#
# HYP_ENGINE_ROOT must point at a hyperion-engine checkout that has been built,
# so that Binaries/<Platform>/<Config>/hyperion.lib (import library for
# hyperion.dll) exists. The plugin MUST be built with the same engine version
# and toolchain as the hyperion.dll it will be loaded by.
# ---------------------------------------------------------------------------
if(NOT DEFINED HYP_ENGINE_ROOT)
  message(FATAL_ERROR "HYP_ENGINE_ROOT is not defined. Pass -DHYP_ENGINE_ROOT=<path to hyperion-engine checkout>")
endif()

get_filename_component(HYP_ENGINE_ROOT "${HYP_ENGINE_ROOT}" ABSOLUTE)

if(NOT DEFINED HYP_PLATFORM_NAME)
  if(ANDROID)
    set(HYP_PLATFORM_NAME "Android")
  elseif(WIN32)
    set(HYP_PLATFORM_NAME "Windows")
  elseif(IOS)
    set(HYP_PLATFORM_NAME "IOS")
  elseif(APPLE)
    set(HYP_PLATFORM_NAME "Mac")
  elseif(UNIX)
    set(HYP_PLATFORM_NAME "Linux")
  else()
    message(FATAL_ERROR "Unsupported platform")
  endif()
endif()

if(NOT DEFINED HYP_BUILD_TYPE)
  set(HYP_BUILD_TYPE "Release" CACHE STRING "Engine build config to link against")
endif()

set(HYP_ENGINE_SOURCE_DIR "${HYP_ENGINE_ROOT}/Source")
# Runtime binaries (hyperion.dll et al) go to <repo>/Binaries, import libraries to the
# build tree at <repo>/Build (CMAKE_BINARY_DIR of the engine's Ninja build).
set(HYP_ENGINE_BIN_DIR "${HYP_ENGINE_ROOT}/Binaries/${HYP_PLATFORM_NAME}/${HYP_BUILD_TYPE}")
set(HYP_ENGINE_LIB_DIR "${HYP_ENGINE_ROOT}/Build/${HYP_PLATFORM_NAME}/${HYP_BUILD_TYPE}")

message(STATUS "HYP_ENGINE_ROOT: ${HYP_ENGINE_ROOT}")
message(STATUS "HYP_ENGINE_BIN_DIR: ${HYP_ENGINE_BIN_DIR}")
message(STATUS "HYP_ENGINE_LIB_DIR: ${HYP_ENGINE_LIB_DIR}")

if(WIN32)
  if(NOT EXISTS "${HYP_ENGINE_LIB_DIR}/hyperion.lib")
    message(WARNING "hyperion.lib not found in ${HYP_ENGINE_LIB_DIR} - build the engine first, or adjust HYP_BUILD_TYPE")
  endif()
endif()

# Engine version - keep in sync with the engine checkout this links against
# (Source/CMakeLists.txt). Used for compile-time version macros.
set(HYP_VERSION_MAJOR 1 CACHE STRING "")
set(HYP_VERSION_MINOR 4 CACHE STRING "")
set(HYP_VERSION_PATCH 0 CACHE STRING "")

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
file(GLOB_RECURSE PLUGIN_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
file(GLOB_RECURSE PLUGIN_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.hpp")
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/src PREFIX "Source Code" FILES ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

add_library(${BUILD_NAME} SHARED ${PLUGIN_SOURCES} ${PLUGIN_HEADERS})

# ---------------------------------------------------------------------------
# Compile definitions - mirror a non-shipping editor build of the engine
# (Source/CMakeLists.txt). These MUST match the engine build or headers will
# produce different layouts than hyperion.dll was built with.
# ---------------------------------------------------------------------------
target_compile_definitions(${BUILD_NAME} PRIVATE
  HYP_BUILD_PLUGIN=1
  HYPERION_ENGINE=1
  HYP_TYPE_ID_COMPILE_TIME=1
  HYP_EDITOR=1
  HYP_ENABLE_SHADER_RELOAD=1
  HYP_DOTNET=1
  HYP_DOTNET_ONLY_FOR_EDITOR=1
  HYP_STRATA=1
  HYP_STRATA_JIT=1
  HYP_TESTS=1
  HYP_VERSION_MAJOR=${HYP_VERSION_MAJOR}
  HYP_VERSION_MINOR=${HYP_VERSION_MINOR}
  HYP_VERSION_PATCH=${HYP_VERSION_PATCH}
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_definitions(${BUILD_NAME} PRIVATE HYP_DEBUG_MODE=1)
endif()

if(NOT HYP_SHIPPING)
  target_compile_definitions(${BUILD_NAME} PRIVATE HYP_ENABLE_CORE_ASSERTIONS=1)
endif()

# Compiler + platform defines (mirrors Source/CMakeLists.txt)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  target_compile_definitions(${BUILD_NAME} PRIVATE HYP_MSVC=1)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  target_compile_definitions(${BUILD_NAME} PRIVATE HYP_CLANG=1 HYP_CLANG_OR_GCC=1)
  if(HYP_PLATFORM_NAME STREQUAL "Windows")
    target_compile_definitions(${BUILD_NAME} PRIVATE HYP_CLANG_WINDOWS=1)
  endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  target_compile_definitions(${BUILD_NAME} PRIVATE HYP_GCC=1 HYP_CLANG_OR_GCC=1)
endif()

# Platform defines
if(HYP_PLATFORM_NAME STREQUAL "Windows")
  target_compile_definitions(${BUILD_NAME} PRIVATE
    HYP_WINDOWS=1
    HYP_PLATFORM_NAME_STR="Windows"
  )
elseif(HYP_PLATFORM_NAME STREQUAL "Mac")
  target_compile_definitions(${BUILD_NAME} PRIVATE
    HYP_APPLE=1
    HYP_MACOS=1
    HYP_UNIX=1
    HYP_PLATFORM_NAME_STR="Mac"
  )
elseif(HYP_PLATFORM_NAME STREQUAL "IOS")
  target_compile_definitions(${BUILD_NAME} PRIVATE
    HYP_IOS=1
    HYP_APPLE=1
    HYP_UNIX=1
    HYP_PLATFORM_NAME_STR="IOS"
  )
elseif(HYP_PLATFORM_NAME STREQUAL "Android")
  target_compile_definitions(${BUILD_NAME} PRIVATE
    HYP_ANDROID=1
    HYP_UNIX=1
    HYP_PLATFORM_NAME_STR="Android"
  )
elseif(HYP_PLATFORM_NAME STREQUAL "Linux")
  target_compile_definitions(${BUILD_NAME} PRIVATE
    HYP_LINUX=1
    HYP_UNIX=1
    HYP_PLATFORM_NAME_STR="Linux"
  )
endif()

# Rendering backend - default matches the engine (Vulkan unless the engine was
# configured with ENABLE_EXPERIMENTAL_DX12_RENDERING_BACKEND)
set(HYP_RENDERING_BACKEND "Vulkan" CACHE STRING "Rendering backend the engine was built with")
if(HYP_RENDERING_BACKEND STREQUAL "Vulkan")
  target_compile_definitions(${BUILD_NAME} PRIVATE HYP_VULKAN=1)
elseif(HYP_RENDERING_BACKEND STREQUAL "DX12")
  target_compile_definitions(${BUILD_NAME} PRIVATE HYP_DX12=1)
else()
  message(FATAL_ERROR "Unsupported HYP_RENDERING_BACKEND: ${HYP_RENDERING_BACKEND}")
endif()

# Physics backend - HYP_JOLT if the engine's Jolt submodule is checked out
if(EXISTS "${HYP_ENGINE_ROOT}/External/Submodules/JoltPhysics/Jolt/Jolt.h")
  target_compile_definitions(${BUILD_NAME} PRIVATE HYP_JOLT=1)
  target_include_directories(${BUILD_NAME} SYSTEM PRIVATE "${HYP_ENGINE_ROOT}/External/Submodules/JoltPhysics")
endif()

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------
if(EXISTS "${HYP_ENGINE_ROOT}/External/ThirdParty/Source")
  target_include_directories(${BUILD_NAME} SYSTEM PRIVATE "${HYP_ENGINE_ROOT}/External/ThirdParty/Source")
endif()

if(HYP_RENDERING_BACKEND STREQUAL "Vulkan" AND DEFINED ENV{VULKAN_SDK})
  target_include_directories(${BUILD_NAME} SYSTEM PRIVATE "$ENV{VULKAN_SDK}/include")
endif()

target_include_directories(${BUILD_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${HYP_ENGINE_SOURCE_DIR}"
  "${HYP_ENGINE_SOURCE_DIR}/Generated"
  "${HYP_ENGINE_SOURCE_DIR}/Engine"
)

# ---------------------------------------------------------------------------
# Compiler flags - mirror the engine's flags so the plugin ABI matches
# ---------------------------------------------------------------------------
if(MSVC)
  # Mirror the engine's flag sets (Source/CMakeLists.txt): the MSVC branch adds
  # /arch:AVX2 & friends, the clang-cl (HYP_CLANG_WINDOWS) branch does NOT - and
  # __AVX2__ enables an F16C intrinsic path that conflicts with clang's constexpr
  # intrinsics, so AVX2 must stay MSVC-only here too.
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(${BUILD_NAME} PRIVATE
      -O2
      -g
      /MD             # dynamic CRT, must match the engine
      /bigobj
      /GR-            # no RTTI
      /EHs-c-         # no exceptions
      -Xclang -fno-recovery-ast
    )
  else()
    target_compile_options(${BUILD_NAME} PRIVATE
      /Zc:preprocessor
      /Zc:__cplusplus
      /O2
      /Oi
      /MD             # dynamic CRT, must match the engine
      /Z7
      /bigobj
      /MP
      /arch:AVX2
      /fp:fast
      /GR-            # no RTTI
      /EHs-c-         # no exceptions
      /wd4251
      /wd4275
    )
  endif()

  target_compile_definitions(${BUILD_NAME} PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    _HAS_EXCEPTIONS=0
  )
else()
  target_compile_options(${BUILD_NAME} PRIVATE
    -fno-exceptions
    -fno-rtti
  )

  if(HYP_PLATFORM_NAME STREQUAL "Windows")
    target_compile_definitions(${BUILD_NAME} PRIVATE NOMINMAX)
  endif()

  if(APPLE)
    set_source_files_properties(${PLUGIN_SOURCES} PROPERTIES COMPILE_OPTIONS "-x objective-c++")
  endif()
endif()

# ---------------------------------------------------------------------------
# Link against the engine's import libraries
#
# hyperion.dll is the main engine library, but some modules are built as their
# own shared libraries (hyperion-core.dll, hyperion-net.dll, hyperion-script.dll);
# code compiled against their headers must link their import libs directly.
# ---------------------------------------------------------------------------
if(WIN32)
  target_link_libraries(${BUILD_NAME} PRIVATE "${HYP_ENGINE_LIB_DIR}/hyperion.lib")

  foreach(HYP_SHARED_MODULE_LIB "${HYP_ENGINE_LIB_DIR}/Core/hyperion-core.lib"
                                "${HYP_ENGINE_LIB_DIR}/Net/hyperion-net.lib"
                                "${HYP_ENGINE_LIB_DIR}/Lang/hyperion-script.lib")
    if(EXISTS "${HYP_SHARED_MODULE_LIB}")
      target_link_libraries(${BUILD_NAME} PRIVATE "${HYP_SHARED_MODULE_LIB}")
    endif()
  endforeach()

  # MCP bridge server uses WinSock
  target_link_libraries(${BUILD_NAME} PRIVATE ws2_32)
elseif(APPLE)
  target_link_libraries(${BUILD_NAME} PRIVATE "${HYP_ENGINE_BIN_DIR}/libhyperion.dylib")
else()
  target_link_libraries(${BUILD_NAME} PRIVATE "${HYP_ENGINE_BIN_DIR}/libhyperion.so")
endif()

# ---------------------------------------------------------------------------
# Output: Binaries/<Platform>/<Config>/Plugins/MCPBridge/ next to the engine,
# so the PluginManager discovers it on next editor launch.
# ---------------------------------------------------------------------------
if(NOT DEFINED HYP_PLUGIN_OUTPUT_DIR)
  set(HYP_PLUGIN_OUTPUT_DIR "${HYP_ENGINE_BIN_DIR}/Plugins/MCPBridge")
endif()

get_filename_component(HYP_PLUGIN_OUTPUT_DIR "${HYP_PLUGIN_OUTPUT_DIR}" ABSOLUTE)

message(STATUS "Plugin output directory: ${HYP_PLUGIN_OUTPUT_DIR}")

set_target_properties(${BUILD_NAME} PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${HYP_PLUGIN_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${HYP_PLUGIN_OUTPUT_DIR}"
  RUNTIME_OUTPUT_DIRECTORY_DEBUG "${HYP_PLUGIN_OUTPUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${HYP_PLUGIN_OUTPUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY_RELEASE "${HYP_PLUGIN_OUTPUT_DIR}"
  LIBRARY_OUTPUT_DIRECTORY_DEBUG "${HYP_PLUGIN_OUTPUT_DIR}"
  PREFIX ""
)

add_custom_command(TARGET ${BUILD_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json"
      "${HYP_PLUGIN_OUTPUT_DIR}/plugin.json"
  COMMENT "Copying plugin.json to ${HYP_PLUGIN_OUTPUT_DIR}"
)
