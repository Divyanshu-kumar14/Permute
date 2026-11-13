# SPDX-License-Identifier: Apache-2.0
#
# Thin wrappers. Gate targets are named here once and reused everywhere.

.RECIPEPREFIX = >
BUILD ?= build
CMAKE ?= cmake

.PHONY: all build test unit codec replay guest asan shadow baton determinism hypercall symcheck negative clean help

all: build

build:
> @$(CMAKE) -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
> @$(CMAKE) --build $(BUILD)

shadow: build
> @./$(BUILD)/spike_shadow

unit: build guest
> @./$(BUILD)/test_objects
> @./$(BUILD)/test_ipc
> @./$(BUILD)/test_pulse
> @./$(BUILD)/test_sched
> @./$(BUILD)/test_pi
> @./$(BUILD)/test_time
> @./$(BUILD)/test_blake3
> @./$(BUILD)/test_guest ./$(BUILD)/proc_counter__0.so ./$(BUILD)/proc_counter__1.so

guest: build
> @python3 tools/make_guest.py --source tests/fixtures/guest/counter.c --name counter --instance 0 --out $(BUILD) --build-dir $(BUILD)
> @python3 tools/make_guest.py --source tests/fixtures/guest/counter.c --name counter --instance 1 --out $(BUILD) --build-dir $(BUILD)

codec: build
> @python3 tests/python/test_codec.py

replay: build
> @python3 tests/python/test_replay.py

asan: guest
> @$(CMAKE) -S . -B $(BUILD)-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
>   -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
> @$(CMAKE) --build $(BUILD)-asan
> @./$(BUILD)-asan/test_objects
> @./$(BUILD)-asan/test_ipc
> @./$(BUILD)-asan/test_pulse
> @./$(BUILD)-asan/test_sched
> @./$(BUILD)-asan/test_pi
> @./$(BUILD)-asan/test_time
> @./$(BUILD)-asan/test_blake3
> @ASAN_OPTIONS=detect_leaks=0 ./$(BUILD)-asan/test_guest ./$(BUILD)/proc_counter__0.so ./$(BUILD)/proc_counter__1.so

baton: build
> @./$(BUILD)/spike_baton

determinism: build
> @python3 tools/seedrun.py

hypercall: build
> @sh tools/hypercall_check.sh

symcheck: build
> @sh tools/symcheck.sh

negative:
> @sh tools/negative_link.sh

test: unit codec replay shadow baton determinism hypercall symcheck negative

clean:
> @rm -rf $(BUILD)

help:
> @printf '%s\n' \
>   'make build      configure and build' \
>   'make unit       kernel object-model unit tests' \
>   'make asan       unit tests under ASan/UBSan' \
>   'make shadow     interposition spike' \
>   'make baton      handoff benchmark' \
>   'make symcheck   archive defines every symbol in the .def' \
>   'make negative   link without the archive must fail' \
>   'make test       shadow + baton + symcheck + negative' \
>   'make clean      remove build/'
