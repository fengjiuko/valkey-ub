# Copyright (c) Valkey Contributors
# SPDX-License-Identifier: BSD-3-Clause
"""CPU-only cache checks. Writes only build/remote-cache-check; no device I/O."""

import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / "build/remote-cache-check"
build.mkdir(parents=True, exist_ok=True)


def run(arguments, log, environment=None):
    with (build / log).open("w") as stream:
        subprocess.run([str(item) for item in arguments], check=True, env=environment,
                       stdout=stream, stderr=subprocess.STDOUT)


run(["make", "-C", root / "tests/urma", "memory-test", "OUT=" + str(build / "plain")], "plain.log")
environment = os.environ.copy()
environment["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
environment["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
run(["make", "-C", root / "tests/urma", "memory-test", "OUT=" + str(build / "sanitized"),
     "SANITIZER_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer"], "sanitizer.log", environment)
print("Pool/codec plain and ASan/UBSan/leak checks: PASS", flush=True)

mutations = [
    ("stale-version", "remote_cache", "version == request->version;", "1;",
     "RemoteCacheTest.LateRepliesCannotPublishAfterOverwriteDeleteOrRecreate"),
    ("corrupt-cache", "remote_value",
     "crc64(0, pool->arena + ref->slot * pool->slot_size, slot->length) != slot->checksum", "0",
     "RemoteValueTest.CorruptionIsAMissAndCacheCanBeRebuilt"),
    ("pinned-reuse", "remote_value", "slot->live || slot->pins ||", "slot->live ||",
     "RemoteValueTest.RetiredPinsPreventReuseAndPoolDestruction"),
]
objects = ["remote_value", "remote_cache", "crc64", "crcspeed", "crccombine", "zmalloc", "serverassert"]
for name, module, before, after, pattern in mutations:
    production = root / ("src/" + module + ".c")
    original = production.read_text()
    if original.count(before) != 1:
        raise RuntimeError("Mutation anchor changed: " + name)
    source = build / (name + ".c")
    source.write_text(original.replace(before, after, 1))
    obj = build / (name + ".o")
    run(["gcc", "-std=gnu11", "-g", "-O1", "-ffunction-sections", "-fdata-sections",
         "-I", root / "src", "-c", source, "-o", obj], name + "-compile.log")
    binary = build / name
    inputs = [obj if item == module else build / ("plain/" + item + ".o") for item in objects]
    run(["g++", "-std=c++17", "-I", root / "src", root / "src/unit/test_remote_value.cpp",
         root / "src/unit/test_remote_cache.cpp", *inputs, "-Wl,--gc-sections",
         "-lgtest_main", "-lgtest", "-pthread", "-o", binary], name + "-link.log")
    with (build / (name + ".log")).open("w") as stream:
        result = subprocess.run([str(binary), "--gtest_filter=" + pattern], stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode != 1 or "[  FAILED  ]" not in (build / (name + ".log")).read_text():
        raise RuntimeError("Mutation not rejected: " + name)
    if production.read_text() != original:
        raise RuntimeError("Production source changed")
    print("Mutation rejected: " + name, flush=True)
run([build / "plain/memory-tests"], "restored.log")
print("Unmodified production suite: PASS", flush=True)
