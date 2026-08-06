#!/usr/bin/env python3
"""Clean-install and visually verify every supported Dead Space donor.

The test never reuses an extracted game tree.  Each repetition starts from the
immutable donor archive, runs eapx into a temporary directory, drives the game
to the first playable scene and evaluates three real framebuffer captures.
Only standard-library modules are used so the same script works on a clean Mac.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import uuid
import zlib


HERE = Path(__file__).resolve().parent
PORT_DIR = HERE.parent
PROJECT_DIR = PORT_DIR.parent
CONFIG_PATH = HERE / "compatibility_donors.json"
RECIPE = PORT_DIR / "ports/deadspace/deadspace.eapx.json"
EAPX = PORT_DIR / "tools/eapx.py"
RUNNER = HERE / "run.sh"
GOLDEN_DIR = HERE / "goldens"
RUNTIME_ROOT = HERE / "runtime/compatibility"

# Donors live in one tree shared by every port, outside the code, so the same
# APK is not duplicated per port and third-party material never sits next to
# what gets published. Paths in the config are relative to this root.
DONOR_ROOT = Path(
    os.environ.get("HANDHELD_DONORS", Path.home() / "Archive/handheld/donors")
).expanduser()

TEXTURE_RE = re.compile(
    r"texture summary atc_decoded=(\d+) pvrtc_native=(\d+) "
    r"pvrtc_decoded=(\d+) rgba=(\d+) subimage=(\d+) passthrough=(\d+) "
    r"failed=(\d+) gl_errors=(\d+)"
)
SUMMARY_RE = re.compile(r"summary assets=(\d+) textures=(\d+) draws=(\d+)")
FRAMES_RE = re.compile(r"frames=(\d+)")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def run_checked(argv: list[str], **kwargs) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(argv, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, **kwargs)
    if result.returncode != 0:
        raise RuntimeError("command failed (%d): %s\n%s" % (
            result.returncode, " ".join(argv), result.stdout[-8000:]))
    return result


def unpack_7z(source: Path, destination: Path) -> Path:
    executable = shutil.which("7zz") or shutil.which("7z")
    if not executable:
        raise RuntimeError("7zz is required on the host for the original Full donor")
    run_checked([executable, "x", "-y", "-o%s" % destination, str(source)])
    return destination


def read_png(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("not a PNG: %s" % path)
    pos, width, height, color, depth = 8, 0, 0, None, None
    compressed = bytearray()
    while pos + 12 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        kind = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color = struct.unpack(">IIBB", payload[:10])
        elif kind == b"IDAT":
            compressed.extend(payload)
        elif kind == b"IEND":
            break
    if depth != 8 or color not in (2, 6):
        raise ValueError("unsupported PNG format in %s" % path)
    channels = 4 if color == 6 else 3
    raw = zlib.decompress(bytes(compressed))
    stride = width * channels
    previous = bytearray(stride)
    rgba = bytearray(width * height * 4)
    offset = 0
    for y in range(height):
        filter_type = raw[offset]
        offset += 1
        scan = bytearray(raw[offset:offset + stride])
        offset += stride
        for x in range(stride):
            left = scan[x - channels] if x >= channels else 0
            up = previous[x]
            upper_left = previous[x - channels] if x >= channels else 0
            if filter_type == 1:
                scan[x] = (scan[x] + left) & 255
            elif filter_type == 2:
                scan[x] = (scan[x] + up) & 255
            elif filter_type == 3:
                scan[x] = (scan[x] + ((left + up) // 2)) & 255
            elif filter_type == 4:
                p = left + up - upper_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upper_left)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else upper_left)
                scan[x] = (scan[x] + predictor) & 255
            elif filter_type != 0:
                raise ValueError("unsupported PNG filter %d" % filter_type)
        for x in range(width):
            src = x * channels
            dst = (y * width + x) * 4
            rgba[dst:dst + 3] = scan[src:src + 3]
            rgba[dst + 3] = scan[src + 3] if channels == 4 else 255
        previous = scan
    return width, height, bytes(rgba)


def luma_values(image: tuple[int, int, bytes], roi: list[int]) -> tuple[int, int, list[float]]:
    width, height, rgba = image
    x0, y0, x1, y1 = roi
    x0, x1 = max(0, x0), min(width, x1)
    y0, y1 = max(0, y0), min(height, y1)
    values = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            at = (y * width + x) * 4
            r, g, b = rgba[at], rgba[at + 1], rgba[at + 2]
            values.append(0.2126 * r + 0.7152 * g + 0.0722 * b)
    return x1 - x0, y1 - y0, values


def image_metrics(path: Path, roi: list[int], golden: Path | None) -> dict[str, float | int | bool | str]:
    width, height, values = luma_values(read_png(path), roi)
    count = max(1, len(values))
    lit_ratio = sum(value >= 16 for value in values) / count
    mean = sum(values) / count
    edges = 0
    comparisons = 0
    for y in range(height):
        for x in range(width):
            at = y * width + x
            if x + 1 < width:
                edges += abs(values[at] - values[at + 1]) >= 20
                comparisons += 1
            if y + 1 < height:
                edges += abs(values[at] - values[at + width]) >= 20
                comparisons += 1
    result: dict[str, float | int | bool | str] = {
        "lit_ratio": round(lit_ratio, 6),
        "mean_luma": round(mean, 4),
        "edge_density": round(edges / max(1, comparisons), 6),
    }
    if golden is None or not golden.is_file():
        result.update({"golden": "missing", "pass": False})
        return result

    gw, gh, expected = luma_values(read_png(golden), roi)
    if (gw, gh, len(expected)) != (width, height, len(values)):
        result.update({"golden": str(golden), "error": "geometry mismatch", "pass": False})
        return result
    ecount = max(1, len(expected))
    e_lit = sum(value >= 16 for value in expected) / ecount
    e_mean = sum(expected) / ecount
    e_edges = 0
    e_comparisons = 0
    for y in range(height):
        for x in range(width):
            at = y * width + x
            if x + 1 < width:
                e_edges += abs(expected[at] - expected[at + 1]) >= 20
                e_comparisons += 1
            if y + 1 < height:
                e_edges += abs(expected[at] - expected[at + width]) >= 20
                e_comparisons += 1
    e_edge = e_edges / max(1, e_comparisons)
    mu_x, mu_y = mean, e_mean
    var_x = sum((v - mu_x) ** 2 for v in values) / ecount
    var_y = sum((v - mu_y) ** 2 for v in expected) / ecount
    covariance = sum((a - mu_x) * (b - mu_y) for a, b in zip(values, expected)) / ecount
    c1, c2 = (0.01 * 255) ** 2, (0.03 * 255) ** 2
    ssim = ((2 * mu_x * mu_y + c1) * (2 * covariance + c2)) / (
        (mu_x * mu_x + mu_y * mu_y + c1) * (var_x + var_y + c2))

    def dhash(vals: list[float], w: int, h: int) -> list[bool]:
        bits = []
        for oy in range(8):
            sy = min(h - 1, int((oy + 0.5) * h / 8))
            for ox in range(8):
                ax = min(w - 1, int((ox + 0.25) * w / 9))
                bx = min(w - 1, int((ox + 1.25) * w / 9))
                bits.append(vals[sy * w + ax] > vals[sy * w + bx])
        return bits

    phash_distance = sum(a != b for a, b in zip(
        dhash(values, width, height), dhash(expected, width, height)))
    ratios = {
        "lit_ratio_vs_golden": lit_ratio / max(e_lit, 1e-9),
        "mean_luma_vs_golden": mean / max(e_mean, 1e-9),
        "edge_density_vs_golden": (edges / max(1, comparisons)) / max(e_edge, 1e-9),
    }
    passed = (
        ratios["lit_ratio_vs_golden"] >= 0.60
        and ratios["mean_luma_vs_golden"] >= 0.50
        and ratios["edge_density_vs_golden"] >= 0.60
        and ssim >= 0.70
        and phash_distance <= 16
    )
    result.update({key: round(value, 6) for key, value in ratios.items()})
    result.update({
        "ssim": round(ssim, 6),
        "phash_distance": phash_distance,
        "golden": str(golden),
        "pass": passed,
    })
    return result


def parse_last(pattern: re.Pattern[str], log: str) -> tuple[int, ...] | None:
    matches = pattern.findall(log)
    if not matches:
        return None
    last = matches[-1]
    if isinstance(last, str):
        return (int(last),)
    return tuple(int(value) for value in last)


def install_donor(profile: str, spec: dict[str, str], temp: Path) -> tuple[Path, str]:
    donor = (DONOR_ROOT / spec["path"]).resolve()
    if not donor.is_file():
        raise RuntimeError(
            "donor missing: %s\n"
            "  Set HANDHELD_DONORS if your donor tree is elsewhere "
            "(default: ~/Archive/handheld/donors)." % donor)
    actual_hash = sha256_file(donor)
    if actual_hash != spec["sha256"]:
        raise RuntimeError("donor SHA256 mismatch for %s: %s" % (profile, actual_hash))
    source: Path = donor
    if spec["kind"] == "7z":
        source = unpack_7z(donor, temp / "unpacked")
    game_dir = temp / "game"
    game_dir.mkdir()
    result = run_checked([
        sys.executable, str(EAPX), "install", "--recipe", str(RECIPE),
        "--game-dir", str(game_dir), "--input", str(source), "--no-adopt",
        "--no-portmaster", "--tty", "none",
    ])
    marker = json.loads((game_dir / ".eapx-deadspace-data.json").read_text())
    detected = marker.get("donor_profile")
    if detected != profile:
        raise RuntimeError("profile mismatch: expected %s, eapx detected %s" % (profile, detected))
    return game_dir, result.stdout


def select_golden(profile: str, frame: int) -> Path | None:
    specific = GOLDEN_DIR / profile / ("frame-%d.png" % frame)
    if specific.is_file():
        return specific
    canonical = GOLDEN_DIR / "vita-rip-atc" / ("frame-%d.png" % frame)
    return canonical if canonical.is_file() else None


def one_run(profile: str, spec: dict[str, str], repetition: int,
            config: dict, output: Path, rebuild: bool) -> dict:
    started = time.monotonic()
    result: dict = {"profile": profile, "repetition": repetition, "checks": {}}
    scratch_root = output / "scratch"
    scratch_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="deadspace-compat-", dir=scratch_root) as raw_temp:
        temp = Path(raw_temp)
        try:
            game_dir, install_log = install_donor(profile, spec, temp)
            result["checks"]["C1_donor"] = {"pass": True, "detected": profile}
            fixture = (HERE / config["save_fixture"]).resolve()
            if not fixture.is_dir():
                raise RuntimeError(
                    "deterministic save fixture missing: %s; copy the known-good "
                    "Dead Space var directory there" % fixture
                )
            save_files = {}
            for source in sorted(fixture.iterdir()):
                if source.is_file():
                    save_files[source.name] = sha256_file(source)
            shutil.copytree(fixture, game_dir / "var", dirs_exist_ok=True)
            result["save_fixture"] = {"path": str(fixture), "files": save_files}
            run_dir = output / ("%s-%d" % (profile, repetition))
            (run_dir / "screenshots").mkdir(parents=True)
            log_path = run_dir / "emulator.log"
            env = os.environ.copy()
            env.update({
                "DEADSPACE_GAMEDIR": str(game_dir),
                "DEADSPACE_CONTROL_DIR": str(run_dir),
                "DEADSPACE_AUTOPILOT": "1",
                "DEADSPACE_AUTOPILOT_VISUAL": "1",
                "DEADSPACE_FRAME_LIMIT": str(config["frame_limit"]),
                "DEADSPACE_GL_DIAG": "1",
                "DEADSPACE_DONOR_PROFILE": profile,
                "DEADSPACE_TEST_TIME_SCALE": "4",
                "DEADSPACE_AUTO_SCREENSHOT_FRAMES": ",".join(map(str, config["checkpoints"])),
            })
            if not rebuild:
                env["DEADSPACE_EMULATOR_SKIP_BUILD"] = "1"
            with log_path.open("w") as log_file:
                process = subprocess.run(
                    [str(RUNNER), "--game-dir", str(game_dir),
                     "--control-dir", str(run_dir)],
                    cwd=PORT_DIR, env=env, text=True, stdout=log_file,
                    stderr=subprocess.STDOUT, timeout=1200,
                )
            log = log_path.read_text(errors="replace")
            texture = parse_last(TEXTURE_RE, log)
            summary = parse_last(SUMMARY_RE, log)
            frames = parse_last(FRAMES_RE, log)
            frame_count = frames[0] if frames else 0
            if texture:
                names = ["atc_decoded", "pvrtc_native", "pvrtc_decoded", "rgba",
                         "subimage", "passthrough", "failed", "gl_errors"]
                texture_stats = dict(zip(names, texture))
            else:
                texture_stats = {}
            if spec["expected_texture"] == "atc":
                expected_count = texture_stats.get("atc_decoded", 0)
            elif spec["expected_texture"] == "pvrtc":
                expected_count = (texture_stats.get("pvrtc_native", 0) +
                                  texture_stats.get("pvrtc_decoded", 0))
            elif spec["expected_texture"] == "etc1":
                # This donor remains in the matrix as a quantified negative
                # case. It may pass only after the runtime reports real ETC1
                # decoding; RGB allocations or a non-black HUD are not enough.
                expected_count = texture_stats.get("etc1_decoded", 0)
            else:
                expected_count = texture_stats.get("subimage", 0)
            c2 = bool(texture_stats) and expected_count > 0 and \
                texture_stats.get("failed") == 0 and texture_stats.get("gl_errors") == 0
            result["checks"]["C2_textures"] = {
                "pass": c2, "expected": spec["expected_texture"], **texture_stats,
            }
            result["checks"]["C3_gameplay"] = {
                "pass": process.returncode == 0 and frame_count >= config["frame_limit"],
                "returncode": process.returncode, "frames": frame_count,
                "summary": summary,
            }
            visual = []
            for frame in config["visual_frames"]:
                shot = run_dir / "screenshots" / ("auto-frame-%d.png" % frame)
                if shot.is_file():
                    metrics = image_metrics(
                        shot, config["isaac_roi"], select_golden(profile, frame))
                else:
                    metrics = {"pass": False, "error": "missing screenshot"}
                metrics["frame"] = frame
                metrics["screenshot"] = str(shot)
                visual.append(metrics)
            visual_passes = sum(bool(item.get("pass")) for item in visual)
            result["checks"]["C4_isaac_visible"] = {
                "pass": visual_passes == len(config["visual_frames"]),
                "captures": visual,
            }
            result["checks"]["C5_stability"] = {
                "pass": visual_passes >= 3,
                "passing_captures": visual_passes,
                "required": 3,
            }
            result["install_log_tail"] = install_log.splitlines()[-10:]
        except (Exception, subprocess.TimeoutExpired) as error:
            result["error"] = "%s: %s" % (type(error).__name__, error)
    result["duration_seconds"] = round(time.monotonic() - started, 3)
    result["pass"] = all(check.get("pass") for check in result["checks"].values()) \
        and len(result["checks"]) == 5
    return result


def record_goldens(report: dict, profile: str) -> None:
    runs = [run for run in report["runs"] if run["profile"] == profile]
    if not runs:
        raise RuntimeError("no run available to record golden for %s" % profile)
    captures = runs[-1].get("checks", {}).get("C4_isaac_visible", {}).get("captures", [])
    target = GOLDEN_DIR / profile
    target.mkdir(parents=True, exist_ok=True)
    for capture in captures:
        source = Path(str(capture["screenshot"]))
        if not source.is_file():
            raise RuntimeError("cannot record missing capture %s" % source)
        shutil.copy2(source, target / ("frame-%d.png" % capture["frame"]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="run all configured profiles")
    parser.add_argument("--profile", action="append", choices=[
        "full-pvrtc-original", "full-pvrtc-repacked", "vita-rip-atc"])
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--frame-limit", type=int)
    parser.add_argument("--release-gate", action="store_true")
    parser.add_argument("--record-golden", choices=[
        "full-pvrtc-original", "full-pvrtc-repacked", "vita-rip-atc"])
    parser.add_argument("--run-id", default=None)
    parser.add_argument("--save-fixture", help="local deterministic var directory")
    args = parser.parse_args()
    config = json.loads(CONFIG_PATH.read_text())
    if args.save_fixture:
        config["save_fixture"] = str(Path(args.save_fixture).expanduser().resolve())
    if args.frame_limit:
        config["frame_limit"] = args.frame_limit
    if args.release_gate:
        args.repeat = max(args.repeat, 3)
    selected = list(config["profiles"]) if args.all or not args.profile else args.profile
    run_id = args.run_id or time.strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:6]
    output = RUNTIME_ROOT / run_id
    output.mkdir(parents=True, exist_ok=args.run_id is not None)
    report = {
        "run_id": run_id,
        "frame_limit": config["frame_limit"],
        "profiles": selected,
        "repeat": args.repeat,
        "runs": [],
    }
    first = True
    for repetition in range(1, args.repeat + 1):
        for profile in selected:
            print("[compat] %s repetition %d/%d" % (profile, repetition, args.repeat), flush=True)
            (output / "progress.json").write_text(json.dumps({
                "state": "running", "profile": profile,
                "repetition": repetition, "repeat": args.repeat,
            }, indent=2) + "\n")
            run = one_run(profile, config["profiles"][profile], repetition,
                          config, output, rebuild=first)
            first = False
            report["runs"].append(run)
            print("[compat] %s: %s" % (profile, "PASS" if run["pass"] else "FAIL"), flush=True)
            (output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    report["pass"] = bool(report["runs"]) and all(run["pass"] for run in report["runs"])
    if args.record_golden:
        try:
            record_goldens(report, args.record_golden)
            report["golden_recorded"] = args.record_golden
        except RuntimeError as error:
            report["golden_error"] = str(error)
    (output / "report.json").write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    (output / "progress.json").write_text(json.dumps({
        "state": "complete", "pass": report["pass"],
        "completed_runs": len(report["runs"]),
    }, indent=2) + "\n")
    print("[compat] report %s" % (output / "report.json"))
    print("[compat] %s" % ("PASS" if report["pass"] else "FAIL"))
    return 0 if report["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
