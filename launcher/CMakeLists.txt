# launcher: the Windows launcher of Midnight Club: Los Angeles (x86). A Win32 + Direct3D 11 + Dear ImGui settings window
# that starts mcla.exe with the chosen settings as `--<cvar>=<value>` arguments.
#
#   target                     what it is
#   mcla_launcher              the launcher (WIN32 executable). Lands in <build>/bin[/<Config>]/mcla_launcher.exe, next to
#                              mcla.exe, with settings.json (launcher/schema/settings.json) copied beside it
#   mcla_launcher_core         static library, launcher/src/core: schema loader, settings store, command line, game
#                              process (no UI; what the tests link)
#   mcla_imgui                 static library, Dear ImGui with its Win32 and DX11 backends
#   mcla_launcher_core_test    console test of the core (CTest test of the same name; also validates the real schema)
#
# Third-party sources are fetched at configure time, pinned by release archive and SHA-256 (both MIT):
#   Dear ImGui v1.91.9      github.com/ocornut/imgui
#   nlohmann/json v3.11.3   github.com/nlohmann/json   (the official json.tar.xz, hash published in its README)
# To build offline point FETCHCONTENT_SOURCE_DIR_MCLA_LAUNCHER_IMGUI / FETCHCONTENT_SOURCE_DIR_MCLA_LAUNCHER_JSON at
# unpacked copies.
#
# Options
#   MCLA_LAUNCHER_GUI     build mcla_launcher and fetch ImGui (default ON; OFF under the root's MCLA_TESTS_ONLY, which
#                         must stay light: then only the core and its test are built)
#   MCLA_LAUNCHER_TESTS   build mcla_launcher_core_test and register it with CTest (default ON)
#
# From the root: add_subdirectory(launcher) (the root calls enable_testing()). Alone:
#   cmake -S launcher -B <dir> -G Ninja -DCMAKE_CXX_COMPILER=<clang++> && cmake --build <dir> && ctest --test-dir <dir>

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  # Standalone with a portable LLVM: llvm-rc and lld-link sit next to clang++, but CMake only looks at PATH, and its
  # Windows-Clang module wants a resource compiler already inside project().
  if(CMAKE_CXX_COMPILER)
    set(_mcla_cxx "${CMAKE_CXX_COMPILER}")
    if(NOT IS_ABSOLUTE "${_mcla_cxx}")
      find_program(_mcla_cxx_found NAMES "${_mcla_cxx}")
      if(_mcla_cxx_found)
        set(_mcla_cxx "${_mcla_cxx_found}")
      endif()
    endif()
    get_filename_component(_mcla_cxx_dir "${_mcla_cxx}" DIRECTORY)
    if(_mcla_cxx_dir)
      if(NOT CMAKE_RC_COMPILER AND EXISTS "${_mcla_cxx_dir}/llvm-rc.exe")
        set(CMAKE_RC_COMPILER "${_mcla_cxx_dir}/llvm-rc.exe" CACHE FILEPATH "Resource compiler")
      endif()
      if(NOT CMAKE_LINKER_TYPE AND EXISTS "${_mcla_cxx_dir}/lld-link.exe")
        set(CMAKE_LINKER_TYPE LLD CACHE STRING "Linker type")
      endif()
    endif()
  endif()
  # A single-config generator would otherwise end up with CMAKE_BUILD_TYPE=Debug, the debug CRT (msvcrtd, which only
  # machines with Visual Studio have). Must be decided before project().
  get_property(_mcla_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
  if(NOT _mcla_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type")
  endif()
  project(mcla_launcher LANGUAGES CXX)
  enable_testing()
endif()

if(NOT WIN32)
  message(STATUS "launcher: Windows only, skipped")
  return()
endif()

if(MCLA_TESTS_ONLY)
  set(_mcla_launcher_gui_default OFF)
else()
  set(_mcla_launcher_gui_default ON)
endif()
option(MCLA_LAUNCHER_GUI "Build the launcher window (fetches Dear ImGui)" ${_mcla_launcher_gui_default})
option(MCLA_LAUNCHER_TESTS "Build and register the launcher core test" ON)

include(FetchContent)

# --- nlohmann/json: header only, so no add_subdirectory of its own CMake (SOURCE_SUBDIR names a directory that does not
# exist: MakeAvailable only downloads and unpacks) -----------------------------------------------------------------------
FetchContent_Declare(mcla_launcher_json
  URL "https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz"
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR mcla_no_cmake_here)
FetchContent_MakeAvailable(mcla_launcher_json)

add_library(mcla_launcher_json INTERFACE)
target_include_directories(mcla_launcher_json SYSTEM INTERFACE "${mcla_launcher_json_SOURCE_DIR}/include")

# --- The core ----------------------------------------------------------------------------------------------------------
add_library(mcla_launcher_core STATIC
  src/core/text.cpp
  src/core/settings_schema.cpp
  src/core/settings_store.cpp
  src/core/game_process.cpp)
target_include_directories(mcla_launcher_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(mcla_launcher_core PUBLIC UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(mcla_launcher_core PUBLIC mcla_launcher_json shell32 ole32 uuid)
set_target_properties(mcla_launcher_core PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Same warning level for everything of ours; third-party code gets none of it.
function(mcla_launcher_warnings target)
  if(MSVC)  # cl and clang-cl
    target_compile_options(${target} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
endfunction()
mcla_launcher_warnings(mcla_launcher_core)

# --- The core test -----------------------------------------------------------------------------------------------------
if(MCLA_LAUNCHER_TESTS)
  add_executable(mcla_launcher_core_test src/core/test/core_test.cpp)
  target_link_libraries(mcla_launcher_core_test PRIVATE mcla_launcher_core)
  target_compile_definitions(mcla_launcher_core_test PRIVATE
    "MCLA_LAUNCHER_SCHEMA_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}/schema/settings.json\"")
  set_target_properties(mcla_launcher_core_test PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  mcla_launcher_warnings(mcla_launcher_core_test)
  add_test(NAME mcla_launcher_core_test COMMAND mcla_launcher_core_test)
  set_tests_properties(mcla_launcher_core_test PROPERTIES LABELS "mcla;unit;launcher" TIMEOUT 120)
endif()

# --- The launcher ------------------------------------------------------------------------------------------------------
if(NOT MCLA_LAUNCHER_GUI)
  message(STATUS "launcher: MCLA_LAUNCHER_GUI is OFF, building the core and its test only")
  return()
endif()

FetchContent_Declare(mcla_launcher_imgui
  URL "https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.tar.gz"
  URL_HASH SHA256=3872a5f90df78fced023c1945f4466b654fd74573370b77b17742149763a7a7c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)  # no CMakeLists.txt inside: MakeAvailable only downloads and unpacks
FetchContent_MakeAvailable(mcla_launcher_imgui)

set(_imgui "${mcla_launcher_imgui_SOURCE_DIR}")
add_library(mcla_imgui STATIC
  "${_imgui}/imgui.cpp"
  "${_imgui}/imgui_draw.cpp"
  "${_imgui}/imgui_tables.cpp"
  "${_imgui}/imgui_widgets.cpp"
  "${_imgui}/imgui_demo.cpp"
  "${_imgui}/backends/imgui_impl_win32.cpp"
  "${_imgui}/backends/imgui_impl_dx11.cpp")
target_include_directories(mcla_imgui SYSTEM PUBLIC "${_imgui}" "${_imgui}/backends")
# The demo and the debug tools stay out of the program (ShowDemoWindow etc. are empty stubs, so a call still links).
target_compile_definitions(mcla_imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_DISABLE_DEBUG_TOOLS)
target_link_libraries(mcla_imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi user32 gdi32)
set_target_properties(mcla_imgui PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

# Everything under launcher/src except the core (a library above) and its test.
file(GLOB_RECURSE MCLA_LAUNCHER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER MCLA_LAUNCHER_SOURCES EXCLUDE REGEX "/src/core/")

# Resources (icon, manifest) when the UI slice provides them: launcher/resources/*.rc.
file(GLOB MCLA_LAUNCHER_RESOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/*.rc")
if(MCLA_LAUNCHER_RESOURCES)
  if(NOT CMAKE_RC_COMPILER)
    # llvm-rc lives next to clang++ in the portable toolchain; CMake only looks at PATH.
    get_filename_component(_mcla_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(CMAKE_RC_COMPILER NAMES llvm-rc rc HINTS "${_mcla_cxx_dir}")
  endif()
  enable_language(RC)
endif()

if(NOT MCLA_LAUNCHER_SOURCES)
  message(WARNING "launcher: no UI sources under launcher/src (main.cpp, ui/): mcla_launcher is not defined")
  return()
endif()

# A .manifest among the sources is handed to the linker as /MANIFESTINPUT (merged into the one it embeds): it must not
# be an RC resource too, or the resource is defined twice.
file(GLOB MCLA_LAUNCHER_MANIFESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/*.manifest")

add_executable(mcla_launcher WIN32 ${MCLA_LAUNCHER_SOURCES} ${MCLA_LAUNCHER_RESOURCES} ${MCLA_LAUNCHER_MANIFESTS})
if(MCLA_LAUNCHER_MANIFESTS)
  # The manifest carries its own trustInfo (asInvoker); the linker's default one would be merged in beside it and end up
  # in the wrong namespace (the program then fails to start: side-by-side error 14001).
  target_link_options(mcla_launcher PRIVATE "LINKER:/MANIFESTUAC:NO")
endif()
target_include_directories(mcla_launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(mcla_launcher PRIVATE mcla_launcher_core mcla_imgui comdlg32 shcore advapi32)
set_target_properties(mcla_launcher PROPERTIES
  CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"   # multi-config generators append /<Config>, as for mcla.exe
  WIN32_EXECUTABLE ON)
mcla_launcher_warnings(mcla_launcher)

# settings.json next to the executable. LINK_DEPENDS relinks (and so reruns the copy) when the schema changes.
set(MCLA_LAUNCHER_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/schema/settings.json")
set_property(TARGET mcla_launcher APPEND PROPERTY LINK_DEPENDS "${MCLA_LAUNCHER_SCHEMA}")
add_custom_command(TARGET mcla_launcher POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MCLA_LAUNCHER_SCHEMA}" "$<TARGET_FILE_DIR:mcla_launcher>/settings.json"
  VERBATIM)
