# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28)
project(permute VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Interposition archive. Linked AFTER SUT objects and BEFORE libc, so its
# strong definitions win (ADR-001).
add_library(permute_guest STATIC abi/shim_ipc.c guest/hypercall.c)
target_include_directories(permute_guest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(permute_guest PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Simulated kernel core. Day 1: PRNG, decision domains, determinism bootstrap.
add_library(permute_core STATIC
  kernel/rng.c
  kernel/decisions.c
  kernel/determinism.c
)
target_include_directories(permute_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Test-only fixture: interposes a symbol libc also defines, which is the only
# way to demonstrate link-order precedence on a Linux host.
add_library(shadow_probe STATIC tests/fixtures/shadow_probe.c)
target_include_directories(shadow_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(spike_shadow tests/spike_shadow.c)
target_link_libraries(spike_shadow PRIVATE permute_guest shadow_probe)

find_package(Threads REQUIRED)

add_executable(spike_baton tests/spike_baton.c)
target_link_libraries(spike_baton PRIVATE Threads::Threads)

add_executable(spike_determinism tests/spike_determinism.c)
target_link_libraries(spike_determinism PRIVATE permute_core)

add_executable(spike_hypercall tests/spike_hypercall.c)
target_link_libraries(spike_hypercall PRIVATE permute_guest)
