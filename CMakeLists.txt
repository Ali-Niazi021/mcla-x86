# mcla-x86: Midnight Club: Los Angeles (Xbox 360), statically recompiled to native x86-64 Windows (docs/PLAN.md).
#
# What this builds, from three inputs that live outside the repository (docs/PLAN.md section 5):
#
#   the SDK         ReXGlue v0.10.0 + the patch series of patches/sdk, built and installed by tools/prepare_sdk.py
#                   (find_package; CMAKE_PREFIX_PATH = <MCLA_WORK>/sdk/install)
#   generated code  the game translated to C++ by tools/codegen.py into MCLA_GENERATED_DIR
#                   (<MCLA_WORK>/generated/default): sources.cmake, nfsmw_*.cpp, nfsmw_pch.h, nfsmw_init.h
#   this repository src/host (the app), mcla/ (hooks, PC profile, unit tests)
#
# and produces
#
#   nfsmw_recomp    static library of the generated code (cmake/mcla_recomp.cmake)
#   mcla            the executable: src/host + the hook and profile sources of mcla/sources.cmake, compiled into it as
#                   objects so the hooks' strong definitions override the generated code's weak defaults. Lands in
#                   <build>/bin/<Config>/mcla.exe with the SDK's DLLs next to it.
#   mcla_*_test     the unit tests of mcla/tests (CTest)
#
# Configure and build with the presets (Ninja Multi-Config, Clang 20.1.8 from <MCLA_WORK>/toolchain):
#
#   cmake --preset win-amd64-relwithdebinfo
#   cmake --build --preset win-amd64-relwithdebinfo
#   ctest --preset win-amd64-relwithdebinfo
#
# Both configurations share the build tree <MCLA_WORK>/build: `cmake --build <MCLA_WORK>/build --config Release`.
#
# -DMCLA_TESTS_ONLY=ON (or the win-amd64-tests preset) configures just the unit tests: no SDK, no generated code.
cmake_minimum_required(VERSION 3.25)

option(MCLA_TESTS_ONLY "Configure only the unit tests of mcla/tests (no SDK, no generated code, no executable)" OFF)
set(MCLA_WORK "$ENV{MCLA_WORK}" CACHE PATH "Work tree of docs/PLAN.md section 5 (the presets set it)")
set(MCLA_GENERATED_DIR "" CACHE PATH "Output directory of tools/codegen.py (sources.cmake, nfsmw_*.cpp, nfsmw_pch.h)")
set(MCLA_PROJECT_NAME "nfsmw" CACHE STRING "Project name of the generated code (nfsmw_init.h, nfsmw_pch.h, ...)")
set(MCLA_HOOKS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/mcla/hooks" CACHE PATH "Hook sources and headers (the unit tests include them)")

# C++ only: the SDK's C code (FFmpeg, SDL, ...) is already inside its DLLs.
project(mcla VERSION 0.1.0 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(mcla_toolchain)

enable_testing()

if(MCLA_TESTS_ONLY)
  message(STATUS "mcla: MCLA_TESTS_ONLY, configuring mcla/tests only")
else()
  # --- Inputs ------------------------------------------------------------------------------------------------------
  include(mcla_sdk)
  include(mcla_recomp)

  if(NOT MCLA_GENERATED_DIR)
    message(FATAL_ERROR
      "MCLA_GENERATED_DIR is not set. Configure with a preset (cmake --preset win-amd64-relwithdebinfo), or pass "
      "-DMCLA_GENERATED_DIR=<dir written by tools/codegen.py>.\n"
      "For the unit tests alone: -DMCLA_TESTS_ONLY=ON")
  endif()
  if(NOT EXISTS "${MCLA_GENERATED_DIR}/sources.cmake")
    message(FATAL_ERROR
      "No generated code: ${MCLA_GENERATED_DIR}/sources.cmake does not exist.\n"
      "Generate it first:  python tools/codegen.py\n"
      "(the SDK must be installed before that: python tools/prepare_sdk.py fetch apply build).\n"
      "For the unit tests alone: -DMCLA_TESTS_ONLY=ON")
  endif()
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/mcla/sources.cmake")
    message(FATAL_ERROR "mcla/sources.cmake (the game layer's build contract) is missing from the source tree")
  endif()

  # --- The generated code ------------------------------------------------------------------------------------------
  mcla_add_recomp_library(nfsmw_recomp GENERATED_DIR "${MCLA_GENERATED_DIR}" PROJECT "${MCLA_PROJECT_NAME}")

  # --- The game layer: MCLA_HOOK_SOURCES, MCLA_HOOK_DEFINITIONS, MCLA_HOOK_INCLUDE_DIRS --------------------------------
  include("${CMAKE_CURRENT_SOURCE_DIR}/mcla/sources.cmake")
  foreach(_mcla_hook_var MCLA_HOOK_SOURCES MCLA_HOOK_DEFINITIONS MCLA_HOOK_INCLUDE_DIRS)
    if(NOT ${_mcla_hook_var})
      message(FATAL_ERROR "mcla/sources.cmake did not set ${_mcla_hook_var}")
    endif()
  endforeach()
  unset(_mcla_hook_var)

  # --- The executable ----------------------------------------------------------------------------------------------
  set(MCLA_HOST_SOURCES
      src/host/main.cpp
      src/host/mcla_app.cpp
      src/host/watchdog.cpp
      src/host/input_script.cpp
      src/host/win32_time.cpp
      src/host/win32_wait.cpp
      src/host/window_fit.cpp
      src/host/system_status.cpp
      src/host/gpu_priority.cpp
      src/host/thread_policy.cpp
      src/host/power_boost.cpp
      src/host/pf_probe.cpp
      src/host/overlay/overlay_feed.cpp
      src/host/overlay/overlay_sample.cpp
      src/host/overlay/overlay_items.cpp
      src/host/overlay/overlay_ui.cpp)

  # The hook and profile sources are objects of the executable itself, not a library: a hook is a strong definition
  # that must replace the weak default inside the generated code, and an archive member nothing references is never
  # pulled in. The SDK's entry point and ReXApp are added by rexglue_configure_target().
  # The application manifest (UTF-8 active code page, supportedOS) is a source: CMake hands a .manifest to the linker as
  # /MANIFESTINPUT, merged into the one lld embeds (/MANIFEST:EMBED). It carries its own trustInfo, so the linker's
  # default UAC manifest is switched off below: merged beside it, that one lands in the wrong namespace and the program
  # does not start (side-by-side error 14001), as the launcher found.
  set(MCLA_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/src/host/mcla.manifest")
  add_executable(mcla WIN32 ${MCLA_HOST_SOURCES} ${MCLA_HOOK_SOURCES} "${MCLA_MANIFEST}")
  target_link_options(mcla PRIVATE "LINKER:/MANIFESTUAC:NO")

  set_source_files_properties(${MCLA_HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Wno-unused-parameter")
  target_include_directories(mcla PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/src/host"
      "${MCLA_GENERATED_DIR}"          # nfsmw_init.h, nfsmw_pch.h (the hooks include it)
      ${MCLA_HOOK_INCLUDE_DIRS})       # nfsc_progress.h, pc_profile.h, mcla_*.h
  target_compile_definitions(mcla PRIVATE
      ${MCLA_HOOK_DEFINITIONS}
      "MCLA_SDK_SERIES=\"${MCLA_SDK_SERIES}\"")

  # rexglue_configure_target: the SDK's wWinMain and ReXApp sources, REXGLUE_BUILD_CONFIG, -msse4.1, and the staging of
  # rexruntime.dll and the xenos GPU plugin. winmm: timeBeginPeriod. Synchronization: WaitOnAddress. dxgi: the adapter
  # list in the log.
  target_link_libraries(mcla PRIVATE nfsmw_recomp rex::runtime winmm Synchronization dxgi)
  rexglue_configure_target(mcla GPU_PLUGINS xenos)
  mcla_stage_sdk_runtime(mcla)
  mcla_apply_link_policy(mcla)

  set_target_properties(mcla PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"   # Ninja Multi-Config appends /<Config>
      WIN32_EXECUTABLE ON)

  # CPU boost cap restore helper (src/host/power_boost.h): waits for the game and puts a boost cap back if the game died
  # without doing it. Built from the SDK-free half of power_boost.cpp; powrprof is loaded at run time.
  add_executable(mcla_power_restore tools/power_restore/power_restore.cpp src/host/power_boost.cpp)
  target_compile_definitions(mcla_power_restore PRIVATE MCLA_POWER_STANDALONE=1 NOMINMAX WIN32_LEAN_AND_MEAN)
  target_compile_features(mcla_power_restore PRIVATE cxx_std_20)
  set_target_properties(mcla_power_restore PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  add_dependencies(mcla mcla_power_restore)
endif()

# --- Unit tests -------------------------------------------------------------------------------------------------------
add_subdirectory(mcla/tests)

# --- Launcher (launcher/: settings UI + Launch button; fetches ImGui and nlohmann/json at configure time) -------------
option(MCLA_BUILD_LAUNCHER "Build mcla_launcher.exe next to mcla.exe" ON)
if(MCLA_BUILD_LAUNCHER)
  add_subdirectory(launcher)
endif()
