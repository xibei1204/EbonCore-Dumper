"""Command-line utilities for building an EbonCore C++ SDK from a runtime dump."""

from __future__ import annotations

import argparse
import fnmatch
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
from datetime import datetime

PROJECT_ROOT = Path(__file__).resolve().parents[1]
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

from tools.ebon.sdk_codegen import CodegenError, generate_sdk
from tools.ebon.cpp_sdk import generate_cpp_sdk
from tools.ebon.containers import ContainerError, PakFile, UtocFile
from tools.ebon.network_version import checked_network_version
from tools.ebon.resource_bundle import BundleLimits, dump_resources
from tools.ebon.resource_bodies import export_resource_bodies
from tools.ebon.oodle import OodleDecoder
from tools.ebon.feature_capture import audit_feature_capture
from tools.ebon.battle_assets import export_battle_assets


def _write_new(path: Path, value: dict) -> None:
    try:
        with path.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")
    except FileExistsError as exc:
        raise CodegenError(f"refusing to overwrite existing output: {path}") from exc


def inventory_resources(client: Path, output: Path, key_env: str) -> dict:
    client = client.resolve()
    output = output.resolve()
    if not client.is_dir():
        raise CodegenError(f"client directory does not exist: {client}")
    protected_root = next(
        (parent for parent in (client, *client.parents) if parent.name.casefold() == "neverness to everness"),
        client,
    )
    if output == protected_root or protected_root in output.parents:
        raise CodegenError("inventory output must be outside the read-only client directory")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise CodegenError(f"inventory output must be new or empty: {output}")
    pak_paths = sorted(client.rglob("*.pak"))
    toc_paths = sorted(client.rglob("*.utoc"))
    if not pak_paths and not toc_paths:
        raise CodegenError(f"no .pak or .utoc files found under {client}")
    key = os.environ.get(key_env)
    output.mkdir(parents=True, exist_ok=True)
    errors: list[dict] = []
    unresolved: list[dict] = []
    containers = 0
    pak_rows = 0
    toc_rows = 0

    streams = []
    try:
        for filename in (
            "resource_containers.jsonl",
            "pak_entries.jsonl",
            "iostore_chunks.jsonl",
            "iostore_directories.jsonl",
        ):
            streams.append((output / filename).open("x", encoding="utf-8", newline="\n"))
        inventory, pak_entries, toc_chunks, toc_directories = streams
        for path in [*pak_paths, *toc_paths]:
            relative = path.relative_to(client).as_posix()
            try:
                if path.suffix.casefold() == ".pak":
                    container = PakFile(path, key=key)
                    info = dict(container.info)
                    info["relative_path"] = relative
                    _write_new_line(inventory, info)
                    containers += 1
                    if info["encrypted_index"] and key is None:
                        unresolved.append({"container": relative, "reason": "encrypted_index_requires_verified_key"})
                        continue
                    for entry in container.entries():
                        row = entry.to_dict()
                        row["container"] = relative
                        _write_new_line(pak_entries, row)
                        pak_rows += 1
                else:
                    container = UtocFile(path, key=key)
                    info = dict(container.info)
                    info["relative_path"] = relative
                    ucas = path.with_suffix(".ucas")
                    info["ucas_exists"] = ucas.is_file()
                    info["ucas_size"] = ucas.stat().st_size if ucas.is_file() else None
                    _write_new_line(inventory, info)
                    containers += 1
                    for chunk in container.chunks():
                        chunk["container"] = relative
                        _write_new_line(toc_chunks, chunk)
                        toc_rows += 1
                    if info["indexed"] and info["directory_index_size"]:
                        try:
                            directories = container.directories()
                        except ContainerError as exc:
                            unresolved.append({"container": relative, "reason": str(exc)})
                        else:
                            for item in directories:
                                item["container"] = relative
                                _write_new_line(toc_directories, item)
            except (ContainerError, OSError) as exc:
                errors.append({"container": relative, "error": str(exc)})
        for stream in streams:
            stream.flush()
    except (ContainerError, OSError) as exc:
        errors.append({"error": str(exc)})
    finally:
        for stream in streams:
            stream.close()

    result = {
        "status": _inventory_status(errors, unresolved),
        "client": str(client),
        "container_count": containers,
        "pak_entry_count": pak_rows,
        "iostore_chunk_count": toc_rows,
        "unresolved_count": len(unresolved),
        "unresolved": unresolved,
        "aes_key_environment_variable": key_env if key is not None else None,
        "errors": errors,
        "limitations": [
            "Container inventories do not deserialize Unreal assets.",
            "IoStore chunk exports are raw chunks, not reconstructed .uasset/.uexp packages.",
            "No signature validation or completeness claim is made.",
        ],
    }
    _write_new(output / "resource_inventory_state.json", result)
    return result


def extract_pak(
    container_path: Path,
    output: Path,
    key_env: str,
    patterns: list[str],
    max_files: int,
    max_file_mb: int,
    max_total_mb: int,
    decompressor=None,
) -> dict:
    container_path = container_path.resolve()
    output = output.resolve()
    if not patterns:
        raise CodegenError("at least one --include pattern is required")
    if not 1 <= max_files <= 100_000 or not 1 <= max_file_mb <= 4096 or not 1 <= max_total_mb <= 32768:
        raise CodegenError("extraction limits are outside the supported bounds")
    if output.exists() and (not output.is_dir() or any(output.iterdir())):
        raise CodegenError(f"extraction output must be new or empty: {output}")
    install_root = next(
        (parent for parent in (container_path.parent, *container_path.parents) if parent.name.casefold() == "neverness to everness"),
        None,
    )
    if install_root and (output == install_root or install_root in output.parents):
        raise CodegenError("extraction output must be outside the read-only client installation")

    output.mkdir(parents=True, exist_ok=True)
    key = os.environ.get(key_env)
    extracted = 0
    bytes_written = 0
    attempted = 0
    unresolved: list[dict] = []
    errors: list[dict] = []
    matched = 0
    manifest_path = output / "pak_extraction.jsonl"
    with manifest_path.open("x", encoding="utf-8", newline="\n") as manifest:
        try:
            container = PakFile(container_path, key=key, decompressor=decompressor)
            entries = container.entries()
            for entry in entries:
                if not any(fnmatch.fnmatchcase(entry.path.casefold(), p.casefold()) for p in patterns):
                    continue
                matched += 1
                if entry.deleted:
                    unresolved.append({"path": entry.path, "reason": "deleted_pak_entry"})
                    continue
                if attempted >= max_files:
                    unresolved.append({"reason": "max_files_limit_reached"})
                    break
                if entry.uncompressed_size > max_file_mb * 1024 * 1024:
                    unresolved.append({"path": entry.path, "reason": "max_file_size_limit_reached"})
                    continue
                if bytes_written + entry.uncompressed_size > max_total_mb * 1024 * 1024:
                    unresolved.append({"reason": "max_total_size_limit_reached"})
                    break
                attempted += 1
                try:
                    destination = container.extract(entry, output, overwrite=False, max_bytes=max_file_mb * 1024 * 1024)
                except ContainerError as exc:
                    errors.append({"path": entry.path, "error": str(exc)})
                    if len(errors) >= 200:
                        unresolved.append({"reason": "error_report_limit_reached"})
                        break
                else:
                    bytes_written += entry.uncompressed_size
                    extracted += 1
                    _write_new_line(
                        manifest,
                        {"path": entry.path, "bytes": entry.uncompressed_size, "output": str(destination.relative_to(output))},
                    )
        except ContainerError as exc:
            unresolved.append({"container": str(container_path), "reason": str(exc)})

    result = {
        "status": _inventory_status(errors, unresolved),
        "container": str(container_path),
        "include_patterns": patterns,
        "matched": matched,
        "attempted": attempted,
        "extracted": extracted,
        "bytes_written": bytes_written,
        "key_environment_variable": key_env if key is not None else None,
        "unresolved": unresolved,
        "errors": errors,
        "limitations": ["Oodle-compressed entries require an external compatible decompressor."],
    }
    _write_new(output / "pak_extraction_state.json", result)
    return result


def _inventory_status(errors: list[dict], unresolved: list[dict]) -> str:
    """Errors dominate; unresolved containers still count as a usable inventory."""
    if errors:
        return "complete_with_errors"
    if unresolved:
        return "complete_with_unresolved"
    return "complete"


def _write_new_line(stream, value: dict) -> None:
    stream.write(json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n")


def capture_sdk(executable: Path, pid: int, profile: Path, dump: Path, sdk: Path | None) -> int:
    if not executable.is_file():
        raise CodegenError(f"native dumper executable does not exist: {executable}")
    if dump.exists() and any(dump.iterdir()):
        raise CodegenError(f"dump output must be a new or empty directory: {dump}")
    command = [str(executable), "--pid", str(pid), "--profile", str(profile), "--out", str(dump)]
    completed = subprocess.run(command, check=False)
    if completed.returncode:
        return completed.returncode
    if sdk is not None:
        result = generate_cpp_sdk(dump, sdk)
        print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


def inject_and_dump(
    injector: Path,
    dumper: Path,
    game: str,
    profile: Path,
    dump: Path,
    sdk: Path | None,
    seconds: int = 900,
    min_modules: int = 24,
    log_path: Path | None = None,
    injector_args: list[str] | None = None,
    wait_aes: bool = False,
    aes_wait_seconds: float = 180.0,
    wait_network_version: bool = False,
    network_version_wait_seconds: float = 600.0,
) -> int:
    if wait_aes and (not math.isfinite(aes_wait_seconds) or aes_wait_seconds <= 0):
        raise CodegenError("AES wait timeout must be positive and finite")
    if wait_network_version and (not math.isfinite(network_version_wait_seconds) or network_version_wait_seconds <= 0):
        raise CodegenError("NetworkVersion wait timeout must be positive and finite")
    if not injector.is_file():
        raise CodegenError(f"injector executable does not exist: {injector}")
    if not dumper.is_file():
        raise CodegenError(f"dumper DLL does not exist: {dumper}")
    if dump.exists() and any(dump.iterdir()):
        raise CodegenError(f"dump output must be a new or empty directory: {dump}")
    command = [
        str(injector),
        "--dumper",
        str(dumper),
        "--game",
        game,
        "--profile",
        str(profile),
        "--out",
        str(dump),
        "--seconds",
        str(seconds),
        "--min-modules",
        str(min_modules),
    ]
    if log_path:
        command.extend(["--log", str(log_path)])
    if injector_args:
        command.extend(injector_args)
    completed = subprocess.run(command, check=False)
    if completed.returncode:
        return completed.returncode
    # LoadLibrary completion is asynchronous: the injected worker may still be
    # discovering/collecting, and its first state file is a progress checkpoint.
    delay = 0
    if injector_args and "--capture-delay" in injector_args:
        delay = int(injector_args[injector_args.index("--capture-delay") + 1])
    state = wait_for_capture(dump, timeout=300 + delay)
    aes_state = wait_for_aes(dump, timeout=aes_wait_seconds) if wait_aes else None
    if not state.get("success"):
        print(json.dumps(state, ensure_ascii=False, indent=2), file=sys.stderr)
        return 1
    if wait_network_version:
        version=wait_for_network_version(dump,network_version_wait_seconds)
        print(json.dumps(version,ensure_ascii=False,indent=2))
    if sdk is not None:
        result = generate_cpp_sdk(dump, sdk)
        print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0 if aes_state is None or aes_state.get("success") else 1


def wait_for_aes(dump: Path, timeout: float = 180.0) -> dict:
    if not math.isfinite(timeout) or timeout <= 0:
        raise CodegenError("AES wait timeout must be positive and finite")
    deadline = time.monotonic() + timeout
    state_path = dump / "aes" / "aes_state.json"
    hotkey_path = dump / "aes_hotkey_state.json"
    last_status = None
    hotkey_hint = False
    while time.monotonic() < deadline:
        try:
            state = json.loads(state_path.read_text(encoding="utf-8"))
        except (FileNotFoundError, PermissionError, json.JSONDecodeError):
            # Hotkey mode keeps the scan disarmed until F9, so report the
            # listener state instead of waiting silently for a scan directory.
            if not hotkey_hint:
                try:
                    hotkey = json.loads(hotkey_path.read_text(encoding="utf-8"))
                except (FileNotFoundError, PermissionError, json.JSONDecodeError):
                    hotkey = None
                if isinstance(hotkey, dict) and hotkey.get("status") in {"armed", "running"}:
                    where = ("press F9 in the dumper terminal" if hotkey.get("terminal_available")
                             else "no dumper terminal; write aes_hunt.request")
                    print(f"AES: {hotkey['status']}; {where}; listener: {hotkey_path}", flush=True)
                    hotkey_hint = True
            time.sleep(0.25)
            continue
        if not isinstance(state, dict) or state.get("format") != "EbonAES-v1":
            raise CodegenError(f"invalid AES state: {state_path}")
        status = state.get("status")
        if status in {"verified", "timed_out", "exhausted", "error", "disabled", "cancelled"}:
            if status == "verified":
                key_path = state_path.with_name("aes_key.txt")
                try:
                    key = key_path.read_text(encoding="ascii").strip()
                    valid = key.startswith("0x") and len(key) == 66 and len(bytes.fromhex(key[2:])) == 32
                except (OSError, UnicodeError, ValueError):
                    valid = False
                if state.get("success") is not True or not valid or state.get("key") != key:
                    raise CodegenError("AES verified state does not match its key file")
            elif state.get("success") is not False:
                raise CodegenError("inconsistent AES terminal state")
            print(f"AES: {status}; result: {state_path}", flush=True)
            return state
        if status != last_status:
            print(f"AES: {status}; result: {state_path}", flush=True)
            last_status = status
        time.sleep(0.25)
    raise CodegenError(f"AES did not finish within {timeout:g} seconds; inspect {state_path}")


def wait_for_capture(dump: Path, timeout: float = 300.0) -> dict:
    deadline = time.monotonic() + timeout
    last_status = None
    while time.monotonic() < deadline:
        try:
            state = json.loads((dump / "dump_state.json").read_text(encoding="utf-8"))
        except (FileNotFoundError, PermissionError, json.JSONDecodeError):
            time.sleep(0.5)
            continue
        status = state.get("status")
        if status in {"complete", "complete_with_unresolved", "failed"}:
            return state
        if status != last_status:
            print(f"capture: {status} ({state.get('phase', 'initializing')})", flush=True)
            last_status = status
        time.sleep(0.5)
    raise CodegenError(f"capture did not finish within {timeout:g} seconds; inspect {dump / 'dump_state.json'} and the dumper console")


def wait_for_network_version(capture: Path, timeout: float = 600.0) -> dict:
    if not math.isfinite(timeout) or timeout <= 0:
        raise CodegenError("NetworkVersion wait timeout must be positive and finite")
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        try:version=checked_network_version(capture)
        except (FileNotFoundError,PermissionError,json.JSONDecodeError):
            time.sleep(0.25);continue
        except ValueError as error:raise CodegenError("invalid NetworkVersion evidence: "+str(error)) from error
        if version['verified']:
            print(f"NetworkVersion: {version['value']} (0x{version['value']:08x})",flush=True)
            return version
        if version['status'] in {'unsupported_profile','disabled','profile_mismatch','invalid_binding'}:
            raise CodegenError("NetworkVersion cannot be observed: "+version['status'])
        time.sleep(0.25)
    raise CodegenError(f"NetworkVersion not initialized/observed within {timeout:g} seconds; no guessed value was exported")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="ebon-dumper")
    commands = parser.add_subparsers(dest="command", required=True)
    feature_audit = commands.add_parser("verify-feature-capture", help="audit actual feature tables, instances and runtime menu RPC indices")
    feature_audit.add_argument("--capture", required=True, help="finished F10 resources/res_N directory")
    battle = commands.add_parser("export-battle-assets", help="preserve original package bytes for captured character/skill references")
    battle.add_argument("--capture", required=True)
    battle.add_argument("--client", required=True)
    battle.add_argument("--out", required=True)
    battle.add_argument("--key-env", default="EBON_AES_KEY")
    battle.add_argument("--oodle-dll")
    battle.add_argument("--oodle-sha256")
    sdk = commands.add_parser("generate-sdk", help="generate headers from a complete reflection dump")
    sdk.add_argument("--dump", required=True, help="directory containing dump_state.json and JSONL files")
    sdk.add_argument("--out", required=True, help="SDK output directory")
    sdk.add_argument("--format", choices=("cpp", "metadata"), default="cpp", help="real C++ declarations (default) or legacy reflection metadata")
    sdk.add_argument("--network-capture", help="separate same-build runtime snapshot for cross-checked network tables; full SDK layouts still use --dump")
    version=commands.add_parser("verify-network-version",help="revalidate saved getter/Hello/cache bytes")
    version.add_argument("--capture",required=True)
    version_wait=commands.add_parser("wait-network-version",help="wait for an initialized real UE checksum")
    version_wait.add_argument("--capture",required=True)
    version_wait.add_argument("--seconds",type=float,default=600)
    resources = commands.add_parser("inventory-resources", help="inventory Pak and IoStore containers")
    resources.add_argument("--client", required=True, help="read-only client installation directory")
    resources.add_argument("--out", required=True, help="new inventory output directory")
    resources.add_argument("--key-env", default="EBON_AES_KEY", help="environment variable containing a verified AES-256 key")
    bodies = commands.add_parser("export-resource-bodies", help="publish existing F10 bodies as separate files; keep JSONL indexes metadata-only")
    bodies.add_argument("--capture", required=True, help="finished F10 resources/res_N directory (partial failed captures are preserved as partial)")
    bodies.add_argument("--out", required=True, help="new output outside the original capture")
    bodies.add_argument("--max-files", type=int, default=100000)
    bodies.add_argument("--max-file-mb", type=int, default=512)
    bodies.add_argument("--max-total-mb", type=int, default=4096)
    bodies.add_argument("--max-manifest-mb", type=int, default=64)
    bodies.add_argument("--timeout-seconds", type=int, default=1800)
    bundle = commands.add_parser("dump-res", help="preserve loose scripts/hotfix, maps, Pak files and raw IoStore chunks")
    bundle.add_argument("--client", required=True, help="read-only installation or Content directory")
    bundle.add_argument("--out", required=True, help="new output outside the client/captures")
    bundle.add_argument("--capture", action="append", default=[], help="successful F10 resources/res_N directory; repeat to preserve snapshots")
    bundle.add_argument("--include", action="append", help="optional case-insensitive path glob; repeat (filters unnamed chunks too)")
    bundle.add_argument("--all", action="store_true", help="also preserve loose/Pak files with unknown extensions")
    bundle.add_argument("--key-env", default="EBON_AES_KEY")
    bundle.add_argument("--max-files", type=int, default=100000)
    bundle.add_argument("--max-file-mb", type=int, default=512)
    bundle.add_argument("--max-total-mb", type=int, default=32768)
    bundle.add_argument("--max-manifest-mb", type=int, default=512)
    bundle.add_argument("--timeout-seconds", type=int, default=1800)
    bundle.add_argument("--oodle-dll", help="explicit trusted local Windows x64 Oodle decoder DLL (executes DLL code)")
    bundle.add_argument("--oodle-sha256", help="optional expected DLL hash checked BEFORE loading")
    extract = commands.add_parser("extract-pak", help="extract selected Pak entries with bounded output")
    extract.add_argument("--container", required=True, help="input .pak file")
    extract.add_argument("--out", required=True, help="new output directory outside the client installation")
    extract.add_argument("--include", action="append", required=True, help="resource path glob; may be repeated")
    extract.add_argument("--key-env", default="EBON_AES_KEY", help="environment variable containing a verified AES-256 key")
    extract.add_argument("--max-files", type=int, default=1000)
    extract.add_argument("--max-file-mb", type=int, default=512)
    extract.add_argument("--max-total-mb", type=int, default=2048)
    extract.add_argument("--oodle-dll", help="explicit trusted local Windows x64 Oodle decoder DLL")
    extract.add_argument("--oodle-sha256", help="optional expected DLL hash checked before loading")
    capture = commands.add_parser("capture-sdk", help="capture reflection and optionally generate C++ metadata")
    capture.add_argument("--exe", default="build/EbonDumper.exe", help="native read-only dumper executable")
    capture.add_argument("--pid", type=int, required=True, help="target process ID")
    capture.add_argument("--profile", required=True, help="version-bound UE profile")
    capture.add_argument("--dump-out", required=True, help="new reflection capture directory")
    capture.add_argument("--sdk-out", help="optional separate SDK output directory")
    inject = commands.add_parser("inject-and-dump", help="watch for game launch, inject before ACE, and capture reflection")
    inject.add_argument("--injector", default="build/EbonDebugInject.exe", help="early injector executable")
    inject.add_argument("--dumper", default="build/EbonDumper.dll", help="internal dumper DLL")
    inject.add_argument("--game", default="HTGame.exe", help="game process name")
    inject.add_argument("--profile", default="profiles/nte-ue5.ini", help="version-bound UE profile")
    inject.add_argument("--dump-out", "--out", help="new reflection capture directory (default: timestamped out/reflection directory)")
    inject.add_argument("--sdk-out", help="optional separate SDK output directory")
    inject.add_argument("--no-sdk", action="store_true", help="capture reflection without generating C++ headers")
    inject.add_argument("--wait-aes", action="store_true", help="also wait for this capture's AES worker; require a verified key for exit 0")
    inject.add_argument("--wait-network-version",action="store_true",help="require a byte-revalidated initialized NetworkVersion before SDK generation/exit 0")
    inject.add_argument("--network-version-wait-seconds",type=float,default=600)
    inject.add_argument("--aes-wait-seconds", type=float, default=180, help="maximum additional AES wait time")
    inject.add_argument("--wait-ms", type=int, help="injector loader wait time")
    inject.add_argument("--capture-delay", type=int, default=0, help="delay reflection capture by 0..300 seconds for network initialization")
    inject.add_argument("--inject-after-ms", type=int, help="injector startup delay")
    inject.add_argument("--no-elevate", action="store_true", help="pass --no-elevate to the injector")
    inject.add_argument("--seconds", type=int, default=900, help="seconds to watch for game launch")
    inject.add_argument("--min-modules", type=int, default=24, help="minimum module count before injection")
    inject.add_argument("--log", help="path to injection log file")
    aes_wait = commands.add_parser("wait-aes", help="wait for a specific capture's AES result")
    aes_wait.add_argument("--capture", required=True)
    aes_wait.add_argument("--seconds", type=float, default=180)
    args = parser.parse_args(argv)

    try:
        if args.command == "verify-feature-capture":
            try:
                result = audit_feature_capture(Path(args.capture))
            except (ValueError, KeyError, TypeError) as error:
                raise CodegenError("Invalid feature capture: " + str(error)) from error
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 0 if result["success"] else 1
        if args.command in {'verify-network-version','wait-network-version'}:
            if args.command=='wait-network-version':result=wait_for_network_version(Path(args.capture),args.seconds)
            else:
                try:result=checked_network_version(Path(args.capture))
                except ValueError as error:raise CodegenError('Invalid NetworkVersion evidence: '+str(error)) from error
            print(json.dumps(result,ensure_ascii=False,indent=2));return 0 if result['verified'] else 1
        decoder = None
        if args.command in {"dump-res", "extract-pak", "export-battle-assets"}:
            if args.oodle_sha256 and not args.oodle_dll:
                raise ContainerError("--oodle-sha256 requires --oodle-dll")
            if args.oodle_dll:
                decoder = OodleDecoder(Path(args.oodle_dll), args.oodle_sha256)
        if args.command == "export-battle-assets":
            result = export_battle_assets(Path(args.capture), Path(args.client), Path(args.out),
                                          key=os.environ.get(args.key_env), decompressor=decoder)
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 0 if result["status"] == "preserved" else 1
        if args.command == "dump-res":
            if args.all and args.include:
                raise ContainerError("--all and --include are mutually exclusive")
            result = dump_resources(Path(args.client), Path(args.out), key=os.environ.get(args.key_env),
                                    decompressor=decoder, patterns=args.include, all_files=args.all,
                                    captures=[Path(path) for path in args.capture],
                                    limits=BundleLimits(args.max_files, args.max_file_mb, args.max_total_mb,
                                                        args.max_manifest_mb, args.timeout_seconds))
            print(json.dumps(result, ensure_ascii=False, indent=2))
            if not result["success"]:
                return 2
            return 0 if result["status"] == "complete" else 1
        if args.command == "export-resource-bodies":
            result = export_resource_bodies(Path(args.capture), Path(args.out),
                                           BundleLimits(args.max_files, args.max_file_mb, args.max_total_mb,
                                                        args.max_manifest_mb, args.timeout_seconds))
            print(json.dumps(result, ensure_ascii=False, indent=2))
            if not result["success"]:
                return 2
            return 0 if result["status"] == "complete" else 1
        if args.command == "wait-aes":
            return 0 if wait_for_aes(Path(args.capture), args.seconds).get("success") else 1
        if args.command == "generate-sdk":
            if args.format == "metadata" and args.network_capture:
                raise CodegenError("network-capture requires --format cpp")
            if args.format == "cpp":
                result = generate_cpp_sdk(args.dump, args.out, network_capture=args.network_capture)
            else:
                result = generate_sdk(args.dump, args.out)
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 0
        if args.command == "inventory-resources":
            result = inventory_resources(Path(args.client), Path(args.out), args.key_env)
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 1 if result["status"] == "complete_with_errors" else 0
        if args.command == "extract-pak":
            result = extract_pak(
                Path(args.container),
                Path(args.out),
                args.key_env,
                args.include,
                args.max_files,
                args.max_file_mb,
                args.max_total_mb,
                decompressor=decoder,
            )
            print(json.dumps(result, ensure_ascii=False, indent=2))
            return 1 if result["status"] == "complete_with_errors" else 0
        if args.command == "capture-sdk":
            sdk_out = Path(args.sdk_out).resolve() if args.sdk_out else None
            return capture_sdk(
                Path(args.exe).resolve(),
                args.pid,
                Path(args.profile).resolve(),
                Path(args.dump_out).resolve(),
                sdk_out,
            )
        if args.command == "inject-and-dump":
            capture_name = "injected_" + datetime.now().strftime("%Y%m%d_%H%M%S") + "_" + str(os.getpid())
            capture_out = Path(args.dump_out).resolve() if args.dump_out else PROJECT_ROOT / "out" / "reflection" / capture_name
            if args.no_sdk:
                sdk_out = None
            elif args.sdk_out:
                sdk_out = Path(args.sdk_out).resolve()
            else:
                sdk_out = PROJECT_ROOT / "out" / "sdk" / ("cpp_" + capture_out.name)
            native_args = []
            if not 0 <= args.capture_delay <= 300:
                raise CodegenError("capture-delay must be in 0..300 seconds")
            for key in ("wait_ms", "inject_after_ms", "capture_delay"):
                value = getattr(args, key)
                if value is not None:
                    native_args.extend(["--" + key.replace("_", "-"), str(value)])
            if args.no_elevate:
                native_args.append("--no-elevate")
            return inject_and_dump(
                Path(args.injector).resolve(),
                Path(args.dumper).resolve(),
                args.game,
                Path(args.profile).resolve(),
                capture_out,
                sdk_out,
                args.seconds,
                args.min_modules,
                Path(args.log).resolve() if args.log else None,
                native_args,
                args.wait_aes,
                args.aes_wait_seconds,
                args.wait_network_version,
                args.network_version_wait_seconds,
            )
    except (CodegenError, ContainerError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
