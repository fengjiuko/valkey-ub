# Copyright (c) Valkey Contributors
# SPDX-License-Identifier: BSD-3-Clause
"""Linux-only focused checks; no SDK, provider simulation or hardware claims."""

from pathlib import Path
import os
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / "build/urma-foundation"
src = root / "src"
build.mkdir(parents=True, exist_ok=True)
sources = ["urma_protocol.c", "urma_session.c", "zmalloc.c", "serverassert.c"]
tests = [src / "unit/test_urma_protocol.cpp", src / "unit/test_urma_session.cpp"]

def run(args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)

def compile_suite(name, protocol=None, session=None, sanitize=False):
    objects = []
    flags = ["-g", "-O1", "-fno-omit-frame-pointer", "-ffunction-sections", "-fdata-sections"]
    if sanitize:
        flags += ["-fsanitize=address,undefined"]
    for source in sources:
        path = src / source
        if source == "urma_protocol.c" and protocol:
            path = protocol
        if source == "urma_session.c" and session:
            path = session
        obj = build / (name + "-" + source + ".o")
        run(["gcc", "-std=gnu11", *flags, "-I", src, "-c", path, "-o", obj])
        objects.append(obj)
    exe = build / name
    run(["g++", "-std=c++17", *flags, "-Wall", "-Wextra", "-Werror", "-I", src,
         *tests, *objects, "-Wl,--gc-sections", "-lgtest_main", "-lgtest", "-pthread", "-o", exe])
    return exe

exe = compile_suite("sanitized-tests", sanitize=True)
environment = os.environ.copy()
environment["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
with (build / "sanitizer.log").open("w") as log:
    run([exe], env=environment, stdout=log, stderr=subprocess.STDOUT)
print("ASan/UBSan: PASS", flush=True)

mutations = [
    ("protocol-limit", "urma_protocol.c", "frame->payload_len <= max_payload", "1", "UrmaProtocolTest.*"),
    ("session-repost", "urma_session.c", "session->rx_frames - session->rx_reposted == session->slots",
     "session->rx_frames - session->rx_consumed == session->slots", "UrmaSessionTest.ConsumedButUnpostedSlotCannotBeOverwritten"),
]
for name, source, before, after, pattern in mutations:
    original = (src / source).read_text()
    assert original.count(before) == 1, name
    mutated = build / (name + ".c")
    mutated.write_text(original.replace(before, after, 1))
    exe = compile_suite("mutant-" + name,
                        protocol=mutated if source == "urma_protocol.c" else None,
                        session=mutated if source == "urma_session.c" else None)
    with (build / ("mutation-" + name + ".log")).open("w") as log:
        result = subprocess.run([str(exe), "--gtest_filter=" + pattern], stdout=log, stderr=subprocess.STDOUT)
    assert result.returncode == 1, (name, result.returncode)
    assert "[  FAILED  ]" in (build / ("mutation-" + name + ".log")).read_text(), name
    assert (src / source).read_text() == original
    print("Mutation rejected: " + name, flush=True)

exe = compile_suite("focused-tests")
with (build / "focused.log").open("w") as log:
    run([exe], stdout=log, stderr=subprocess.STDOUT)
print("Restored production suite: PASS", flush=True)
