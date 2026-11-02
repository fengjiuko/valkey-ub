#!/usr/bin/env python3
# Copyright (c) Valkey Contributors
# All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
"""Read-only prerequisites for Valkey's Linux, CPU-memory, builtin RDMA tests.

Uses only Python's standard library. No install, modprobe, network changes,
server startup, or RDMA connection. No inspected application binary is executed.
Only --output writes a new report file. No root privileges are required.
Exit: 0 = checked prerequisites passed; 2 = blocked; 1 = script/report error.
"""

import sys

sys.dont_write_bytecode = True

import argparse
import datetime
import gzip
import importlib.machinery
import ipaddress
import json
import os
from pathlib import Path
import platform
import re
import resource
import shlex
import shutil
import subprocess


def allowed_command(argv):
    """Deny mutations even if a future caller accidentally adds one."""
    if not argv:
        return False
    name, args = Path(argv[0]).name, argv[1:]
    if name in ("make", "cc", "gcc", "clang"):
        return args == ["--version"]
    if name == "git":
        return len(args) == 4 and args[0] == "-C" and args[2:] == ["rev-parse", "HEAD"]
    if name == "ldconfig":
        return args == ["-p"]
    if name == "pkg-config":
        return (len(args) == 2 and args[0] in ("--modversion", "--cflags-only-I")
                and args[1] in ("libibverbs", "librdmacm"))
    if name == "rdma":
        return args == ["link", "show"]
    if name == "ip":
        return args == ["-j", "address", "show"]
    if name == "ibv_devinfo":
        return (len(args) == 5 and args[0] == "-d" and args[2] == "-i"
                and args[4] == "-v" and bool(re.fullmatch(r"[A-Za-z0-9_.:-]+", args[1]))
                and args[3].isdigit() and int(args[3]) > 0)
    return False


def read_text(path):
    try:
        return Path(path).read_text(errors="replace").strip()
    except OSError:
        return ""


def command(argv, timeout=8):
    if not allowed_command(argv):
        return {"argv": argv, "exit_code": None, "stdout": "",
                "stderr": "Rejected: command is not an allowlisted read-only query"}
    try:
        environment = os.environ.copy()
        environment.update({"GIT_OPTIONAL_LOCKS": "0", "PYTHONDONTWRITEBYTECODE": "1",
                            "LC_ALL": "C"})
        proc = subprocess.run(argv, input="", capture_output=True, text=True,
                              errors="replace", timeout=timeout, env=environment)
        return {"argv": argv, "exit_code": proc.returncode,
                "stdout": proc.stdout.strip(), "stderr": proc.stderr.strip()}
    except (OSError, subprocess.TimeoutExpired) as exc:
        return {"argv": argv, "exit_code": None, "stdout": "", "stderr": str(exc)}


def kernel_config():
    candidates = [Path("/proc/config.gz"), Path("/boot/config-" + platform.release())]
    for path in candidates:
        try:
            if path.suffix == ".gz":
                with gzip.open(path, "rt", errors="replace") as stream:
                    content = stream.read()
            else:
                content = path.read_text(errors="replace")
            return str(path), content
        except OSError:
            continue
    return None, ""


def config_value(content, name):
    match = re.search(r"^" + re.escape(name) + r"=(\w+)$", content, re.MULTILINE)
    if match:
        return match.group(1)
    if "# " + name + " is not set" in content:
        return "disabled"
    return "unknown"


def directory_entries(path):
    """Keep permission denied distinct from an empty directory."""
    try:
        return list(Path(path).iterdir()), None
    except OSError as exc:
        return [], type(exc).__name__ + ": " + str(exc)


def device_access(path):
    try:
        Path(path).stat()
    except FileNotFoundError:
        return "missing"
    except OSError as exc:
        return "unavailable: " + str(exc)
    if os.access(path, os.R_OK | os.W_OK, effective_ids=True):
        return "accessible"
    return "permission_denied"


def selected_uverbs(device):
    nodes, error = directory_entries("/sys/class/infiniband_verbs")
    selected = []
    for node in nodes:
        if node.name.startswith("uverbs") and read_text(node / "ibdev") == device:
            selected.append(Path("/dev/infiniband") / node.name)
    return selected, error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", required=True, type=Path, help="Valkey source directory")
    parser.add_argument("--stage", choices=("build", "run", "all"), default="all",
                        help="build: dependencies; run: runtime; all: both (default)")
    parser.add_argument("--device", help="RDMA device, e.g. mlx5_0 or rxe_valkeyrdma0")
    parser.add_argument("--ib-port", type=int, default=1, help="RDMA device port, not listener port")
    parser.add_argument("--bind-ip", help="Planned local server RDMA IP; required for run/all")
    parser.add_argument("--output", type=Path, help="Write JSON to a NEW file (no overwrite)")
    parser.add_argument("--query-verbs", action="store_true",
                        help="Opt in to ibv_devinfo: opens a short-lived device context to query it; no QP/MR/CM or traffic")
    args = parser.parse_args()
    if args.ib_port < 1 or (args.device and not re.fullmatch(r"[A-Za-z0-9_.:-]+", args.device)):
        parser.error("invalid device or --ib-port")
    if args.bind_ip:
        try:
            bind_ip = ipaddress.ip_address(args.bind_ip)
        except ValueError:
            parser.error("--bind-ip must be an IP address without interface suffix")
    else:
        bind_ip = None

    repo = args.repo.resolve()
    build = args.stage in ("build", "all")
    runtime = args.stage in ("run", "all")
    report = {
        "schema_version": 2,
        "checked_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "scope": "Valkey Linux CPU-memory builtin Verbs/librdmacm prerequisites",
        "stage": args.stage,
        "repo": str(repo),
        "manifest": {"os": platform.platform(), "kernel": platform.release(),
                     "architecture": platform.machine(), "gpu": "N/A (CPU-memory smoke)",
                     "backend": "Verbs/librdmacm", "provider_runtime_verified": False},
        "checks": [],
        "evidence": {},
        "limitations": [
            "Preflight only: no RDMA connection or data integrity test was executed.",
            "Configured providers and a selected device do not prove the application's actual path.",
            "No physical NIC, GPU Direct, EFA, native URMA, RoUB or UMS compatibility is certified.",
        ],
    }

    def check(name, status, detail, action=""):
        report["checks"].append({"name": name, "status": status,
                                 "detail": detail, "action": action})

    def probe(name, argv):
        result = command(argv)
        report["evidence"][name] = result
        return result

    linux = sys.platform.startswith("linux")
    check("linux", "pass" if linux else "block", sys.platform,
          "Run inside Linux with a working RDMA subsystem, not native Windows.")
    source_ok = (repo / "src/rdma.c").is_file() and (repo / "tests/rdma/rdma-test.c").is_file()
    check("valkey_source", "pass" if source_ok else "block", str(repo),
          "Point --repo to a Valkey checkout containing the RDMA sources.")
    if shutil.which("git") and source_ok:
        sha = probe("source_sha", ["git", "-C", str(repo), "rev-parse", "HEAD"])
        report["manifest"]["source_sha"] = sha["stdout"] if sha["exit_code"] == 0 else None

    if linux:
        report["manifest"]["identity"] = {"uid": os.getuid(), "euid": os.geteuid(),
                                           "gid": os.getgid(), "groups": os.getgroups()}
        check("execution_identity", "info", "euid=" + str(os.geteuid()) + "; no privilege escalation")
        soft, hard = resource.getrlimit(resource.RLIMIT_MEMLOCK)
        report["manifest"]["memlock_bytes"] = {"soft": soft, "hard": hard}
        check("memlock", "info", "soft=" + str(soft) + ", hard=" + str(hard),
              "Recorded only; per-connection registration needs must be checked during a separate test.")
        report["manifest"]["os_release"] = read_text("/etc/os-release")
        config_path, config = kernel_config()
        symbols = {name: config_value(config, name) for name in
                   ("CONFIG_INFINIBAND", "CONFIG_INFINIBAND_USER_ACCESS",
                    "CONFIG_INFINIBAND_ADDR_TRANS", "CONFIG_RDMA_RXE")}
        report["manifest"]["kernel_config"] = {"source": config_path, "symbols": symbols}
        if symbols["CONFIG_INFINIBAND"] == "disabled":
            check("rdma_kernel", "block" if runtime else "warn", "CONFIG_INFINIBAND disabled",
                  "Use an RDMA-enabled Linux kernel/host; userspace packages cannot fix this.")
        elif config_path:
            check("rdma_kernel", "info", json.dumps(symbols),
                  "Config values alone do not prove modules are loaded or devices accessible.")
        else:
            check("rdma_kernel", "warn", "Kernel config unreadable; capability unknown.")

        ldconfig = shutil.which("ldconfig")
        if not ldconfig:
            ldconfig = next((path for path in ("/sbin/ldconfig", "/usr/sbin/ldconfig")
                             if os.access(path, os.X_OK)), None)
        cache = probe("library_cache", [ldconfig, "-p"])["stdout"] if ldconfig else ""
        for library in ("ibverbs", "rdmacm"):
            matches = re.findall(r"\blib" + library + r"\.so[^\s]*[^\n]*=>\s*(\S+)", cache)
            paths = [Path(path) for path in matches]
            if not paths:
                for directory in ("/lib", "/lib64", "/usr/lib", "/usr/lib64", "/usr/local/lib"):
                    for pattern in ("lib" + library + ".so*", "*/lib" + library + ".so*"):
                        paths.extend(Path(directory).glob(pattern))
            soname = next((str(path) for path in paths if path.is_file()), None)
            report["manifest"]["lib" + library] = soname
            check("library_" + library, "pass" if soname else "block",
                  soname or "Shared library not discoverable",
                  "Install/locate the matching rdma-core runtime and development packages.")

        if build:
            for name, alternatives in (("make", ("make",)), ("compiler", ("cc", "gcc", "clang"))):
                executable = next((shutil.which(item) for item in alternatives if shutil.which(item)), None)
                check(name, "pass" if executable else "block", executable or "Not found",
                      "Install a Linux C build toolchain.")
                if executable:
                    probe(name + "_version", [executable, "--version"])
            pkgconfig = shutil.which("pkg-config")
            check("pkg_config", "pass" if pkgconfig else "warn", pkgconfig or "Not found",
                  "pkg-config improves dependency discovery; Valkey Makefiles also have linker fallbacks.")
            include_dirs = [Path("/usr/include"), Path("/usr/local/include")]
            if pkgconfig:
                for package in ("libibverbs", "librdmacm"):
                    probe(package + "_version", [pkgconfig, "--modversion", package])
                    flags = probe(package + "_includes", [pkgconfig, "--cflags-only-I", package])
                    include_dirs.extend(Path(flag[2:]) for flag in shlex.split(flags["stdout"])
                                        if flag.startswith("-I"))
            for header in ("infiniband/verbs.h", "rdma/rdma_cma.h"):
                found = next((str(root / header) for root in include_dirs if (root / header).is_file()), None)
                check("header_" + header, "pass" if found else "block", found or "Not found",
                      "Install libibverbs/librdmacm development headers; custom compiler paths need manual verification.")

        if runtime:
            for name in ("rdma", "ip", "timeout"):
                executable = shutil.which(name)
                check("tool_" + name, "pass" if executable else "block", executable or "Not found",
                      "Install rdma-core/ibverbs-utils/iproute2/coreutils as appropriate.")
            query_tool = shutil.which("ibv_devinfo")
            check("tool_ibv_devinfo", "pass" if query_tool else ("block" if args.query_verbs else "warn"),
                  query_tool or "Not found (optional unless --query-verbs is requested)")
            if shutil.which("rdma"):
                result = probe("rdma_links", ["rdma", "link", "show"])
                check("rdma_netlink", "pass" if result["exit_code"] == 0 else "block",
                      result["stdout"] or result["stderr"], "Check kernel support and namespace permissions.")

            device_entries, device_error = directory_entries("/sys/class/infiniband")
            devices = sorted(path.name for path in device_entries)
            report["evidence"]["device_enumeration_error"] = device_error
            report["manifest"]["available_devices"] = devices
            device = args.device or (devices[0] if len(devices) == 1 else None)
            valid_device = bool(device and device in devices)
            check("selected_device", "pass" if valid_device else "block",
                  device_error or device or ("No RDMA devices" if not devices else "Multiple devices: " + ", ".join(devices)),
                  "Expose an RDMA device and specify --device when there is more than one.")
            report["manifest"]["selected_device"] = device
            netdevs = set()
            if valid_device:
                base = Path("/sys/class/infiniband") / device
                port = base / "ports" / str(args.ib_port)
                state = read_text(port / "state")
                check("port_active", "pass" if state.startswith("4:") else "block",
                      state or "Port state unavailable", "Check link state, port number and network setup.")
                report["manifest"]["device"] = {
                    "name": device, "port": args.ib_port, "state": state,
                    "link_layer": read_text(port / "link_layer"),
                    "firmware": read_text(base / "fw_ver"),
                    "kernel_driver": str((base / "device/driver").resolve()),
                }
                netdevs.update(read_text(path) for path in (port / "gid_attrs/ndevs").glob("*"))
                netdevs.discard("")
                if not netdevs and len(list((base / "ports").glob("*"))) == 1:
                    netdevs.update(path.name for path in (base / "device/net").glob("*"))
                report["manifest"]["device"]["netdevs"] = sorted(netdevs)
                report["manifest"]["device"]["gids"] = {
                    path.name: read_text(path) for path in (port / "gids").glob("*")}
                if args.query_verbs and shutil.which("ibv_devinfo"):
                    result = probe("verbs_device_query", ["ibv_devinfo", "-d", device, "-i", str(args.ib_port), "-v"])
                    check("verbs_device_query", "pass" if result["exit_code"] == 0 else "block",
                          result["stdout"] or result["stderr"], "Check provider loading and uverbs permissions.")

            if not args.query_verbs:
                check("verbs_device_query", "info", "Not executed (passive default).",
                      "--query-verbs opts in to a temporary userspace query context, with no QP/MR or traffic.")
            uverbs, mapping_error = selected_uverbs(device) if valid_device else ([], None)
            access = {str(path): device_access(path) for path in uverbs}
            report["evidence"]["selected_uverbs_access"] = access
            usable = [path for path, status in access.items() if status == "accessible"]
            check("uverbs_access", "pass" if usable else "block", ", ".join(usable) or "No accessible uverbs node",
                  "No access to the SELECTED device: " + str(access or mapping_error or "mapping unavailable")
                  + "; ask the operator to review availability/permissions; this script changes nothing.")
            cm = Path("/dev/infiniband/rdma_cm")
            cm_access = device_access(cm)
            check("rdma_cm_access", "pass" if cm_access == "accessible" else "block",
                  str(cm) + ": " + cm_access,
                  "Ask the operator to review CM availability/permissions; no chmod/chown or elevation is attempted.")
            report["manifest"]["provider_configs"] = {
                str(path): read_text(path) for path in Path("/etc/libibverbs.d").glob("*.driver")}
            check("provider_identity", "warn", "Kernel driver and provider configuration are recorded separately.",
                  "Record exact userspace provider/version and prove the application-selected path in the smoke test.")

            address_ok = False
            if bind_ip and not (bind_ip.is_loopback or bind_ip.is_unspecified) and shutil.which("ip"):
                result = probe("local_addresses", ["ip", "-j", "address", "show"])
                if result["exit_code"] == 0:
                    try:
                        for interface in json.loads(result["stdout"]):
                            for address in interface.get("addr_info", []):
                                if (interface.get("ifname") in netdevs
                                        and ipaddress.ip_address(address["local"]) == bind_ip):
                                    address_ok = True
                    except (ValueError, KeyError, TypeError) as exc:
                        check("address_parse", "warn", str(exc))
            check("bind_ip_mapping", "pass" if address_ok else "block",
                  str(bind_ip) if bind_ip else "No --bind-ip provided",
                  "Provide a local address mapped to the selected RDMA device; unsupported mappings require manual verification.")
            report["manifest"]["bind_ip"] = str(bind_ip) if bind_ip else None

            spec = importlib.machinery.PathFinder.find_spec("netifaces", sys.path)
            check("upstream_python_netifaces", "info" if spec else "warn",
                  (str(spec.origin) + " (found, not imported)") if spec else "Not discoverable",
                  "Metadata discovery only; runtest-rdma needs an actual import, which is outside this preflight.")
            for relative in ("src/valkey-server", "tests/rdma/rdma-test"):
                binary = repo / relative
                executable = binary.is_file() and os.access(binary, os.X_OK)
                check("binary_" + relative, "pass" if executable else "block", str(binary),
                      "Build with make BUILD_RDMA=yes and make -C tests/rdma before the run-stage check.")
            check("application_execution", "info", "Never execute inspected binaries; file/permission checks only.")
            settings = read_text(repo / "src/.make-settings")
            match = re.search(r"^BUILD_RDMA=(.*)$", settings, re.MULTILINE)
            check("builtin_build_evidence", "info" if match and match.group(1) == "yes" else "warn",
                  match.group(0) if match else "No Makefile BUILD_RDMA record",
                  "Build records may be stale; RDMA capability still requires an actual RDMA listener/connection test.")

    blockers = sum(item["status"] == "block" for item in report["checks"])
    report["status"] = "environment_blocked" if blockers else "preflight_pass"
    report["exit_code"] = 2 if blockers else 0
    for item in report["checks"]:
        print("[{status}] {name}: {detail}".format(**item))
        if item["action"] and item["status"] in ("block", "warn"):
            print("  -> " + item["action"])
    print("\nRESULT: " + report["status"] + " (blockers=" + str(blockers) + ")")
    print("This is prerequisite evidence, not an RDMA compatibility/performance result.")
    if args.output:
        descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
        print("JSON: " + str(args.output.resolve()))
    return report["exit_code"]


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print("Preflight error: " + str(error), file=sys.stderr)
        sys.exit(1)
