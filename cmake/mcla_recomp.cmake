# The generated code as a static library (docs/PLAN.md 3.7).
#
#   mcla_add_recomp_library(<target> GENERATED_DIR <dir> PROJECT <name>)
#
# builds ${GENERATED_DIR}/sources.cmake's GENERATED_SOURCES (written by tools/codegen.py: <name>_init.cpp,
# <name>_register.cpp, <name>_recomp.N.cpp) into a STATIC library. Everything the SDK's own generated/rexglue.cmake
# glue does for its generated target is done here instead, because that glue's paths are relative to wherever codegen
# ran and our build includes sources.cmake directly:
#
#   * -fasync-exceptions (clang on Windows): the guest's SEH scopes become __try/__except, which traps hardware
#     faults only under asynchronous EH. It goes on the TARGET, not on the sources: CMake compiles the precompiled
#     header from its own TU, which carries no source properties, and clang rejects a PCH whose EH mode disagrees with
#     the TU that includes it.
#   * rexglue_apply_target_settings() (-msse4.1 on x86-64, from the SDK package config)
#   * one shared precompiled header, <name>_pch.h. No unity build.
#   * the register TU (the table of ~30k guest functions) at -O0 and without the PCH: one huge function run once at
#     boot, which took more than 10 minutes at -O3 before the register-table patch and still has nothing to gain from
#     optimisation.
#   * a Ninja job pool "recomp" for the compile steps: min(logical CPUs, floor(free RAM in GiB / 0.6)) computed at
#     configure time (measured: up to 0.4 GB per compiler on Windows), or -DMCLA_RECOMP_JOBS=<n>.
#
# Debug info comes from the configuration flags (RelWithDebInfo: -gline-tables-only; Release: none), not from a
# generated-code-only option, so a Release build does not pay for line tables of 120 MB of C++.
#
# What it never adds: -ffp-contract, -ffast-math, a different -march, LTO. See cmake/mcla_toolchain.cmake.
include_guard(GLOBAL)

set(MCLA_RECOMP_JOBS "" CACHE STRING
    "Compile jobs of the generated code at once (empty: min(logical CPUs, free RAM GiB / 0.6), computed at configure time)")

# Sets <out_var> to the size of the job pool.
function(mcla_recomp_job_count out_var)
  if(MCLA_RECOMP_JOBS)
    set(${out_var} "${MCLA_RECOMP_JOBS}" PARENT_SCOPE)
    return()
  endif()
  cmake_host_system_information(RESULT _cpus QUERY NUMBER_OF_LOGICAL_CORES)
  cmake_host_system_information(RESULT _free_mib QUERY AVAILABLE_PHYSICAL_MEMORY)
  # floor(free GiB / 0.6) = floor(free MiB * 10 / 6144)
  math(EXPR _ram_jobs "${_free_mib} * 10 / 6144")
  set(_jobs ${_cpus})
  if(_ram_jobs LESS _jobs)
    set(_jobs ${_ram_jobs})
  endif()
  if(_jobs LESS 1)
    set(_jobs 1)
  endif()
  message(STATUS "mcla: recomp job pool ${_jobs} (${_cpus} logical CPUs, ${_free_mib} MiB free RAM)")
  set(${out_var} "${_jobs}" PARENT_SCOPE)
endfunction()

function(mcla_add_recomp_library target)
  cmake_parse_arguments(ARG "" "GENERATED_DIR;PROJECT" "" ${ARGN})
  if(NOT ARG_GENERATED_DIR OR NOT ARG_PROJECT)
    message(FATAL_ERROR "mcla_add_recomp_library: GENERATED_DIR and PROJECT are required")
  endif()

  include("${ARG_GENERATED_DIR}/sources.cmake")  # sets GENERATED_SOURCES (absolute, via CMAKE_CURRENT_LIST_DIR)
  if(NOT GENERATED_SOURCES)
    message(FATAL_ERROR "${ARG_GENERATED_DIR}/sources.cmake lists no sources: run python tools/codegen.py")
  endif()
  # Codegen rewrites sources.cmake only when the list of files changes: configure again when it does.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ARG_GENERATED_DIR}/sources.cmake")

  set(_pch "${ARG_GENERATED_DIR}/${ARG_PROJECT}_pch.h")
  set(_register "${ARG_GENERATED_DIR}/${ARG_PROJECT}_register.cpp")
  foreach(_required "${_pch}" "${_register}")
    if(NOT EXISTS "${_required}")
      message(FATAL_ERROR "${_required} is missing: the generated directory is incomplete, run python tools/codegen.py")
    endif()
  endforeach()

  add_library(${target} STATIC ${GENERATED_SOURCES})
  target_include_directories(${target} PRIVATE "${ARG_GENERATED_DIR}")
  target_link_libraries(${target} PRIVATE rex::runtime)
  rexglue_apply_target_settings(${target})
  target_compile_options(${target} PRIVATE
    -fasync-exceptions
    -Wno-unused-variable)
  set_target_properties(${target} PROPERTIES UNITY_BUILD OFF)

  target_precompile_headers(${target} PRIVATE "${_pch}")
  set_source_files_properties("${_register}" PROPERTIES
    COMPILE_OPTIONS "-O0"
    SKIP_PRECOMPILE_HEADERS ON)

  mcla_recomp_job_count(_jobs)
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS recomp=${_jobs})
  set_target_properties(${target} PROPERTIES JOB_POOL_COMPILE recomp)
endfunction()
