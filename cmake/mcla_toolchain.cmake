# Compiler requirements and the flags every target of the build shares (docs/PLAN.md 3.4, 3.7).
#
# Included by the root CMakeLists.txt right after project(). It sets variables, so it must run in the root scope. It
# defines no targets; its one function, mcla_apply_link_policy(), is for the executable.
#
#   Compiler       Clang with the GNU-style driver (clang++, not clang-cl): the SDK refuses cl.exe, and the generated
#                  code relies on Clang semantics (weak aliases, volatile loads, builtins).
#   -march         x86-64-v2, the baseline the SDK is built with (its presets put it in CMAKE_CXX_FLAGS; ours do the
#                  same). A configure without the presets still gets it. Never raise it casually: x86-64-v3 / znver5 is
#                  experiment E5 with a determinism check.
#   FP contraction The build never passes -ffp-contract. Clang's default (fuse within a statement) cannot produce an
#                  FMA at x86-64-v2, and the SDK sets no more for the generated code: fusing a PPC mul+add would
#                  change float results, and physics with them.
#   Debug info     RelWithDebInfo carries -gline-tables-only (readable crash stacks, a PDB the size of the line
#                  tables); Release carries none. No LTO anywhere.
include_guard(GLOBAL)

# The unit tests are plain C++ and build with any compiler (mcla/tests/CMakeLists.txt even works standalone). With
# -DMCLA_TESTS_ONLY=ON and a compiler that is not the GNU-style Clang of the presets, leave everything else alone.
if(MCLA_TESTS_ONLY AND NOT (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"))
  message(STATUS "mcla: MCLA_TESTS_ONLY with ${CMAKE_CXX_COMPILER_ID} (${CMAKE_CXX_COMPILER}): toolchain policy not applied")
  return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR
    "mcla-x86 needs the Clang compiler (found ${CMAKE_CXX_COMPILER_ID}: ${CMAKE_CXX_COMPILER}).\n"
    "Configure with a preset: cmake --preset win-amd64-relwithdebinfo")
endif()
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
  message(FATAL_ERROR
    "mcla-x86 needs the GNU-style Clang driver (clang++), not clang-cl (${CMAKE_CXX_COMPILER}).")
endif()
if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
  message(FATAL_ERROR "mcla-x86 needs Clang 18 or newer (found ${CMAKE_CXX_COMPILER_VERSION}); the pin is 20.1.8.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "mcla-x86 is a 64-bit program.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CXX_FLAGS MATCHES "(^| )-march=")
  string(APPEND CMAKE_CXX_FLAGS " -march=x86-64-v2")
endif()

# CMake (policy CMP0141) would add "-g -Xclang -gcodeview" to every RelWithDebInfo compile through the target property
# MSVC_DEBUG_INFORMATION_FORMAT. Empty turns that off; the line-tables-only flag below replaces it. The driver adds
# -debug to the lld-link command line because -g flags are among the link flags too, so the PDB is still written.
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "")
string(APPEND CMAKE_CXX_FLAGS_RELWITHDEBINFO " -gline-tables-only")

# One thing less to go wrong: the generated code's TUs are 1-2 MB each, so a unity build would only cost parallelism.
set(CMAKE_UNITY_BUILD OFF)

# Linker policy for executables and DLLs of the project (lld-link, selected by CMAKE_LINKER_TYPE=LLD in the preset):
#   /INCREMENTAL:NO   always
#   /OPT:REF          always
#   /OPT:ICF          Release only. Folding identical functions of the generated code is fine for the register
#                     table (it maps guest addresses to function pointers), but RelWithDebInfo stays unfolded so a
#                     crash stack names the function that crashed.
# The target_link_options calls live in mcla_apply_link_policy(); the executable and nothing else calls it.
function(mcla_apply_link_policy target)
  target_link_options(${target} PRIVATE
    "LINKER:/INCREMENTAL:NO"
    "LINKER:/OPT:REF"
    "$<IF:$<CONFIG:Release>,LINKER:/OPT:ICF,LINKER:/OPT:NOICF>"
    "$<$<CONFIG:RelWithDebInfo>:LINKER:/DEBUG>")
endfunction()

message(STATUS "mcla: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}, CXX flags '${CMAKE_CXX_FLAGS}'")
