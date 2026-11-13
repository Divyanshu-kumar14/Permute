# SPDX-License-Identifier: Apache-2.0
#
# Thin wrappers. Gate targets are named here once and reused everywhere.

.RECIPEPREFIX = >
BUILD ?= build
CMAKE ?= cmake

.PHONY: all build test shadow baton determinism hypercall symcheck negative clean help

all: build

build:
> @$(CMAKE) -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
> @$(CMAKE) --build $(BUILD)

shadow: build
> @./$(BUILD)/spike_shadow

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

test: shadow baton determinism hypercall symcheck negative

clean:
> @rm -rf $(BUILD)

help:
> @printf '%s\n' \
>   'make build      configure and build' \
>   'make shadow     interposition spike' \
>   'make baton      handoff benchmark' \
>   'make symcheck   archive defines every symbol in the .def' \
>   'make negative   link without the archive must fail' \
>   'make test       shadow + baton + symcheck + negative' \
>   'make clean      remove build/'
