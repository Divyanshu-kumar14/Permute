# SPDX-License-Identifier: Apache-2.0
#
# Thin wrappers. Gate targets are named here once and reused everywhere.

.RECIPEPREFIX = >
BUILD ?= build
CMAKE ?= cmake

.PHONY: all build test unit codec replay guest allocprobe gate gatelint rng vendordup worker cli scenarios executor faults oracles shrink conformance bench-bugs-quick cov licence bootstrap determinism docs-check quickstart ci-check ci asan shadow baton hypercall symcheck negative clean help

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
> @./$(BUILD)/test_alloc
> @./$(BUILD)/test_shim
> @./$(BUILD)/test_rng_golden tests/golden/rng_vectors.tsv
> @./$(BUILD)/test_lease
> @./$(BUILD)/test_fault
> @./$(BUILD)/test_cycle

guest: build
> @python3 tools/make_guest.py --source tests/fixtures/guest/counter.c --name counter --instance 0 --out $(BUILD) --build-dir $(BUILD)
> @python3 tools/make_guest.py --source tests/fixtures/guest/counter.c --name counter --instance 1 --out $(BUILD) --build-dir $(BUILD)

codec: build
> @python3 tests/python/test_codec.py

replay: build
> @python3 tests/python/test_replay.py

allocprobe: build
> @python3 tests/python/test_alloc_probe.py

gate: build
> @python3 tools/symcheck.py

gatelint: build
> @python3 tests/python/test_determinism_gate.py

rng:
> @python3 tests/python/test_rng.py

vendordup: build
> @sh tools/vendor_dup_check.sh $(BUILD) $(CC)

worker: build
> @python3 tests/python/test_worker.py

cli: build
> @python3 tests/python/test_cli.py

scenarios: build
> @python3 tools/build_scenarios.py

executor: build scenarios
> @python3 tools/executor_check.py

faults: build scenarios
> @python3 tests/python/test_fault.py

oracles: build scenarios
> @python3 tests/python/test_oracle.py

shrink: build scenarios
> @python3 tests/python/test_shrink.py

conformance: build
> @python3 tests/python/test_conformance.py
> @python3 conformance/runner/run_conformance.py

conformance-record: build
> @python3 conformance/runner/run_conformance.py --record

bench-bugs: build
> @python3 tools/bench_bugs.py

bench-bugs-quick: build
> @python3 tools/bench_bugs.py --quick --jobs 4

licence:
> @python3 tools/licence_scan.py

determinism: build
> @python3 tools/determinism_check.py

cov: build
> @sh tools/cov.sh

docs-check:
> @python3 tools/docs_check.py

quickstart: build
> @python3 tools/quickstart_check.py

ci-check:
> @python3 tools/ci_check.py

ci: test docs-check quickstart licence determinism cov ci-check

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

bootstrap: build
> @python3 tools/determinism_check.py --seeds 8 --runs 4

hypercall: build
> @sh tools/hypercall_check.sh

symcheck: build
> @python3 tools/symcheck.py

negative:
> @sh tools/negative_link.sh

test: unit codec replay allocprobe gate gatelint rng vendordup worker cli scenarios executor faults oracles shrink conformance bench-bugs-quick docs-check shadow baton bootstrap hypercall symcheck negative

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
