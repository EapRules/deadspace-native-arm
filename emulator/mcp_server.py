#!/usr/bin/env python3
"""MCP server for the Dead Space qemu/Mesa development emulator.

It intentionally uses only Python's standard library so any MCP client can run
it without npm, pip, virtualenvs, or network access. Transport is MCP JSON-RPC over
stdio (one JSON object per line).
"""

from __future__ import annotations

import atexit
import base64
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import uuid
from typing import Any


PORT_DIR = Path(__file__).resolve().parent.parent
RUNTIME_DIR = Path(
    os.environ.get("DEADSPACE_EMULATOR_RUNTIME", PORT_DIR / "emulator/runtime")
).resolve()
RUNNER = PORT_DIR / "emulator/run.sh"
COMMANDS = RUNTIME_DIR / "commands"
STATUS = RUNTIME_DIR / "status.json"
LOG = RUNTIME_DIR / "emulator.log"
COMPAT_RUNNER = PORT_DIR / "emulator/compatibility_test.py"
COMPAT_ROOT = RUNTIME_DIR / "compatibility"

_process: subprocess.Popen[bytes] | None = None
_log_handle: Any = None
_compat_process: subprocess.Popen[bytes] | None = None
_compat_log_handle: Any = None
_compat_run_id: str | None = None


def _text(message: str) -> dict[str, Any]:
    return {"content": [{"type": "text", "text": message}]}


def _running() -> bool:
    global _process
    if _process is not None:
        return _process.poll() is None
    return False


def _read_status() -> dict[str, Any]:
    try:
        return json.loads(STATUS.read_text())
    except (OSError, json.JSONDecodeError):
        return {"state": "starting" if _running() else "stopped", "frame": 0}


def _append(command: str) -> None:
    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    if not COMMANDS.exists():
        raise RuntimeError("emulator command channel does not exist; start it first")
    with COMMANDS.open("a", encoding="utf-8") as out:
        out.write(command.rstrip("\n") + "\n")
        out.flush()


def _wait_until(predicate: Any, timeout: float, interval: float = 0.1) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        if _process is not None and _process.poll() is not None:
            return False
        time.sleep(interval)
    return False


def start_emulator(arguments: dict[str, Any]) -> dict[str, Any]:
    global _process, _log_handle
    if _running():
        return _text(f"Emulator already running: {_read_status()}")

    RUNTIME_DIR.mkdir(parents=True, exist_ok=True)
    (RUNTIME_DIR / "screenshots").mkdir(exist_ok=True)
    for stale in (STATUS, RUNTIME_DIR / "status.json.tmp"):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    _log_handle = LOG.open("wb")
    env = os.environ.copy()
    env["DEADSPACE_CONTROL_DIR"] = str(RUNTIME_DIR)
    if arguments.get("game_dir"):
        env["DEADSPACE_GAMEDIR"] = str(Path(arguments["game_dir"]).expanduser().resolve())
    if arguments.get("gl_diag") or arguments.get("mali_compat"):
        env["DEADSPACE_GL_DIAG"] = "1"
    if arguments.get("mali_compat"):
        env["DEADSPACE_MALI_COMPAT"] = "1"
    if (PORT_DIR / "build/deadspace").exists() and not arguments.get("rebuild"):
        env["DEADSPACE_EMULATOR_SKIP_BUILD"] = "1"
    _process = subprocess.Popen(
        [str(RUNNER), "--control-dir", str(RUNTIME_DIR)],
        cwd=PORT_DIR,
        env=env,
        stdout=_log_handle,
        stderr=subprocess.STDOUT,
        start_new_session=True,
    )
    (RUNTIME_DIR / "runner.pid").write_text(f"{_process.pid}\n")

    ready = _wait_until(
        lambda: _read_status().get("state") in {"ready", "running"}, 60.0, 0.2
    )
    if not ready:
        tail = read_log({"lines": 40})["content"][0]["text"]
        raise RuntimeError(f"emulator did not become ready\n{tail}")
    return _text(f"Emulator started: {_read_status()}")


def stop_emulator(arguments: dict[str, Any]) -> dict[str, Any]:
    global _process, _log_handle
    if not _running():
        return _text("Emulator is not running.")

    _append("quit")
    if not _wait_until(lambda: _process is not None and _process.poll() is not None, 8.0):
        # NativeOnDrawFrame can block while loading. Signal only the process
        # group created by this MCP instance; never identify a target by name.
        assert _process is not None
        os.killpg(_process.pid, signal.SIGINT)
        _wait_until(lambda: _process is not None and _process.poll() is not None, 5.0)
    if _process is not None and _process.poll() is None:
        os.killpg(_process.pid, signal.SIGTERM)
        _wait_until(lambda: _process is not None and _process.poll() is not None, 3.0)

    rc = _process.poll() if _process is not None else None
    _process = None
    if _log_handle is not None:
        _log_handle.close()
        _log_handle = None
    return _text(f"Emulator stopped (runner rc={rc}).")


def emulator_status(arguments: dict[str, Any]) -> dict[str, Any]:
    status = _read_status()
    status["runnerAlive"] = _running()
    status["runtime"] = str(RUNTIME_DIR)
    return _text(json.dumps(status, indent=2))


def move_cursor(arguments: dict[str, Any]) -> dict[str, Any]:
    x = float(arguments["x"])
    y = float(arguments["y"])
    if not 0 <= x < 640 or not 0 <= y < 480:
        raise ValueError("cursor coordinates must be inside 640x480")
    _append(f"cursor {x:.2f} {y:.2f}")
    return _text(f"Cursor queued at ({x:.1f}, {y:.1f}).")


def click(arguments: dict[str, Any]) -> dict[str, Any]:
    if "x" in arguments or "y" in arguments:
        if "x" not in arguments or "y" not in arguments:
            raise ValueError("provide both x and y, or neither")
        move_cursor({"x": arguments["x"], "y": arguments["y"]})
    _append("click down")
    _append("click up")
    return _text("Touch click queued (down/up on separate game frames).")


VALID_CONTROLS = {
    "a", "b", "x", "y", "l1", "l2", "r1", "r2", "l3", "r3",
    "start", "select", "up", "down", "left", "right",
}


def press_control(arguments: dict[str, Any]) -> dict[str, Any]:
    control = str(arguments["control"]).lower()
    if control not in VALID_CONTROLS:
        raise ValueError(f"unknown control {control!r}")
    _append(f"button {control} down")
    _append(f"button {control} up")
    return _text(f"{control} queued (down/up on separate game frames).")


def set_stick(arguments: dict[str, Any]) -> dict[str, Any]:
    stick = str(arguments["stick"]).lower()
    if stick not in {"left", "right"}:
        raise ValueError(f"unknown stick {stick!r}")
    x = float(arguments["x"])
    y = float(arguments["y"])
    if not -1.0 <= x <= 1.0 or not -1.0 <= y <= 1.0:
        raise ValueError("stick axes must be inside -1.0..1.0")
    _append(f"stick {stick} {x:.5f} {y:.5f}")
    return _text(f"{stick} stick held at ({x:.3f}, {y:.3f}); send 0,0 to release.")


def capture_screen(arguments: dict[str, Any]) -> dict[str, Any]:
    token = f"mcp-{int(time.time())}-{uuid.uuid4().hex[:8]}"
    path = RUNTIME_DIR / "screenshots" / f"{token}.png"
    _append(f"screenshot {token}")
    if not _wait_until(lambda: path.is_file() and path.stat().st_size > 0, 60.0, 0.2):
        raise RuntimeError(
            "screenshot timed out; NativeOnDrawFrame may be blocked. "
            f"Current status: {_read_status()}"
        )
    data = base64.b64encode(path.read_bytes()).decode("ascii")
    return {
        "content": [
            {"type": "text", "text": f"Framebuffer capture: {path}"},
            {"type": "image", "data": data, "mimeType": "image/png"},
        ]
    }


def read_log(arguments: dict[str, Any]) -> dict[str, Any]:
    lines = int(arguments.get("lines", 120))
    lines = max(1, min(lines, 1000))
    try:
        content = LOG.read_text(errors="replace").splitlines()
    except FileNotFoundError:
        content = []
    return _text("\n".join(content[-lines:]) or "(no emulator log yet)")


def run_compatibility_matrix(arguments: dict[str, Any]) -> dict[str, Any]:
    global _compat_process, _compat_log_handle, _compat_run_id
    if _compat_process is not None and _compat_process.poll() is None:
        return _text(f"Compatibility matrix already running: {_compat_run_id}")
    if _running():
        raise RuntimeError("stop the interactive emulator before starting the matrix")
    profiles = arguments.get("profiles") or []
    repeat = int(arguments.get("repeat", 1))
    if repeat < 1 or repeat > 10:
        raise ValueError("repeat must be between 1 and 10")
    frame_limit = int(arguments.get("frame_limit", 3800))
    if frame_limit < 100 or frame_limit > 50000:
        raise ValueError("frame_limit must be between 100 and 50000")
    _compat_run_id = time.strftime("%Y%m%d-%H%M%S") + "-mcp-" + uuid.uuid4().hex[:6]
    output = COMPAT_ROOT / _compat_run_id
    output.mkdir(parents=True, exist_ok=False)
    command = [
        sys.executable, str(COMPAT_RUNNER), "--run-id", _compat_run_id,
        "--repeat", str(repeat), "--frame-limit", str(frame_limit),
    ]
    if profiles:
        for profile in profiles:
            command.extend(["--profile", str(profile)])
    else:
        command.append("--all")
    if arguments.get("release_gate"):
        command.append("--release-gate")
    _compat_log_handle = (output / "matrix.log").open("wb")
    _compat_process = subprocess.Popen(
        command, cwd=PORT_DIR, stdout=_compat_log_handle,
        stderr=subprocess.STDOUT, start_new_session=True,
    )
    return _text(json.dumps({
        "state": "running", "run_id": _compat_run_id,
        "report": str(output / "report.json"),
    }, indent=2))


def compatibility_status(arguments: dict[str, Any]) -> dict[str, Any]:
    running = _compat_process is not None and _compat_process.poll() is None
    report_path = COMPAT_ROOT / (_compat_run_id or "") / "report.json"
    report: dict[str, Any] = {}
    if report_path.is_file():
        try:
            report = json.loads(report_path.read_text())
        except (OSError, json.JSONDecodeError):
            report = {}
    completed = report.get("runs", [])
    progress_path = COMPAT_ROOT / (_compat_run_id or "") / "progress.json"
    progress: dict[str, Any] = {}
    if progress_path.is_file():
        try:
            progress = json.loads(progress_path.read_text())
        except (OSError, json.JSONDecodeError):
            progress = {}
    frame = None
    if _compat_run_id:
        status_files = sorted((COMPAT_ROOT / _compat_run_id).glob("*-*/status.json"))
        if status_files:
            try:
                frame = json.loads(status_files[-1].read_text()).get("frame")
            except (OSError, json.JSONDecodeError):
                frame = None
    payload = {
        "state": "running" if running else ("complete" if _compat_run_id else "idle"),
        "run_id": _compat_run_id,
        "completed_runs": len(completed),
        "last_run": completed[-1] if completed else None,
        "pass": report.get("pass") if not running else None,
        "profile": progress.get("profile"),
        "repetition": progress.get("repetition"),
        "frame": frame,
    }
    return _text(json.dumps(payload, indent=2))


def read_compatibility_report(arguments: dict[str, Any]) -> dict[str, Any]:
    run_id = str(arguments.get("run_id") or _compat_run_id or "")
    if not run_id or not all(c.isalnum() or c in "-_" for c in run_id):
        raise ValueError("a valid run_id is required")
    report = COMPAT_ROOT / run_id / "report.json"
    if not report.is_file():
        raise RuntimeError(f"report does not exist yet: {report}")
    return _text(report.read_text())


TOOLS: list[dict[str, Any]] = [
    {
        "name": "start_emulator",
        "description": "Start the persistent qemu-arm/Mesa Dead Space emulator and wait until its control channel is ready.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "rebuild": {
                    "type": "boolean",
                    "description": "Recompile the loader before starting.",
                    "default": False,
                },
                "game_dir": {
                    "type": "string",
                    "description": "Optional extracted Full or RIP game tree to mount instead of NeededFiles/data/deadspace.",
                },
                "gl_diag": {
                    "type": "boolean",
                    "description": "Enable per-upload GLES diagnostics in the emulator log.",
                    "default": False,
                },
                "mali_compat": {
                    "type": "boolean",
                    "description": "Mark this run as Mali-compatibility validation; forces diagnostic logging for the GLES1 upload path.",
                    "default": False,
                }
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "stop_emulator",
        "description": "Stop only the emulator process started by this MCP server.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "emulator_status",
        "description": "Return runner state, game frame number and control-directory path.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "move_cursor",
        "description": "Move the visible touch cursor to absolute 640x480 framebuffer coordinates.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "number", "minimum": 0, "maximum": 639},
                "y": {"type": "number", "minimum": 0, "maximum": 479},
            },
            "required": ["x", "y"],
            "additionalProperties": False,
        },
    },
    {
        "name": "click",
        "description": "Send a touchscreen down/up pair at the current cursor, or first move to optional x/y.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "number", "minimum": 0, "maximum": 639},
                "y": {"type": "number", "minimum": 0, "maximum": 479},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "press_control",
        "description": "Press and release one R36S control through the same JNI bridge used on hardware.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "control": {"type": "string", "enum": sorted(VALID_CONTROLS)}
            },
            "required": ["control"],
            "additionalProperties": False,
        },
    },
    {
        "name": "set_stick",
        "description": "Set and hold a virtual R36S analog stick; send x=0,y=0 to release it.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "stick": {"type": "string", "enum": ["left", "right"]},
                "x": {"type": "number", "minimum": -1.0, "maximum": 1.0},
                "y": {"type": "number", "minimum": -1.0, "maximum": 1.0},
            },
            "required": ["stick", "x", "y"],
            "additionalProperties": False,
        },
    },
    {
        "name": "capture_screen",
        "description": "Capture the emulator's current default framebuffer and return a real 640x480 PNG image.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "read_emulator_log",
        "description": "Read the tail of the persistent emulator log.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "lines": {"type": "integer", "minimum": 1, "maximum": 1000, "default": 120}
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "run_compatibility_matrix",
        "description": "Clean-install and visually test Full PVRTC, Vita RIP ATC, and the known unsupported ETC1 repack.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "profiles": {
                    "type": "array",
                    "items": {"type": "string", "enum": [
                        "full-pvrtc-original", "full-pvrtc-repacked", "vita-rip-atc"
                    ]},
                },
                "repeat": {"type": "integer", "minimum": 1, "maximum": 10, "default": 1},
                "frame_limit": {"type": "integer", "minimum": 100, "maximum": 50000, "default": 3800},
                "release_gate": {"type": "boolean", "default": False},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "compatibility_status",
        "description": "Return progress and the latest completed profile result from the compatibility matrix.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "read_compatibility_report",
        "description": "Read the quantitative JSON report for the latest or specified compatibility run.",
        "inputSchema": {
            "type": "object",
            "properties": {"run_id": {"type": "string"}},
            "additionalProperties": False,
        },
    },
]

HANDLERS = {
    "start_emulator": start_emulator,
    "stop_emulator": stop_emulator,
    "emulator_status": emulator_status,
    "move_cursor": move_cursor,
    "click": click,
    "press_control": press_control,
    "set_stick": set_stick,
    "capture_screen": capture_screen,
    "read_emulator_log": read_log,
    "run_compatibility_matrix": run_compatibility_matrix,
    "compatibility_status": compatibility_status,
    "read_compatibility_report": read_compatibility_report,
}


def _send(message: dict[str, Any]) -> None:
    sys.stdout.write(json.dumps(message, separators=(",", ":")) + "\n")
    sys.stdout.flush()


def _handle(message: dict[str, Any]) -> dict[str, Any] | None:
    method = message.get("method")
    request_id = message.get("id")
    if request_id is None:
        return None

    if method == "initialize":
        requested = message.get("params", {}).get("protocolVersion", "2025-06-18")
        result = {
            "protocolVersion": requested,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": "deadspace-emulator", "version": "0.1.0"},
        }
    elif method == "ping":
        result = {}
    elif method == "tools/list":
        result = {"tools": TOOLS}
    elif method == "tools/call":
        params = message.get("params", {})
        name = params.get("name")
        try:
            handler = HANDLERS[name]
            result = handler(params.get("arguments") or {})
        except Exception as exc:  # tool errors are data, not protocol failures
            result = {
                "content": [{"type": "text", "text": f"{type(exc).__name__}: {exc}"}],
                "isError": True,
            }
    else:
        return {
            "jsonrpc": "2.0",
            "id": request_id,
            "error": {"code": -32601, "message": f"Method not found: {method}"},
        }

    return {"jsonrpc": "2.0", "id": request_id, "result": result}


def _cleanup() -> None:
    if _running():
        try:
            stop_emulator({})
        except Exception:
            pass
    if _compat_process is not None and _compat_process.poll() is None:
        try:
            os.killpg(_compat_process.pid, signal.SIGTERM)
        except OSError:
            pass


def main() -> None:
    atexit.register(_cleanup)
    for raw in sys.stdin:
        try:
            message = json.loads(raw)
            response = _handle(message)
            if response is not None:
                _send(response)
        except Exception as exc:
            _send(
                {
                    "jsonrpc": "2.0",
                    "id": None,
                    "error": {"code": -32700, "message": f"Parse error: {exc}"},
                }
            )


if __name__ == "__main__":
    main()
