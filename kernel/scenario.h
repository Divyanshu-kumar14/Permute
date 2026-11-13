// SPDX-License-Identifier: Apache-2.0
//
// One seeded scenario run. Used by permute-run and by the replay fixture.

#ifndef PERMUTE_SCENARIO_H
#define PERMUTE_SCENARIO_H

#include <stdint.h>

// Runs the scenario for `seed`, writes a trace to `trace_path`, and returns
// the run digest. Returns 0 on success; internal failure exits the process
// (the caller is a single-seed runner).
int permute_scenario_run(uint64_t seed, const char *trace_path,
                         uint64_t *digest_out, uint64_t *records_out);

#define PERMUTE_SCENARIO_CONFIG_HASH 0xC0FFEEull

#endif // PERMUTE_SCENARIO_H
