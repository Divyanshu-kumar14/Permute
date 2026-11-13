# SPDX-License-Identifier: Apache-2.0
#
# Thin wrappers. Gate targets are named here once and reused everywhere.

.RECIPEPREFIX = >
BUILD ?= build
CMAKE ?= cmake

.PHONY: all build test unit asan shadow baton determinism hypercall symcheck negative clean help

all: build

build:
> @$(CMAKE) -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
> @$(CMAKE) --build $(BUILD)

shadow: build
> @./$(BUILD)/spike_shadow

unit: build
> @./$(BUILD)/test_objects

asan:
> @$(CMAKE) -S . -B $(BUILD)-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
>   -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
> @$(CMAKE) --build $(BUILD)-asan
> @./$(BUILD)-asan/test_objects

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

test: unit shadow baton determinism hypercall symcheck negative

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
