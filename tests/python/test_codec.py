# SPDX-License-Identifier: Apache-2.0
"""Cross-language test for the .pmt codec.

The C writer produces a file and prints the records it wrote; this test reads
the file with the Python codec and compares the decoded values. It then
truncates and corrupts copies and checks that the reader degrades instead of
crashing.
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from cli.permute.trace import blake3, codec  # noqa: E402

GEN = REPO / "build" / "trace_gen"

checks = 0
failures = 0


def check(cond, message):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
        print(f"FAIL {message}")


def run_gen(*args):
    result = subprocess.run([str(GEN), *map(str, args)], capture_output=True,
                            text=True)
    if result.returncode != 0:
        raise SystemExit(f"trace_gen failed: {result.stderr}")
    return result.stdout


def parse_gen(output):
    records = []
    digest = None
    count = None
    for line in output.splitlines():
        if line.startswith("{"):
            raw = json.loads(line)
            for key in ("blob", "event"):
                raw[key] = bytes.fromhex(raw[key]) if raw[key] else None
            records.append(raw)
        elif line.startswith("DIGEST "):
            digest = line.split()[1]
        elif line.startswith("RECORDS "):
            count = int(line.split()[1])
    return records, digest, count


def test_golden(tmp):
    out = run_gen(tmp / "golden.pmt", "golden")
    want, digest, count = parse_gen(out)
    tr = codec.read(tmp / "golden.pmt")

    check(tr.error is None, f"golden read error: {tr.error}")
    check(not tr.partial, "golden should be complete")
    check(tr.ok, "golden should verify")
    check(len(tr.records) == len(want) == 22, f"record count {len(tr.records)}")
    check(count == 22, f"generator count {count}")

    for i, (got, exp) in enumerate(zip(tr.records, want)):
        check(got["kind"] == exp["kind"], f"record {i} kind")
        check(got["seq"] == exp["seq"], f"record {i} seq")
        check(got["t_ns"] == exp["t_ns"], f"record {i} time")
        check(got["src"] == exp["src"], f"record {i} src")
        check(got["dst"] == exp["dst"], f"record {i} dst")
        check(got["obj"] == exp["obj"], f"record {i} obj")
        check(got["aux"] == exp["aux"], f"record {i} aux")
        check(got["blob"] == exp["blob"], f"record {i} blob")
        check(got["event"] == exp["event"], f"record {i} event")

    check(f"{tr.run_digest:016x}" == digest,
          f"run digest {tr.run_digest:016x} != {digest}")
    check(tr.final_hash is not None, "final hash present")
    check(tr.n_chunks == 1, f"chunk count {tr.n_chunks}")

    nd = codec.to_ndjson(tr).splitlines()
    check(len(nd) == 22, f"ndjson lines {len(nd)}")
    check(all(json.loads(line)["kind"] for line in nd), "ndjson parses")

    chrome = codec.to_chrome(tr)
    check(len(chrome["traceEvents"]) == 22, "chrome event count")
    check(chrome["traceEvents"][0]["name"] == "CHANNEL_CREATE", "chrome name")
    return tr


def test_bulk(tmp):
    out = run_gen(tmp / "bulk.pmt", "bulk", 5000)
    _, digest, count = parse_gen(out)
    tr = codec.read(tmp / "bulk.pmt")

    check(count == 5000, f"bulk generator count {count}")
    check(tr.error is None, f"bulk read error: {tr.error}")
    check(not tr.partial, "bulk should be complete")
    check(len(tr.records) == 5000, f"bulk records {len(tr.records)}")
    check(tr.n_chunks > 3, f"bulk chunks {tr.n_chunks} (expected several)")
    check(f"{tr.run_digest:016x}" == digest, "bulk run digest")
    check(all(r["blob"] == b"\x5a" * 64 for r in tr.records), "bulk blobs")
    # Average encoded size includes the 64-byte blob; the wire budget is about
    # the record overhead, so subtract the blob.
    overhead = (Path(tmp / "bulk.pmt").stat().st_size - codec.HEADER_SIZE
                - codec.FOOTER_SIZE) / 5000 - 64
    check(overhead < 24, f"average overhead {overhead:.1f} B/record")
    return tr


def test_truncation(tmp, full):
    raw = (Path(tmp) / "golden.pmt").read_bytes()
    cuts = [10, codec.HEADER_SIZE, 100, codec.HEADER_SIZE + 1000,
            codec.HEADER_SIZE + codec.CHUNK_SIZE - 1,
            codec.HEADER_SIZE + codec.CHUNK_SIZE,
            codec.HEADER_SIZE + codec.CHUNK_SIZE + 1,
            len(raw) - codec.FOOTER_SIZE - 1, len(raw) - 1]

    for cut in cuts:
        path = Path(tmp) / f"cut-{cut}.pmt"
        path.write_bytes(raw[:cut])
        tr = codec.read(path)
        check(tr.partial or tr.error is not None,
              f"cut {cut}: should be partial, got error={tr.error}")
        check(len(tr.records) <= len(full),
              f"cut {cut}: too many records {len(tr.records)}")
        for got, want in zip(tr.records, full):
            check(got["kind"] == want["kind"], f"cut {cut}: prefix kind")
            check(got["seq"] == want["seq"], f"cut {cut}: prefix seq")
            if got["blob"] is not None:
                check(got["blob"] == want["blob"], f"cut {cut}: prefix blob")


def test_corruption(tmp, bulk):
    raw = bytearray((Path(tmp) / "bulk.pmt").read_bytes())
    chunk = codec.HEADER_SIZE
    raw[chunk + codec.CHUNK_SIZE + 100] ^= 0xFF  # second chunk, records area
    path = Path(tmp) / "corrupt.pmt"
    path.write_bytes(raw)

    tr = codec.read(path)
    check(tr.corrupt, "corruption should be detected")
    check(tr.error is not None, "corruption should set an error")
    check(0 < len(tr.records) < len(bulk.records),
          f"records before the bad chunk survive ({len(tr.records)})")


def test_chain(tmp, bulk):
    raw = bytearray((Path(tmp) / "bulk.pmt").read_bytes())
    second = codec.HEADER_SIZE + codec.CHUNK_SIZE
    raw[second + 8] ^= 0x01  # prev_hash of chunk 1
    # Repair the CRC so the chain check, not the CRC, is what fires.
    body = raw[second:second + codec.CHUNK_SIZE - 4]
    raw[second + codec.CHUNK_SIZE - 4:second + codec.CHUNK_SIZE] = (
        codec.crc32c(bytes(body)).to_bytes(4, "little")
    )
    path = Path(tmp) / "chain.pmt"
    path.write_bytes(raw)

    tr = codec.read(path)
    check(not tr.chain_ok, "broken chain should be detected")
    check(tr.error is not None, "broken chain should set an error")


def test_blake3_vector():
    check(blake3.hash32(b"").hex() ==
          "af1349b9f5f9a1a6a0404dea36dcc949"
          "9bcb25c9adc112b7cc9a93cae41f3262",
          "blake3 empty vector")
    # 1025-byte vector, exercising the two-chunk tree.
    data = bytes(i % 251 for i in range(1025))
    check(blake3.hash32(data).hex() ==
          "d00278ae47eb27b34faecf67b4fe263f"
          "82d5412916c1ffd97c8cb7fb814b8444",
          "blake3 1025-byte vector")


def main():
    if not GEN.exists():
        raise SystemExit(f"missing {GEN}; run make build")

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        full = test_golden(tmp)
        bulk = test_bulk(tmp)
        test_truncation(tmp, full.records)
        test_corruption(tmp, bulk)
        test_chain(tmp, bulk)
        test_blake3_vector()

    print(f"test_codec: {checks} checks, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
