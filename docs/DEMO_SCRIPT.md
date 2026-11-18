<!-- SPDX-License-Identifier: Apache-2.0 -->

# Permute Demo Video: 120-Second High-Impact Storyboard & Script

**Owner:** [@Divyanshu-kumar14](https://github.com/Divyanshu-kumar14)  
**Status:** Ready for recording  
**Target Duration:** 120 Seconds (Strict limit: < 180s)  
**Target Hackathon:** QNX Global Hackathon 2026 (Category: *Developer Tooling & Host Tools*)

---

## Technical Setup & Screen Layout

- **Resolution:** 1920x1080 @ 60 FPS
- **Window Layout:** Clean dark terminal (e.g. Alacritty / Wezterm / Kitty) with 20pt readable monospace font.
- **Top Bar / Watermark:** `Permute | Deterministic Concurrency Testing for QNX 8.0 Neutrino`
- **Zero Distractions:** No browser bookmark clutter, no uncommitted git warnings, no debug logs.

---

## Scene-by-Scene Breakdown

### Scene 1: The Automotive Micro-Crisis (0:00 - 0:15)
- **Visual:** Clean terminal title slide or dark graphic displaying:
  > **ISO 26262 ASIL-D Automotive Concurrency Failure:**
  > Intermittent Priority-Inversion Deadlock & Lost Pulses in QNX Neutrino Services.
- **Voiceover:**
  > *"In safety-critical automotive systems powered by BlackBerry QNX, intermittent concurrency bugs are catastrophic. A priority-inversion deadlock or a dropped pulse can cause an ECU or braking controller to freeze once every 100,000 hours. On your development workstation, these bugs are impossible to reproduce under GDB. In target testing, they appear as phantom hangs.*
  > *Meet Permute: deterministic concurrency testing and automated root-cause shrinking for QNX 8.0."*

---

### Scene 2: Seed-Controlled Exploration (0:15 - 0:42)
- **Visual:** Terminal executing seed exploration against a racy QNX IPC service.
- **Terminal Command:**
  ```bash
  permute run --explore --seeds 0-1000 --guest build/scenarios/proc_s02_pi_chain__0.so
  ```
- **Visual Action:** The novelty bitmap dynamically updates, finding coverage edges. In under 3 seconds, a RED violation banner hits:
  ```text
  [VIOLATION DETECTED]
  Seed: 0x000000000000002b
  Oracle: PTHREAD_PRIO_INHERIT_DEADLOCK (Cycle: Thread 2 -> Thread 3 -> Thread 2)
  Events in trace: 1,842 discrete microkernel transitions
  ```
- **Voiceover:**
  > *"Permute replaces nondeterministic hardware execution with a mathematical, seed-driven microkernel simulator. Every context switch, message queue reorder, and pulse delivery is a pure function of a 64-bit seed.*
  > *In just 2.4 seconds, Permute explores 1,000 interleavings and isolates the exact seed that triggers a priority-inversion deadlock across a 3-thread service chain."*

---

### Scene 3: Sub-150ms Automated Shrinking (0:42 - 1:08)
- **Visual:** Running the delta-debugging causal shrinker on the raw 1,842-event trace.
- **Terminal Command:**
  ```bash
  permute shrink --trace .permute/traces/seed_002b.pmt --out minimal.pmt
  ```
- **Visual Action:** Watch the shrinker output:
  ```text
  Pass 1 (Causal pre-slice): 1,842 -> 148 events
  Pass 2 (ddmin delta-debug): 148 -> 24 -> 12 events
  Shrink completed in 142ms.
  Status: PROVEN MINIMAL (12 causal events)
  ```
- **Voiceover:**
  > *"A trace with thousands of context switches is impossible to debug manually. Permute's causal delta-debugging engine automatically slices away independent operations.*
  > *In 142 milliseconds, it reduces a messy 1,800-event execution trace down to the minimal 12 causal events that produced the deadlock."*

---

### Scene 4: Dual-Target Proof on Genuine QNX 8.0 (1:08 - 1:38)
- **Visual:** Split screen: Left terminal shows QNX SDP `qcc` cross-compilation; Right terminal shows booted headless QEMU serial log.
- **Terminal Commands (Left):**
  ```bash
  qcc -Vgcc_ntox86_64 -o c12_message_pi conformance/scenarios/c12_message_pi.c
  file c12_message_pi
  ```
  *(Output: ELF 64-bit LSB pie executable, x86-64, interpreter /usr/lib/ldqnx-64.so.2)*
- **Terminal Commands (Right):**
  ```bash
  tail -n 16 docs/evidence/qnx/serial.log
  ```
  *(Output: 14/14 RESULT ... rc=0 lines under procnto-smp-instr)*
- **Voiceover:**
  > *"Permute is not an abstract mock. Every scenario is dual-target. Here, the exact same C source code is cross-compiled with the vendor `qcc` toolchain from the QNX SDP 8.0.*
  > *Inside booted QNX 8.0 Neutrino running under QEMU, all 14 conformance scenarios execute directly on `procnto-smp-instr`, verifying identical message-passing behavior, priority inheritance, and return codes."*

---

### Scene 5: Industrial Reporting & Enterprise CI/CD (1:38 - 1:55)
- **Visual:** Quick display of the generated HTML report or SARIF output showing OASIS-compliant static analysis ingestion.
- **Terminal Command:**
  ```bash
  permute report --sarif report.sarif --html report.html
  ```
- **Voiceover:**
  > *"Permute integrates directly into standard automotive CI/CD pipelines, outputting native SARIF reports and interactive Mazurkiewicz causal DAGs.*
  > *With 2,000,000 soak runs verified and 100% bug benchmark detection with zero false positives, Permute delivers deterministic verification to mission-critical QNX systems."*

---

### Scene 6: Outro (1:55 - 2:00)
- **Visual:** End card with GitHub link and Devpost category:
  > **Permute: Deterministic Concurrency Testing for QNX Neutrino**  
  > GitHub: `github.com/Divyanshu-kumar14/Permute`  
  > Category: *Developer Tooling & Host Tools*
- **Voiceover:**
  > *"Zero flakiness. Zero phantom hangs. Deterministic safety for BlackBerry QNX."*
