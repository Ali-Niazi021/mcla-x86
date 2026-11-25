# The installed ReXGlue SDK (docs/PLAN.md 3.3, 3.7): find_package, sanity checks, and what the executable needs from it.
#
# The SDK is built and installed once per patch-series change by tools/prepare_sdk.py (prefix $MCLA_WORK/sdk/install,
# Release only, Clang 20.1.8). This build consumes it through CMAKE_PREFIX_PATH, which the presets set.
#
# Included by the root CMakeLists.txt in the root scope: the variables the SDK's package config sets (REXGLUE_SHARE_DIR,
# REXGLUE_VERSION_STRING, ...) and the imported targets (rex::runtime, rex::gpu-xenos, rex::rexglue) must stay visible
# to the caller, so nothing here that matters is inside a function.
#
# Provides:
#   MCLA_SDK_PREFIX   the install prefix the SDK was found in
#   MCLA_SDK_SERIES   "<series fingerprint>/<n>patches" from the install stamp of prepare_sdk.py, or "unknown"
#   mcla_stage_sdk_runtime(<exe target>)   copies the SDK's DLLs next to the executable on every build
include_guard(GLOBAL)

# The SDK installs one configuration (Release), while we build RelWithDebInfo too: let every imported target of the
# SDK map RelWithDebInfo to its Release binaries (rexglueConfig.cmake does it for rex::rexglue only).
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)

# Only CMAKE_PREFIX_PATH (the presets point it at <MCLA_WORK>/sdk/install) may supply the SDK: an SDK install registers
# itself in the CMake user package registry (HKCU), and any other build of ReXGlue on the machine (Renan's, a stock one)
# would be found silently instead of ours.
find_package(rexglue 0.10.0 CONFIG QUIET
  NO_CMAKE_PACKAGE_REGISTRY NO_CMAKE_SYSTEM_PACKAGE_REGISTRY)
if(NOT rexglue_FOUND)
  message(FATAL_ERROR
    "The ReXGlue SDK is not installed where CMake looks (CMAKE_PREFIX_PATH='${CMAKE_PREFIX_PATH}').\n"
    "Build and install it first:  python tools/prepare_sdk.py fetch apply build\n"
    "(it installs into <MCLA_WORK>/sdk/install, the prefix the presets use; MCLA_WORK='${MCLA_WORK}').")
endif()
foreach(_mcla_sdk_target rex::runtime rex::gpu-xenos)
  if(NOT TARGET ${_mcla_sdk_target})
    message(FATAL_ERROR "The installed SDK (${rexglue_DIR}) has no target ${_mcla_sdk_target}: an incomplete install.")
  endif()
endforeach()
unset(_mcla_sdk_target)

# rexglue_DIR = <prefix>/lib/cmake/rexglue
get_filename_component(MCLA_SDK_PREFIX "${rexglue_DIR}/../../.." ABSOLUTE)
foreach(_mcla_sdk_dll rexruntime.dll rexgpu-xenos.dll)
  if(NOT EXISTS "${MCLA_SDK_PREFIX}/bin/${_mcla_sdk_dll}")
    message(FATAL_ERROR "The installed SDK lacks ${MCLA_SDK_PREFIX}/bin/${_mcla_sdk_dll}: an incomplete install.")
  endif()
endforeach()
unset(_mcla_sdk_dll)

# The SDK's REXGLUE_ENABLE_PERF_COUNTERS definition is part of the public headers. Renan's patch series makes it
# unconditional; stock v0.10.0 exports it for every configuration but Release. This build compiles RelWithDebInfo
# against Release-only binaries, so a configuration-dependent definition would give the exe and the DLL different
# headers.
get_target_property(_mcla_sdk_defs rex::runtime INTERFACE_COMPILE_DEFINITIONS)
if(_mcla_sdk_defs MATCHES "CONFIG:Release")
  message(WARNING
    "rex::runtime exports a compile definition that depends on the configuration (${_mcla_sdk_defs}). The SDK is "
    "installed Release only, so RelWithDebInfo would compile the headers differently from the DLL. Is the patch "
    "series applied (python tools/prepare_sdk.py status)?")
endif()
unset(_mcla_sdk_defs)

# Which patch series this SDK was built from, for the start-up banner (src/host/mcla_app.cpp).
set(MCLA_SDK_SERIES "unknown")
set(_mcla_sdk_stamp "${MCLA_SDK_PREFIX}/mcla-x86-sdk-build.json")
if(EXISTS "${_mcla_sdk_stamp}")
  file(READ "${_mcla_sdk_stamp}" _mcla_sdk_stamp_json)
  string(JSON _mcla_sdk_fingerprint ERROR_VARIABLE _mcla_sdk_err1 GET "${_mcla_sdk_stamp_json}" series_fingerprint)
  string(JSON _mcla_sdk_patches ERROR_VARIABLE _mcla_sdk_err2 GET "${_mcla_sdk_stamp_json}" patches)
  if(NOT _mcla_sdk_err1 AND NOT _mcla_sdk_err2)
    set(MCLA_SDK_SERIES "${_mcla_sdk_fingerprint}/${_mcla_sdk_patches}patches")
  endif()
  # A rebuilt SDK rewrites the stamp: configure again so the banner follows.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_mcla_sdk_stamp}")
endif()
unset(_mcla_sdk_stamp)
message(STATUS "mcla: ReXGlue SDK ${REXGLUE_VERSION_STRING} at ${MCLA_SDK_PREFIX} (series ${MCLA_SDK_SERIES})")

# Copies every DLL of the SDK's bin directory (rexruntime, rexgpu-xenos, the FidelityFX runtimes, ...) next to the
# executable. A custom target that runs on every build, not a post-build step of the executable: a rebuilt SDK
# changes the DLLs without relinking mcla.exe, and copy_if_different makes the no-op case free.
# rexglue_configure_target() stages rexruntime and the GPU plugin too, but not the FidelityFX DLLs of an installed
# SDK (it looks for them under their in-tree target names).
function(mcla_stage_sdk_runtime exe_target)
  file(GLOB _dlls CONFIGURE_DEPENDS "${MCLA_SDK_PREFIX}/bin/*.dll" "${MCLA_SDK_PREFIX}/bin/amd_fidelityfx_signed_*.md")
  add_custom_target(${exe_target}_stage_sdk_dlls ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dlls} "$<TARGET_FILE_DIR:${exe_target}>"
    COMMENT "Staging the SDK's DLLs next to ${exe_target}.exe"
    VERBATIM)
  add_dependencies(${exe_target}_stage_sdk_dlls ${exe_target})
endfunction()
