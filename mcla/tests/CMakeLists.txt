# mcla/tests: the pure C++ unit tests of the MCLA hooks' arithmetic (PLAN W0.6, ported AS-IS from the iOS port).
# No SDK, no game, no generated code: every test compiles standalone from the hook headers.
#
#   CTest test / target        source                          what it checks
#   mcla_sim_math_test         test_sim_math.cpp               hooks/mcla_t_sim_math.h (n/60 quantiser, camera and
#                                                              chassis smoothing, race clock, movie stepper, hold rule)
#                                                              and, through it, hooks/frame_clock_policy.h
#   mcla_game_state_test       mcla_game_state_test.cpp        hooks/mcla_game_state.{h,cpp} (the screen classifier)
#   mcla_present_cadence_test  mcla_present_cadence_test.cpp   present_cadence.h next to this file (on-screen
#                                                              interval histogram, refresh slot choice)
#
# `mcla_unit_tests` is the aggregate target (`cmake --build <dir> --target mcla_unit_tests`). The three executables
# are also part of ALL, so a normal full build is followed by `ctest` (multi-config generators: `ctest -C <config>`).
# Each test is one executable and exits 1 on the first failed check, which is what CTest looks at.
#
# Input, set by the root CMakeLists.txt before add_subdirectory(mcla/tests) or with -D on the command line:
#   MCLA_HOOKS_DIR   directory with mcla_t_sim_math.h, frame_clock_policy.h, mcla_game_state.h and mcla_game_state.cpp.
#                    Default: ../hooks (mcla/hooks). A relative value is taken from the top-level source directory.
#
# From the root: add_subdirectory(mcla/tests) (the root calls enable_testing()). Alone, with no SDK or toolchain
# preset: cmake -S mcla/tests -B <dir> -G Ninja && cmake --build <dir> && ctest --test-dir <dir>

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.25)
  project(mcla_unit_tests LANGUAGES CXX)
  enable_testing()
endif()

if(NOT DEFINED MCLA_HOOKS_DIR)
  set(MCLA_HOOKS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../hooks")
endif()
get_filename_component(_mcla_tests_hooks_dir "${MCLA_HOOKS_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")

# Say so at configure time when the GAME slice's files are not where the tests expect them.
foreach(_mcla_tests_input IN ITEMS mcla_t_sim_math.h frame_clock_policy.h mcla_game_state.h mcla_game_state.cpp)
  if(NOT EXISTS "${_mcla_tests_hooks_dir}/${_mcla_tests_input}")
    message(FATAL_ERROR "mcla/tests: ${_mcla_tests_input} not found in MCLA_HOOKS_DIR (${_mcla_tests_hooks_dir})")
  endif()
endforeach()

add_custom_target(mcla_unit_tests)

# One executable plus one CTest test of the same name. `std` is the C++ standard the iOS port compiled the test with.
function(mcla_add_unit_test target std)
  add_executable(${target} ${ARGN})
  set_target_properties(${target} PROPERTIES CXX_STANDARD ${std} CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
  if(MSVC)  # cl and clang-cl
    target_compile_options(${target} PRIVATE /W4 /utf-8)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra)
  endif()
  add_test(NAME ${target} COMMAND ${target})
  set_tests_properties(${target} PROPERTIES LABELS "mcla;unit" TIMEOUT 120)
  add_dependencies(mcla_unit_tests ${target})
endfunction()

mcla_add_unit_test(mcla_sim_math_test 23 test_sim_math.cpp)
target_include_directories(mcla_sim_math_test PRIVATE "${_mcla_tests_hooks_dir}")

mcla_add_unit_test(mcla_game_state_test 20 mcla_game_state_test.cpp "${_mcla_tests_hooks_dir}/mcla_game_state.cpp")
target_include_directories(mcla_game_state_test PRIVATE "${_mcla_tests_hooks_dir}")

# present_cadence.h is found next to the test (a quoted include looks in the including file's directory first).
mcla_add_unit_test(mcla_present_cadence_test 23 mcla_present_cadence_test.cpp)
