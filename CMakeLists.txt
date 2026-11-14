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
add_library(permute_guest STATIC
  abi/shim_ipc.c
  abi/shim_proc.c
  abi/shim_pthread.c
  abi/shim_time.c
  abi/shim_sched.c
  abi/shim_rand.c
  guest/hypercall.c
)
target_include_directories(permute_guest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(permute_guest PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Simulated kernel core. Day 1: PRNG, decision domains, determinism bootstrap.
add_library(permute_core STATIC
  kernel/rng.c
  kernel/decisions.c
  kernel/determinism.c
  kernel/objects.c
  kernel/blob.c
  kernel/msg.c
  kernel/pulse.c
  kernel/readyq.c
  kernel/sched.c
  kernel/pi.c
  kernel/eventq.c
  kernel/vclock.c
  kernel/timeout.c
  kernel/timer.c
  kernel/fault.c
  kernel/oracle.c
  kernel/blake3.c
  kernel/trace.c
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

enable_testing()

add_executable(test_objects tests/unit/test_objects.c)
target_include_directories(test_objects PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_objects PRIVATE permute_core)
add_test(NAME objects COMMAND test_objects)

add_executable(test_ipc tests/unit/test_ipc.c)
target_include_directories(test_ipc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_ipc PRIVATE permute_core)
add_test(NAME ipc COMMAND test_ipc)

add_executable(test_pulse tests/unit/test_pulse.c)
target_include_directories(test_pulse PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_pulse PRIVATE permute_core)
add_test(NAME pulse COMMAND test_pulse)

add_executable(test_sched tests/unit/test_sched.c)
target_include_directories(test_sched PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_sched PRIVATE permute_core)
add_test(NAME sched COMMAND test_sched)

add_executable(test_pi tests/unit/test_pi.c)
target_include_directories(test_pi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_pi PRIVATE permute_core)
add_test(NAME pi COMMAND test_pi)

add_executable(test_time tests/unit/test_time.c)
target_include_directories(test_time PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_time PRIVATE permute_core)
add_test(NAME time COMMAND test_time)

add_executable(test_blake3 tests/unit/test_blake3.c)
target_include_directories(test_blake3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_blake3 PRIVATE permute_core)
add_test(NAME blake3 COMMAND test_blake3)

add_executable(trace_gen tools/trace_gen.c)
target_link_libraries(trace_gen PRIVATE permute_core)

add_executable(replay_fixture tools/replay_fixture.c kernel/scenario.c)
target_link_libraries(replay_fixture PRIVATE permute_core)

add_executable(test_guest tests/unit/test_guest.c guest/dlmain.c)
target_include_directories(test_guest PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_guest PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME guest COMMAND test_guest
         ${CMAKE_BINARY_DIR}/proc_counter__0.so
         ${CMAKE_BINARY_DIR}/proc_counter__1.so)

# The unit test calls the allocator explicitly so the test process keeps
# libc's malloc; the probe links the interposed build.
add_library(permute_alloc_prefixed OBJECT abi/alloc.c)
target_compile_definitions(permute_alloc_prefixed PRIVATE PERMUTE_ALLOC_PREFIX)
target_include_directories(permute_alloc_prefixed PUBLIC
                           ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(test_alloc tests/unit/test_alloc.c
               $<TARGET_OBJECTS:permute_alloc_prefixed>)
target_include_directories(test_alloc PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME alloc COMMAND test_alloc)

# The interposed probe relies on glibc internals; musl builds skip it.
option(PERMUTE_NO_INTERPOSED_ALLOC "Skip the glibc-only interposed allocator probe" OFF)
if(NOT PERMUTE_NO_INTERPOSED_ALLOC)
  add_executable(alloc_probe tools/alloc_probe.c abi/alloc.c)
  target_include_directories(alloc_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
add_executable(test_shim tests/unit/test_shim.c)
target_include_directories(test_shim PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_shim PRIVATE permute_guest)
add_test(NAME shim COMMAND test_shim)

add_executable(test_rng_golden tests/unit/test_rng_golden.c)
target_include_directories(test_rng_golden PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_rng_golden PRIVATE permute_core)
add_test(NAME rng COMMAND test_rng_golden
         ${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/rng_vectors.tsv)

add_library(vendor_dup STATIC tests/fixtures/vendor_dup/vendor.c)
target_include_directories(vendor_dup PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(permute_run
  worker/run.c
  kernel/scenario.c
  guest/hypercall.c
  guest/dlmain.c
  runtime/executor.c
  runtime/bridge.c
)
target_include_directories(permute_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(permute_run PRIVATE permute_core ${CMAKE_DL_LIBS})
set_target_properties(permute_run PROPERTIES OUTPUT_NAME permute-run)

add_executable(permute_worker worker/main.c worker/lease.c)
target_include_directories(permute_worker PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(permute_worker PROPERTIES OUTPUT_NAME permute-worker)

add_executable(test_fault tests/unit/test_fault.c)
target_include_directories(test_fault PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_fault PRIVATE permute_core)
add_test(NAME fault COMMAND test_fault)

add_executable(test_cycle tests/unit/test_cycle.c)
target_include_directories(test_cycle PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test_cycle PRIVATE permute_core)
add_test(NAME cycle COMMAND test_cycle)

add_executable(test_lease tests/unit/test_lease.c worker/lease.c)
target_include_directories(test_lease PRIVATE
                           ${CMAKE_CURRENT_SOURCE_DIR}
                           ${CMAKE_CURRENT_SOURCE_DIR}/tests)
add_test(NAME lease COMMAND test_lease)
