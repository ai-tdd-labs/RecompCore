cmake_minimum_required(VERSION 3.16)
project(recompcore_module C)

# Per-game native module for RecompCore: generated chunks + DolRecomp CPU
# runtime + ABI export glue, no other host dependencies.
#
# Configure with:
#   cmake -B build -DGAME_ID=G4QE01 \
#         -DGENERATED_DIR=/path/to/generated \
#         -DGXRUNTIME_DIR=/path/to/GXRuntime

set(CMAKE_C_STANDARD 11)
option(MODERNGEKKO_ENABLE_LTO "Use link-time optimization for release modules" ON)
option(MODERNGEKKO_FAST_BUILD "Prefer fast compilation over generated-code optimization" OFF)
set(MODERNGEKKO_OPTIMIZATION_LEVEL "" CACHE STRING
    "Generated-code optimization level (0, 1, or 2)")
if(MODERNGEKKO_OPTIMIZATION_LEVEL STREQUAL "")
  if(MODERNGEKKO_FAST_BUILD)
    set(MODERNGEKKO_OPTIMIZATION_LEVEL 0)
  else()
    set(MODERNGEKKO_OPTIMIZATION_LEVEL 1)
  endif()
endif()
if(NOT MODERNGEKKO_OPTIMIZATION_LEVEL MATCHES "^[012]$")
  message(FATAL_ERROR "MODERNGEKKO_OPTIMIZATION_LEVEL must be 0, 1, or 2")
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(GAME_ID "" CACHE STRING "Six-character disc ID, e.g. G4QE01")
if(NOT GAME_ID)
  message(FATAL_ERROR "Set -DGAME_ID=<disc id> (e.g. -DGAME_ID=G4QE01)")
endif()
set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated" CACHE PATH "DolRecomp output directory (generated.h, generated_smc.txt, main.dol, chunks/)")
if(NOT EXISTS "${GENERATED_DIR}")
  message(FATAL_ERROR "Generated directory not found at ${GENERATED_DIR}. Set -DGENERATED_DIR=<path to DolRecomp output>")
endif()
set(GXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../GXRuntime" CACHE PATH "GXRuntime checkout")
if(NOT EXISTS "${GXRUNTIME_DIR}")
  message(FATAL_ERROR "GXRuntime not found at ${GXRUNTIME_DIR}. Set -DGXRUNTIME_DIR=<path to GXRuntime>")
endif()
set(CHASSIS_ABI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Source/Core/Core/PowerPC/StaticRecomp"
    CACHE PATH "RecompCore StaticRecomp ABI directory")

set(MODULE_NAME "g${GAME_ID}_recomp")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MODULE_TABLES "${CMAKE_CURRENT_BINARY_DIR}/module_tables.inc")
add_custom_command(
  OUTPUT "${MODULE_TABLES}"
  COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol" "${MODULE_TABLES}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_tables.py"
          "${GENERATED_DIR}/generated.h" "${GENERATED_DIR}/generated_smc.txt"
          "${GENERATED_DIR}/main.dol"
  COMMENT "Generating module_tables.inc")

file(GLOB CHUNK_SOURCES "${GENERATED_DIR}/chunks/*.c")
file(GLOB_RECURSE REL_CHUNK_SOURCES
     CONFIGURE_DEPENDS "${GENERATED_DIR}/rels/*/chunks/*.c")
set(REL_MODULE_TABLES "${CMAKE_CURRENT_BINARY_DIR}/rel_module_tables.inc")
add_custom_command(
  OUTPUT "${REL_MODULE_TABLES}"
  COMMAND Python3::Interpreter
          "${CMAKE_CURRENT_SOURCE_DIR}/gen_rel_module_tables.py"
          "${GENERATED_DIR}" "${REL_MODULE_TABLES}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_rel_module_tables.py"
          ${REL_CHUNK_SOURCES}
  COMMENT "Generating rel_module_tables.inc")
set(MODULE_PATCH_SOURCE "${GENERATED_DIR}/module_patch.c")
set(MODULE_PATCH_ADDRESSES "${GENERATED_DIR}/module_patch_addresses.txt")
set(MODULE_PATCH_SOURCES)
if(EXISTS "${MODULE_PATCH_SOURCE}")
  if(NOT EXISTS "${MODULE_PATCH_ADDRESSES}")
    message(FATAL_ERROR
      "module_patch.c requires the explicit sidecar module_patch_addresses.txt")
  endif()
  list(APPEND MODULE_PATCH_SOURCES "${MODULE_PATCH_SOURCE}")
endif()

set(MODULE_PATCH_POINTS)
if(MODULE_PATCH_SOURCES)
  set(MODULE_PATCH_POINTS "${CMAKE_CURRENT_BINARY_DIR}/module_patch_points.h")
  add_custom_command(
    OUTPUT "${MODULE_PATCH_POINTS}"
    COMMAND Python3::Interpreter
            "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_patch_dispatch.py"
            "${MODULE_PATCH_ADDRESSES}" "${GENERATED_DIR}/generated.h"
            "${MODULE_PATCH_POINTS}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/gen_module_patch_dispatch.py"
            "${MODULE_PATCH_ADDRESSES}" "${GENERATED_DIR}/generated.h"
    COMMENT "Validating sparse module-patch addresses")
endif()

add_library(${MODULE_NAME} SHARED
  module_export.c
  "${MODULE_TABLES}"
  "${REL_MODULE_TABLES}"
  "${GXRUNTIME_DIR}/src/core/cpu.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_table.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_float.c"
  "${GXRUNTIME_DIR}/src/core/cpu_interpreter_integer.c"
  ${MODULE_PATCH_POINTS}
  ${MODULE_PATCH_SOURCES}
  ${CHUNK_SOURCES}
  ${REL_CHUNK_SOURCES})

set_target_properties(${MODULE_NAME} PROPERTIES PREFIX "" OUTPUT_NAME "${MODULE_NAME}" UNITY_BUILD OFF)
set_source_files_properties("${MODULE_TABLES}" PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)
set_source_files_properties("${REL_MODULE_TABLES}"
                            PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)
if(MODULE_PATCH_POINTS)
  set_source_files_properties("${MODULE_PATCH_POINTS}"
                              PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)
  set_source_files_properties(${CHUNK_SOURCES}
                              PROPERTIES OBJECT_DEPENDS "${MODULE_PATCH_POINTS}")
  if(MSVC)
    set_property(SOURCE ${CHUNK_SOURCES} APPEND PROPERTY
                 COMPILE_OPTIONS "/FI${MODULE_PATCH_POINTS}")
  else()
    set_property(SOURCE ${CHUNK_SOURCES} APPEND PROPERTY
                 COMPILE_OPTIONS "-include" "${MODULE_PATCH_POINTS}")
  endif()
endif()

target_compile_definitions(${MODULE_NAME} PRIVATE MODULE_GAME_ID="${GAME_ID}")
if(MODULE_PATCH_SOURCES)
  target_compile_definitions(${MODULE_NAME} PRIVATE MODERNGEKKO_HAVE_MODULE_PATCH=1)
endif()

target_include_directories(${MODULE_NAME} PRIVATE
  "${GENERATED_DIR}"
  "${GXRUNTIME_DIR}/include"
  "${CHASSIS_ABI_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}")

if(NOT WIN32)
  target_link_libraries(${MODULE_NAME} PRIVATE m)
endif()

# Keep each generated chunk as its own translation unit. Clang uses ThinLTO and
# lld; GCC deliberately avoids LTO until the differential parity gate covers it.
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
  set(MODERNGEKKO_MODULE_OPTIMIZATION
      "-O${MODERNGEKKO_OPTIMIZATION_LEVEL}")
  target_compile_options(${MODULE_NAME} PRIVATE ${MODERNGEKKO_MODULE_OPTIMIZATION} -fvisibility=hidden
                         -ffp-contract=off -fno-fast-math)
  if(MODERNGEKKO_ENABLE_LTO)
    target_compile_options(${MODULE_NAME} PRIVATE -flto=thin)
    target_link_options(${MODULE_NAME} PRIVATE -flto=thin)
    if(APPLE)
      # Apple ld can reuse ThinLTO backend objects across patch-only relinks.
      # Keep the cache inside the stable incremental module workspace.
      file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lto-cache")
      target_link_options(${MODULE_NAME} PRIVATE
                          "LINKER:-cache_path_lto,${CMAKE_CURRENT_BINARY_DIR}/lto-cache")
    endif()
  endif()
  if(UNIX AND NOT APPLE)
    target_link_options(${MODULE_NAME} PRIVATE -fuse-ld=lld -Wl,--thinlto-jobs=4)
  endif()
elseif(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  set(MODERNGEKKO_MODULE_OPTIMIZATION
      "-O${MODERNGEKKO_OPTIMIZATION_LEVEL}")
  target_compile_options(${MODULE_NAME} PRIVATE ${MODERNGEKKO_MODULE_OPTIMIZATION} -fvisibility=hidden
                         -ffp-contract=off -fno-fast-math)
elseif(MSVC)
  if(MODERNGEKKO_FAST_BUILD)
    target_compile_options(${MODULE_NAME} PRIVATE /Od)
  else()
    target_compile_options(${MODULE_NAME} PRIVATE /O2)
  endif()
  target_compile_options(${MODULE_NAME} PRIVATE /fp:strict)
endif()
