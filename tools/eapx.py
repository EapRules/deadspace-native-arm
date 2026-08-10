#!/usr/bin/env python3
"""eapx - first-boot extractor for Android-to-Linux game ports.

Created by EapRules.

Engine only: recipe loading, content-based discovery, planning, staged
extraction, validation and transactional publication. See DESIGN.md.
"""

import argparse
import binascii
import errno
import fcntl
import fnmatch
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import time
import uuid
import zipfile

VERSION = "0.4.4"
FORMAT_VERSION = 1
CHUNK_SIZE = 1 << 20
DEFAULT_SAFETY_BYTES = 128 << 20
DEFAULT_HOOK_TIMEOUT = 600
MAX_MESSAGE_BYTES = 200

# One 4x5 pixel typeface for logo and signature, rendered with only the
# half blocks the Linux console can actually draw: space, ▀, ▄ and █, all
# present in every kernel console font because they date back to cp437.
#
# This is a hard constraint, not a style choice. The framebuffer console
# draws glyphs from a small kernel font, and a rune it does not carry is
# substituted with a fallback diamond - photographed on real hardware, an
# earlier quadrant-block logo (▄▖▙▌▚▘) rendered as three rows of diamond
# noise. The `branded` gate below can only test the ENCODING; no userspace
# check sees the console font's coverage, so the art itself must stay
# inside the set that every console font has.
EAPX_LOGO = (
    "█▀▀▀ ▄▀▀▄ █▀▀▄ ▀▄▄▀",
    "█▀▀  █▀▀█ █▀▀   ██",
    "▀▀▀▀ ▀  ▀ ▀    ▀  ▀",
)
EAPRULES_SIGNATURE = (
    "█▀▀▄ █  █   █▀▀▀ ▄▀▀▄ █▀▀▄ █▀▀▄ █  █ █    █▀▀▀ ▄▀▀▀",
    "█▀▀▄  ██    █▀▀  █▀▀█ █▀▀  █▀█  █  █ █    █▀▀   ▀▀▄",
    "▀▀▀   ▀▀    ▀▀▀▀ ▀  ▀ ▀    ▀  ▀  ▀▀  ▀▀▀▀ ▀▀▀▀ ▀▀▀",
)
EAPX_SUBTITLE = "Android Port eXtractor"

# ELF e_machine values, paired with the expected EI_CLASS. The class check is
# what stops an ARMv5 32-bit object from validating as arm64.
ELF_ABIS = {
    "armeabi": (40, 1),
    "armeabi-v7a": (40, 1),
    "arm": (40, 1),
    "arm64-v8a": (183, 2),
    "arm64": (183, 2),
    "aarch64": (183, 2),
    "x86": (3, 1),
    "x86_64": (62, 2),
}


class EapxError(Exception):
    pass


class RecipeError(EapxError):
    pass


class SourceError(EapxError):
    pass


class PlanError(EapxError):
    pass


class ValidationError(EapxError):
    pass


# --------------------------------------------------------------------------
# paths
# --------------------------------------------------------------------------


def validate_relative_path(value, label="path"):
    if not isinstance(value, str) or not value:
        raise RecipeError("%s must be a non-empty string" % label)
    if "\x00" in value:
        raise RecipeError("%s contains a NUL byte" % label)
    if "\\" in value:
        raise RecipeError("%s uses a backslash; separators must be '/'" % label)
    if any(ord(character) < 32 for character in value):
        raise RecipeError("%s contains a control character" % label)
    if value.startswith("/"):
        raise RecipeError("%s must be relative, got an absolute path" % label)
    for part in value.split("/"):
        if part == "":
            raise RecipeError("%s has an empty path component" % label)
        if part == "." or part == "..":
            raise RecipeError("%s contains a '%s' component" % (label, part))
    return value


def safe_join(root, relative, label="path"):
    validate_relative_path(relative, label)
    root = os.path.realpath(root)
    target = os.path.normpath(os.path.join(root, relative))
    if target != root and not target.startswith(root + os.sep):
        raise EapxError("%s escapes the root directory" % label)
    return target


def ensure_no_symlink_parents(root, relative):
    current = os.path.realpath(root)
    for part in relative.split("/")[:-1]:
        current = os.path.join(current, part)
        if os.path.islink(current):
            raise EapxError("refusing to traverse symlink: %s" % current)


def safe_zip_name(name):
    """Normalise a zip member name, rejecting anything unsafe."""
    if not name or name.endswith("/"):
        return None
    if "\x00" in name or "\\" in name:
        return None
    if name.startswith("/") or ":" in name:
        return None
    parts = []
    for part in name.split("/"):
        if part in ("", ".", ".."):
            return None
        parts.append(part)
    return "/".join(parts)


def is_regular_file(path):
    try:
        return stat.S_ISREG(os.lstat(path).st_mode)
    except OSError:
        return False


def remove_path(path):
    if os.path.islink(path) or os.path.isfile(path):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
    elif os.path.isdir(path):
        shutil.rmtree(path, ignore_errors=True)


def human_bytes(value):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if value < 1024 or unit == "GiB":
            return "%.1f %s" % (value, unit)
        value /= 1024.0
    return "%d B" % value


# --------------------------------------------------------------------------
# durability
# --------------------------------------------------------------------------


class Durability:
    """Directory fsync, with the capability probed once and reported.

    Silently swallowing a failed directory fsync is worse than not having the
    guarantee: it means believing you have it. On exFAT/vfat -- realistic on a
    handheld SD card -- the transaction survives a kill but not a power cut,
    and the user deserves to be told.
    """

    def __init__(self, logger):
        self.logger = logger
        self.supported = None

    def probe(self, path):
        self.supported = self._fsync(path)
        if self.supported:
            self.logger.log("directory fsync supported; transaction is power-cut safe")
        else:
            self.logger.log(
                "WARNING: directory fsync unavailable on this filesystem. "
                "The transaction survives a killed process but NOT a power cut."
            )
        return self.supported

    def _fsync(self, path):
        try:
            descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
        except OSError:
            return False
        try:
            os.fsync(descriptor)
            return True
        except OSError:
            return False
        finally:
            os.close(descriptor)

    def sync_dir(self, path):
        self._fsync(path)

    def sync_parents(self, root, relatives):
        """fsync every parent directory touched, not just the root.

        A marker made durable before the renames it certifies lets recovery
        conclude the transaction published and drop the backup, losing the
        original and the payload together.
        """
        seen = set()
        for relative in relatives:
            current = os.path.realpath(root)
            seen.add(current)
            for part in relative.split("/")[:-1]:
                current = os.path.join(current, part)
                if os.path.isdir(current):
                    seen.add(current)
        for directory in sorted(seen, key=len, reverse=True):
            self.sync_dir(directory)


def atomic_write(path, data, durability=None, sync=True):
    parent = os.path.dirname(path) or "."
    os.makedirs(parent, exist_ok=True)
    temporary = "%s.tmp.%d.%s" % (path, os.getpid(), uuid.uuid4().hex[:8])
    try:
        with open(temporary, "w", encoding="utf-8", newline="\n") as stream:
            stream.write(data)
            stream.flush()
            if sync:
                os.fsync(stream.fileno())
        os.replace(temporary, path)
        if sync and durability is not None:
            durability.sync_dir(parent)
    finally:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass


def atomic_write_json(path, value, durability=None, sync=True):
    payload = json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    atomic_write(path, payload, durability, sync)


def load_json(path):
    def no_duplicates(pairs):
        seen = {}
        for key, value in pairs:
            if key in seen:
                raise RecipeError("duplicate key %r" % key)
            seen[key] = value
        return seen

    with open(path, "r", encoding="utf-8") as stream:
        return json.load(stream, object_pairs_hook=no_duplicates)


def canonical_json(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


# --------------------------------------------------------------------------
# digests -- every byte is read once
# --------------------------------------------------------------------------


class DigestCache:
    """Single-pass sha256 + crc32, memoised.

    Without this the same object gets hashed once per (group x ABI) planning
    attempt, again to break ties, again on resume and again on adoption. On a
    slow SD each extra pass over 2 GB is roughly 100 seconds of wall clock.
    """

    def __init__(self):
        self._files = {}
        self._members = {}
        self._region_digests = {}
        self.bytes_read = 0

    def file(self, path):
        try:
            info = os.stat(path, follow_symlinks=False)
        except OSError as error:
            raise SourceError("cannot stat %s: %s" % (path, error))
        key = (os.path.realpath(path), info.st_size, info.st_mtime_ns)
        cached = self._files.get(key)
        if cached is not None:
            return cached
        sha = hashlib.sha256()
        crc = 0
        with open(path, "rb") as stream:
            while True:
                block = stream.read(CHUNK_SIZE)
                if not block:
                    break
                sha.update(block)
                crc = binascii.crc32(block, crc)
                self.bytes_read += len(block)
        result = (sha.hexdigest(), crc & 0xFFFFFFFF, info.st_size)
        self._files[key] = result
        return result

    def invalidate(self, prefix):
        """Drop everything cached under `prefix`.

        FAT stores mtime with one to two second resolution, so the
        (path, size, mtime) key cannot distinguish two same-sized contents
        written inside the same second -- and an SD card in a handheld is
        exactly where this runs. Anything that may have rewritten a file behind
        our back has to drop the cache rather than trust the timestamp.
        """
        prefix = os.path.realpath(prefix)
        for key in [k for k in self._files if k[0].startswith(prefix)]:
            del self._files[key]
        for key in [k for k in self._region_digests
                    if k[0] == "file" and k[1].startswith(prefix)]:
            del self._region_digests[key]

    def remember(self, path, sha256, crc, size):
        """Seed the cache for a file we just wrote and already hashed."""
        try:
            info = os.stat(path, follow_symlinks=False)
        except OSError:
            return
        key = (os.path.realpath(path), info.st_size, info.st_mtime_ns)
        self._files[key] = (sha256, crc & 0xFFFFFFFF, size)

    def member(self, archive, name):
        key = (archive.path, name)
        cached = self._members.get(key)
        if cached is not None:
            return cached
        info = archive.info(name)
        sha = hashlib.sha256()
        crc = 0
        with archive.open(name) as stream:
            while True:
                block = stream.read(CHUNK_SIZE)
                if not block:
                    break
                sha.update(block)
                crc = binascii.crc32(block, crc)
                self.bytes_read += len(block)
        result = (sha.hexdigest(), crc & 0xFFFFFFFF, info.file_size)
        self._members[key] = result
        return result

    def _critical_regions(self, key, opener, regions, label):
        signature = tuple((region["offset"], region["size"]) for region in regions)
        cache_key = key + (signature,)
        cached = self._region_digests.get(cache_key)
        if cached is not None:
            return cached
        sha = hashlib.sha256()
        try:
            with opener() as stream:
                for region in regions:
                    offset = region["offset"]
                    remaining = region["size"]
                    if stream.seek(offset) != offset:
                        raise SourceError(
                            "%s: cannot seek to critical region offset %d"
                            % (label, offset)
                        )
                    while remaining:
                        block = stream.read(min(CHUNK_SIZE, remaining))
                        if not block:
                            raise SourceError(
                                "%s: critical region at offset %d is truncated"
                                % (label, offset)
                            )
                        sha.update(block)
                        self.bytes_read += len(block)
                        remaining -= len(block)
        except SourceError:
            raise
        except (OSError, ValueError, zipfile.BadZipFile) as error:
            raise SourceError("cannot read critical regions from %s: %s" % (label, error))
        result = sha.hexdigest()
        self._region_digests[cache_key] = result
        return result

    def file_regions(self, path, regions):
        try:
            info = os.stat(path, follow_symlinks=False)
        except OSError as error:
            raise SourceError("cannot stat %s: %s" % (path, error))
        real = os.path.realpath(path)
        key = ("file", real, info.st_size, info.st_mtime_ns)
        return self._critical_regions(
            key, lambda: open(path, "rb"), regions, path
        )

    def member_regions(self, archive, name, regions):
        info = archive.info(name)
        key = ("member", archive.path, name, info.file_size, info.CRC)
        return self._critical_regions(
            key, lambda: archive.open(name), regions,
            "%s:%s" % (archive.path, name),
        )


def elf_identity(header):
    """Return (e_machine, ei_class) or None when this is not an ELF header."""
    if len(header) < 20 or header[:4] != b"\x7fELF":
        return None
    ei_class = header[4]
    little_endian = header[5] != 2
    layout = "<H" if little_endian else ">H"
    (machine,) = struct.unpack_from(layout, header, 18)
    return machine, ei_class


# --------------------------------------------------------------------------
# logging and progress
# --------------------------------------------------------------------------


class Logger:
    def __init__(self, path, verbose=True):
        self.verbose = verbose
        self.stream = None
        if path:
            try:
                os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
                self.stream = open(path, "a", encoding="utf-8")
            except OSError:
                self.stream = None

    def log(self, message):
        line = "%s %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), message)
        if self.stream is not None:
            self.stream.write(line + "\n")
            self.stream.flush()
        if self.verbose:
            sys.stderr.write(line + "\n")

    def close(self):
        if self.stream is not None:
            self.stream.close()
            self.stream = None


def clamp_text(value):
    """Bound message length so a long error cannot desynchronise the reader."""
    text = " ".join(str(value).split())
    encoded = text.encode("utf-8")[:MAX_MESSAGE_BYTES]
    return encoded.decode("utf-8", "ignore")


class PortMaster:
    """Talks to the progress bar the PortMaster runtime already ships.

    pugwash listens on a FIFO and accepts a `progress` command taking an
    amount, a total and a format. It is fully implemented and, as far as the
    catalogue shows, no port has ever called it -- ports that want to show
    advancement hardcode an expected size and poll `du`. An extractor knows
    exactly how many bytes it is about to write, so it can drive the real bar.
    """

    INPUT = "/dev/shm/portmaster/pm_input"
    DONE = "/dev/shm/portmaster/pm_done"

    def __init__(self, logger, enabled=True):
        self.logger = logger
        self.active = bool(enabled) and os.path.exists(self.INPUT)
        self.last = 0.0
        if self.active:
            logger.log("using the PortMaster progress bar")

    def send(self, command, *args, **kwargs):
        if not self.active:
            return
        payload = "\1".join([command] + [str(a) for a in args]) + "\n"
        try:
            with open(self.DONE, "w") as done:
                done.write("WAIT")
            # A FIFO opened for writing blocks until someone is reading it.
            # Non-blocking turns "the GUI went away" into ENXIO instead of a
            # hung install -- the UI is never allowed to be fatal.
            handle = os.open(self.INPUT, os.O_WRONLY | os.O_NONBLOCK)
            try:
                os.write(handle, payload.encode("utf-8"))
            finally:
                os.close(handle)
            deadline = time.time() + kwargs.get("wait", 2.0)
            while time.time() < deadline:
                with open(self.DONE) as done:
                    if done.read().strip() != "WAIT":
                        return
                time.sleep(0.05)
        except OSError as error:
            self.logger.log("progress bar unavailable (%s); continuing" % error)
            self.active = False

    def progress(self, text, done, total):
        now = time.time()
        if now - self.last < 0.5:
            return
        self.last = now
        self.send("progress", text, done, total, "data")

    def clear(self):
        self.send("progress_clear")


class Progress:
    """State-based progress file. Written atomically, deliberately not fsynced.

    Every write is the complete state rather than a delta, so a reader that
    misses updates simply reads the latest one. Losing this file in a power cut
    costs nothing -- the real state lives in the journal -- so paying 25 fsyncs
    a second against the same SD card the extraction is using is pure damage.
    """

    def __init__(self, path, logger, title="", tty=None, portmaster=None):
        self.path = path
        self.logger = logger
        self.title = title
        self.portmaster = portmaster
        self.spoken = None
        self.last = None
        self.last_write = 0.0
        self.total_bytes = 0
        self.done_bytes = 0
        self.started = time.time()
        self.tty = self._open_tty(tty)
        self.tty_started = False
        self.tty_geometry = None

    def _open_tty(self, override):
        """The console TTY is the whole UI.

        A PortMaster launcher already owns the screen by the time it runs -- the
        existing ports print full-screen messages here -- so there is nothing to
        negotiate with a display server and no need for an SDL binary. Text on
        the framebuffer console is enough to stop a five minute first boot from
        looking like a hang.
        """
        if override in ("none", "off"):
            return None
        if override:
            try:
                return open(override, "w")
            except OSError as error:
                self.logger.log("cannot write progress to %s: %s" % (override, error))
                return None
        for path in ("/dev/tty0", "/dev/tty1"):
            try:
                if os.access(path, os.W_OK):
                    return open(path, "w")
            except OSError:
                continue
        return None

    def _render(self, state, overall, message, detail):
        if self.tty is None:
            return
        try:
            geometry = os.get_terminal_size(self.tty.fileno())
            columns, rows = geometry.columns, geometry.lines
        except (AttributeError, OSError, ValueError):
            columns, rows = 80, 24
        # Framebuffer consoles sometimes report cells that are partly hidden by
        # display overscan. Keep all visible content inside a small safe area and
        # never write in the terminal's final row. This also avoids triggering
        # auto-wrap by filling a physical row to its last column.
        columns = max(1, columns)
        rows = max(1, rows)
        side_margin = 2 if columns >= 24 else (1 if columns >= 8 else 0)
        top_margin = 1 if rows >= 10 else 0
        bottom_margin = 1 if rows >= 5 else 0
        safe_width = max(1, columns - side_margin * 2)
        safe_height = max(1, rows - top_margin - bottom_margin)

        encoding = getattr(self.tty, "encoding", None) or "utf-8"
        branded = safe_width >= max(len(line) for line in EAPRULES_SIGNATURE)
        if branded:
            try:
                "".join(EAPX_LOGO + EAPRULES_SIGNATURE).encode(encoding)
            except (LookupError, UnicodeError):
                branded = False

        def block(lines):
            # Pad to one width so per-line centring cannot shear the
            # letterforms sideways: art is a block, not independent rows.
            width = max(len(line) for line in lines)
            return tuple(line.ljust(width) for line in lines)

        logo = block(EAPX_LOGO) if branded else ("EAPX",)
        signature = block(EAPRULES_SIGNATURE) if branded else ("BY EAPRULES",)
        subtitle = "%s %s v%s" % (
            EAPX_SUBTITLE, "·" if branded else "-", VERSION
        )
        width = min(34, max(1, safe_width - 8))
        filled = max(0, min(width, overall * width // 1000))
        full, empty = ("█", "░") if branded else ("#", "-")
        bar = full * filled + empty * (width - filled)
        eta = ""
        if state == 1 and self.done_bytes and self.total_bytes:
            elapsed = time.time() - self.started
            rate = self.done_bytes / elapsed if elapsed > 0.5 else 0
            if rate > 0:
                left = int((self.total_bytes - self.done_bytes) / rate)
                eta = "%d:%02d left  %.1f MB/s" % (
                    left // 60, left % 60, rate / 1048576.0
                )
        heading = {1: "IMPORTING", 2: "SETUP FAILED", 3: "READY"}.get(
            state, "IMPORTING"
        )
        bar_line = "[%s] %3d%%" % (bar, overall // 10)

        def fitted(value):
            # No strip: art rows are padded to a common width so the block
            # centres as one piece; stripping re-centred each row alone and
            # sheared the letterforms a column sideways.
            value = str(value).rstrip("\n")
            if len(value) > safe_width:
                suffix = "..."  # U+2026 is not in console fonts; see the art note
                if safe_width <= len(suffix):
                    return value[:safe_width]
                value = value[:safe_width - len(suffix)] + suffix
            return value

        if safe_height >= 18:
            body_values = (
                [""] + list(logo) + ["", subtitle, "", heading, self.title, "",
                 bar_line, "", message]
            )
        else:
            body_values = (
                list(logo) + [subtitle, heading, self.title,
                              bar_line, message]
            )
        if detail:
            body_values.append(detail)
        if eta:
            body_values.append(eta)
        body = [fitted(line) for line in body_values]

        separator_glyph = "─" if branded else "-"
        separator_width = min(safe_width, max(len(line) for line in signature))
        footer = [fitted(separator_glyph * separator_width)]
        footer.extend(fitted(line) for line in signature)

        # Preserve the identity and progress bar on short terminals, dropping
        # secondary status text before allowing the block to reach an edge.
        if len(body) + len(footer) > safe_height:
            compact = list(logo) + [subtitle, heading, self.title, bar_line, message]
            if detail:
                compact.append(detail)
            if eta:
                compact.append(eta)
            body = [fitted(line) for line in compact]

        def drop_last(value):
            for index in range(len(body) - 1, -1, -1):
                if body[index] == fitted(value):
                    body.pop(index)
                    return

        for optional in (eta, detail, message):
            if optional and len(body) + len(footer) > safe_height:
                drop_last(optional)
        if len(body) + len(footer) > safe_height and len(footer) > 1:
            footer.pop(0)
        if len(body) + len(footer) > safe_height and len(footer) > 1:
            footer = [fitted("BY EAPRULES")]
        if len(body) + len(footer) > safe_height and len(logo) > 1:
            body = [fitted("EAPX")] + body[len(logo):]
        for optional in (subtitle, self.title, heading, "EAPX", bar_line):
            if len(body) + len(footer) > safe_height:
                drop_last(optional)

        block = body + footer
        first_row = top_margin + (safe_height - len(block)) // 2 + 1
        visible_rows = range(top_margin + 1, rows - bottom_margin + 1)
        rendered = []
        for row in visible_rows:
            rendered.append("\033[%d;1H\033[2K" % row)
        for offset, value in enumerate(block):
            if not value:
                continue
            column = side_margin + (safe_width - len(value)) // 2 + 1
            rendered.append("\033[%d;%dH%s" % (first_row + offset, column, value))
        frame = "".join(rendered)
        geometry_changed = self.tty_geometry != (columns, rows)
        prefix = "\033[?25l\033[H\033[2J" if (
            not self.tty_started or geometry_changed
        ) else "\033[?25l"
        suffix = "\033[?25h" if state in (2, 3) else ""
        try:
            self.tty.write(prefix + frame + suffix)
            self.tty.flush()
            self.tty_started = True
            self.tty_geometry = (columns, rows)
        except (OSError, UnicodeError):
            self.tty = None

    def update(self, state=1, overall=0, message="", detail="", force=False):
        record = (state, int(overall), clamp_text(message), clamp_text(detail))
        now = time.time()
        if not force and record == self.last:
            return
        if not force and now - self.last_write < 0.08:
            return
        self.last = record
        self.last_write = now
        self._render(*record)
        if self.portmaster is not None and state == 1:
            if message and message != self.spoken and under_patcher():
                self.spoken = message
                sys.stdout.write(message.capitalize() + "\n")
                sys.stdout.flush()
            if self.total_bytes:
                self.portmaster.progress(
                    message, self.done_bytes, self.total_bytes
                )
        if not self.path:
            return
        payload = "EAPX1 %d %d %d %d\n%s\n%s\n" % (
            record[0],
            record[1],
            self.done_bytes,
            self.total_bytes,
            record[2],
            record[3],
        )
        try:
            atomic_write(self.path, payload, sync=False)
        except OSError:
            pass

    def fail(self, message):
        self.update(state=2, overall=0, message=message, force=True)

    def done(self, message="GAME DATA READY"):
        self.update(state=3, overall=1000, message=message, force=True)


# --------------------------------------------------------------------------
# recipe
# --------------------------------------------------------------------------

ROOT_KEYS = {
    "$schema", "schema", "id", "version", "title", "abi_order", "input", "extract",
    "hooks", "validate", "profiles", "commit", "marker", "space", "log",
    "placeholder", "requires_eapx",
}
SEMANTIC_KEYS = {
    "extract", "hooks", "validate", "profiles", "commit", "abi_order",
    "input", "schema",
}
RULE_KEYS = {"id", "description", "required", "destination", "source", "validate"}
SOURCE_KEYS = {"kind", "patterns", "strip_prefix"}
HOOK_KEYS = {"id", "argv", "cwd", "env", "timeout_seconds", "checkpoint"}
FILE_VALIDATORS = {
    "size", "min_size", "max_size", "sha256", "critical_regions",
    "elf_machine",
}
TREE_VALIDATORS = {"min_files", "max_files", "min_bytes", "max_bytes"}
VALIDATE_KEYS = FILE_VALIDATORS | TREE_VALIDATORS | {"path"}
COMMIT_KEYS = {"path", "exclusive"}
PROFILE_KEYS = {"id", "description", "validate"}
CRITICAL_REGIONS_KEYS = {"regions", "sha256"}
CRITICAL_REGION_KEYS = {"offset", "size"}
SOURCE_KINDS = {"entry", "entries", "blob"}
ID_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
ENV_PATTERN = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
REQUIRES_EAPX_PATTERN = re.compile(r"^>=(\d+)\.(\d+)\.(\d+)$")


def check_keys(mapping, allowed, label):
    """Strict whitelist. A typo must be an error, never a silently disabled check.

    For a format whose entire thesis is 'we validate by content', a mistyped
    validator key that passes the checker and validates nothing is the number
    one hole in practice.
    """
    if not isinstance(mapping, dict):
        raise RecipeError("%s must be an object" % label)
    unknown = sorted(set(mapping) - allowed)
    if not unknown:
        return
    hints = []
    for key in unknown:
        near = [candidate for candidate in allowed if _close(key, candidate)]
        if near:
            hints.append("%s (did you mean %s?)" % (key, " or ".join(sorted(near))))
        else:
            hints.append(key)
    raise RecipeError("%s has unknown key(s): %s" % (label, ", ".join(hints)))


def _close(a, b):
    if abs(len(a) - len(b)) > 2:
        return False
    a_low, b_low = a.lower().replace("_", ""), b.lower().replace("_", "")
    if a_low == b_low:
        return True
    if len(a_low) == len(b_low):
        return sum(1 for x, y in zip(a_low, b_low) if x != y) <= 1
    shorter, longer = (a_low, b_low) if len(a_low) < len(b_low) else (b_low, a_low)
    return longer.startswith(shorter) or shorter in longer


def want_bool(mapping, key, default, label):
    value = mapping.get(key, default)
    if not isinstance(value, bool):
        raise RecipeError("%s.%s must be true or false, got %r" % (label, key, value))
    return value


def want_int(mapping, key, label, minimum=0):
    value = mapping[key]
    if isinstance(value, bool) or not isinstance(value, int):
        raise RecipeError("%s.%s must be an integer, got %r" % (label, key, value))
    if value < minimum:
        raise RecipeError("%s.%s must be >= %d" % (label, key, minimum))
    return value


def parse_engine_version(value, label):
    match = REQUIRES_EAPX_PATTERN.match(value) if isinstance(value, str) else None
    if not match:
        raise RecipeError("%s must use the form >=MAJOR.MINOR.PATCH" % label)
    return tuple(int(component) for component in match.groups())


class Recipe:
    def __init__(self, path):
        self.path = os.path.realpath(path)
        self.directory = os.path.dirname(self.path)
        try:
            self.data = load_json(self.path)
        except (OSError, ValueError) as error:
            raise RecipeError("cannot read recipe %s: %s" % (path, error))
        if not isinstance(self.data, dict):
            raise RecipeError("recipe must be a JSON object")
        self._validate()
        self.digest = hashlib.sha256(canonical_json(self.data).encode("utf-8")).hexdigest()
        semantic = {k: v for k, v in self.data.items() if k in SEMANTIC_KEYS}
        self.semantic_digest = hashlib.sha256(
            canonical_json(semantic).encode("utf-8")
        ).hexdigest()

    def _validate(self):
        data = self.data
        check_keys(data, ROOT_KEYS, "recipe")
        if data.get("schema") != FORMAT_VERSION:
            raise RecipeError("recipe schema must be %d" % FORMAT_VERSION)
        self.identifier = data.get("id")
        if not isinstance(self.identifier, str) or not ID_PATTERN.match(self.identifier):
            raise RecipeError("recipe id is missing or invalid: %r" % (self.identifier,))
        version = data.get("version")
        if isinstance(version, bool) or not isinstance(version, (str, int)):
            raise RecipeError("recipe version must be a string or integer")
        self.version = str(version)
        self.title = data.get("title", self.identifier)
        if not isinstance(self.title, str) or not self.title.strip():
            raise RecipeError("recipe title must be a non-empty string")

        self.requires_eapx = data.get("requires_eapx")
        if self.requires_eapx is not None:
            required = parse_engine_version(self.requires_eapx, "requires_eapx")
            current = tuple(int(component) for component in VERSION.split("."))
            if current < required:
                raise RecipeError(
                    "recipe requires eapx %s, but this engine is %s"
                    % (self.requires_eapx, VERSION)
                )

        self.abi_order = data.get("abi_order") or ["arm64-v8a", "armeabi-v7a"]
        if (
            not isinstance(self.abi_order, list)
            or not self.abi_order
            or not all(isinstance(a, str) and a for a in self.abi_order)
        ):
            raise RecipeError("abi_order must be a non-empty list of strings")

        source_dirs = data.get("input", {})
        check_keys(source_dirs, {"search_dirs"}, "input")
        self.search_dirs = source_dirs.get("search_dirs") or ["gamedata", "."]
        if not isinstance(self.search_dirs, list) or not self.search_dirs:
            raise RecipeError("input.search_dirs must be a non-empty list")
        for entry in self.search_dirs:
            if entry != "." :
                validate_relative_path(entry, "input.search_dirs entry")

        self.rules = self._validate_rules(data.get("extract"))
        self.hooks = self._validate_hooks(data.get("hooks", []))
        self.commit = self._validate_commit(data.get("commit"))
        self.output_checks = self._validate_output_checks(data.get("validate", []))
        self.profiles = self._validate_profiles(data.get("profiles", []))

        self.marker = data.get("marker", ".eapx-%s.json" % self.identifier)
        validate_relative_path(self.marker, "marker")
        self.log = data.get("log", "eapx.log")
        validate_relative_path(self.log, "log")
        self.placeholder = data.get("placeholder")
        if self.placeholder is not None:
            validate_relative_path(self.placeholder, "placeholder")

        space = data.get("space", {})
        check_keys(space, {"safety_bytes"}, "space")
        self.safety_bytes = (
            want_int(space, "safety_bytes", "space") if "safety_bytes" in space
            else DEFAULT_SAFETY_BYTES
        )
        self._check_layout()

    def _validate_validation(self, spec, label, kind):
        check_keys(spec, VALIDATE_KEYS, label)
        used_file = set(spec) & FILE_VALIDATORS
        if kind == "entries" and used_file:
            # In the design we inherited these were accepted and then ignored:
            # a sha256 on a tree rule validated nothing at all.
            raise RecipeError(
                "%s: %s only apply to single-file rules; this rule extracts a tree"
                % (label, ", ".join(sorted(used_file)))
            )
        for key in ("size", "min_size", "max_size", "min_files", "max_files",
                    "min_bytes", "max_bytes"):
            if key in spec:
                want_int(spec, key, label)
        if "sha256" in spec:
            digests = spec["sha256"]
            if isinstance(digests, str):
                digests = [digests]
            if not isinstance(digests, list) or not digests:
                raise RecipeError("%s.sha256 must be a hex string or list" % label)
            for digest in digests:
                if not isinstance(digest, str) or not re.match(r"^[0-9a-fA-F]{64}$", digest):
                    raise RecipeError("%s.sha256 entries must be 64 hex chars" % label)
            spec["sha256"] = [d.lower() for d in digests]
        if "critical_regions" in spec:
            if "size" not in spec:
                raise RecipeError(
                    "%s.critical_regions requires an exact size validator" % label
                )
            critical = spec["critical_regions"]
            critical_label = label + ".critical_regions"
            check_keys(critical, CRITICAL_REGIONS_KEYS, critical_label)
            regions = critical.get("regions")
            if not isinstance(regions, list) or not regions:
                raise RecipeError("%s.regions must be a non-empty list" % critical_label)
            normalised = []
            for index, region in enumerate(regions):
                region_label = "%s.regions[%d]" % (critical_label, index)
                check_keys(region, CRITICAL_REGION_KEYS, region_label)
                offset = region.get("offset")
                if isinstance(offset, str) and re.match(r"^0x[0-9a-fA-F]+$", offset):
                    offset = int(offset, 16)
                if isinstance(offset, bool) or not isinstance(offset, int) or offset < 0:
                    raise RecipeError(
                        "%s.offset must be a non-negative integer or 0x hex string"
                        % region_label
                    )
                size = region.get("size")
                if isinstance(size, bool) or not isinstance(size, int) or size <= 0:
                    raise RecipeError("%s.size must be a positive integer" % region_label)
                if offset + size > spec["size"]:
                    raise RecipeError(
                        "%s extends past the declared file size %d"
                        % (region_label, spec["size"])
                    )
                normalised.append({"offset": offset, "size": size})
            digest = critical.get("sha256")
            if not isinstance(digest, str) or not re.match(
                r"^[0-9a-fA-F]{64}$", digest
            ):
                raise RecipeError("%s.sha256 must be 64 hex chars" % critical_label)
            critical["regions"] = normalised
            critical["sha256"] = digest.lower()
        if "elf_machine" in spec:
            value = spec["elf_machine"]
            if not isinstance(value, str):
                raise RecipeError("%s.elf_machine must be a string" % label)
            if value != "{abi}" and value not in ELF_ABIS:
                raise RecipeError(
                    "%s.elf_machine must be '{abi}' or one of %s"
                    % (label, ", ".join(sorted(ELF_ABIS)))
                )
        # Contradictions guarantee nothing ever matches, and the error the user
        # then sees blames the APK instead of the recipe.
        low, high = spec.get("min_size"), spec.get("max_size")
        if low is not None and high is not None and low > high:
            raise RecipeError("%s: min_size > max_size" % label)
        exact = spec.get("size")
        if exact is not None:
            if low is not None and exact < low:
                raise RecipeError("%s: size is below min_size" % label)
            if high is not None and exact > high:
                raise RecipeError("%s: size is above max_size" % label)
        if (spec.get("min_files") is not None and spec.get("max_files") is not None
                and spec["min_files"] > spec["max_files"]):
            raise RecipeError("%s: min_files > max_files" % label)
        if (spec.get("min_bytes") is not None and spec.get("max_bytes") is not None
                and spec["min_bytes"] > spec["max_bytes"]):
            raise RecipeError("%s: min_bytes > max_bytes" % label)
        return spec

    def _validate_rules(self, rules):
        if not isinstance(rules, list) or not rules:
            raise RecipeError("recipe.extract must be a non-empty list")
        seen = set()
        result = []
        for index, rule in enumerate(rules):
            label = "extract[%d]" % index
            check_keys(rule, RULE_KEYS, label)
            identifier = rule.get("id")
            if not isinstance(identifier, str) or not ID_PATTERN.match(identifier):
                raise RecipeError("%s.id is missing or invalid" % label)
            if identifier in seen:
                raise RecipeError("duplicate extract rule id: %s" % identifier)
            seen.add(identifier)
            label = "extract[%s]" % identifier

            source = rule.get("source")
            check_keys(source or {}, SOURCE_KEYS, label + ".source")
            kind = (source or {}).get("kind")
            if kind not in SOURCE_KINDS:
                raise RecipeError(
                    "%s.source.kind must be one of %s"
                    % (label, ", ".join(sorted(SOURCE_KINDS)))
                )
            patterns = source.get("patterns")
            if not isinstance(patterns, list) or not patterns:
                raise RecipeError("%s.source.patterns must be a non-empty list" % label)
            for pattern in patterns:
                if not isinstance(pattern, str) or not pattern:
                    raise RecipeError("%s.source.patterns entries must be strings" % label)
            strip = source.get("strip_prefix")
            if strip is not None and (not isinstance(strip, str) or not strip):
                raise RecipeError("%s.source.strip_prefix must be a string" % label)
            if strip is not None and kind != "entries":
                raise RecipeError("%s.source.strip_prefix only applies to 'entries'" % label)

            destination = rule.get("destination")
            validate_relative_path(
                template(destination, "ABI") if isinstance(destination, str) else "",
                label + ".destination",
            )
            validation = self._validate_validation(
                rule.get("validate", {}), label + ".validate", kind
            )
            result.append({
                "id": identifier,
                "description": rule.get("description", identifier),
                "required": want_bool(rule, "required", True, label),
                "destination": destination,
                "kind": kind,
                "patterns": patterns,
                "strip_prefix": strip,
                "validate": validation,
            })
        return result

    def _validate_hooks(self, hooks):
        if not isinstance(hooks, list):
            raise RecipeError("recipe.hooks must be a list")
        seen = set()
        result = []
        for index, hook in enumerate(hooks):
            label = "hooks[%d]" % index
            check_keys(hook, HOOK_KEYS, label)
            identifier = hook.get("id")
            if not isinstance(identifier, str) or not ID_PATTERN.match(identifier):
                raise RecipeError("%s.id is missing or invalid" % label)
            if identifier in seen:
                raise RecipeError("duplicate hook id: %s" % identifier)
            seen.add(identifier)
            argv = hook.get("argv")
            if (not isinstance(argv, list) or not argv
                    or not all(isinstance(a, str) and a for a in argv)):
                raise RecipeError("%s.argv must be a non-empty list of strings" % label)
            environment = hook.get("env", {})
            if not isinstance(environment, dict):
                raise RecipeError("%s.env must be an object" % label)
            for name, value in environment.items():
                if not ENV_PATTERN.match(name):
                    raise RecipeError("%s.env has an invalid name: %r" % (label, name))
                if not isinstance(value, str):
                    raise RecipeError("%s.env values must be strings" % label)
            timeout = hook.get("timeout_seconds", DEFAULT_HOOK_TIMEOUT)
            if isinstance(timeout, bool) or not isinstance(timeout, int) or timeout <= 0:
                raise RecipeError("%s.timeout_seconds must be a positive integer" % label)
            checkpoints = hook.get("checkpoint", [])
            if not isinstance(checkpoints, list):
                raise RecipeError("%s.checkpoint must be a list" % label)
            for position, check in enumerate(checkpoints):
                sub = "%s.checkpoint[%d]" % (label, position)
                validate_relative_path(
                    template(check.get("path", ""), "ABI"), sub + ".path"
                )
                self._validate_validation(check, sub, "entry")
            result.append({
                "id": identifier,
                "argv": argv,
                "cwd": hook.get("cwd", "{game_dir}"),
                "env": environment,
                "timeout_seconds": timeout,
                "checkpoint": checkpoints,
            })
        return result

    def _validate_commit(self, commit):
        if not isinstance(commit, list) or not commit:
            raise RecipeError("recipe.commit must be a non-empty list")
        result = []
        for index, entry in enumerate(commit):
            label = "commit[%d]" % index
            if isinstance(entry, str):
                entry = {"path": entry}
            check_keys(entry, COMMIT_KEYS, label)
            path = entry.get("path")
            validate_relative_path(
                template(path, "ABI") if isinstance(path, str) else "", label + ".path"
            )
            result.append({
                "path": path,
                "exclusive": want_bool(entry, "exclusive", False, label),
            })
        return result

    def _validate_output_checks(self, checks):
        if not isinstance(checks, list):
            raise RecipeError("recipe.validate must be a list")
        result = []
        for index, check in enumerate(checks):
            label = "validate[%d]" % index
            path = check.get("path")
            validate_relative_path(
                template(path, "ABI") if isinstance(path, str) else "", label + ".path"
            )
            spec = dict(check)
            spec.pop("path", None)
            kind = "entries" if not (set(spec) & FILE_VALIDATORS) else "entry"
            self._validate_validation(spec, label, kind)
            result.append({"path": path, "spec": spec})
        return result

    def _validate_profiles(self, profiles):
        """Validate correlated output fingerprints for supported donors."""
        if not isinstance(profiles, list):
            raise RecipeError("recipe.profiles must be a list")
        seen = set()
        result = []
        for index, profile in enumerate(profiles):
            label = "profiles[%d]" % index
            check_keys(profile, PROFILE_KEYS, label)
            identifier = profile.get("id")
            if not isinstance(identifier, str) or not ID_PATTERN.match(identifier):
                raise RecipeError("%s.id is missing or invalid" % label)
            if identifier in seen:
                raise RecipeError("duplicate profile id: %s" % identifier)
            seen.add(identifier)
            checks = profile.get("validate")
            if not isinstance(checks, list) or not checks:
                raise RecipeError("%s.validate must be a non-empty list" % label)
            result.append({
                "id": identifier,
                "description": profile.get("description", identifier),
                "validate": self._validate_output_checks(checks),
            })
        return result

    def _check_layout(self):
        """Cross-field checks that must not wait until runtime."""
        for abi in self.abi_order:
            roots = [template(entry["path"], abi) for entry in self.commit]
            for index, first in enumerate(roots):
                for second in roots[index + 1:]:
                    if first == second or under(first, second) or under(second, first):
                        raise RecipeError(
                            "commit roots overlap for abi %s: %s and %s"
                            % (abi, first, second)
                        )
            for rule in self.rules:
                destination = template(rule["destination"], abi)
                if not any(under(destination, root) or destination == root
                           for root in roots):
                    raise RecipeError(
                        "extract[%s] writes to %s which is outside every commit root "
                        "(%s) for abi %s"
                        % (rule["id"], destination, ", ".join(roots), abi)
                    )
            # A marker or log living under a commit root is destroyed on every
            # reinstall, and the fast path then silently never works again.
            for name, value in (("marker", self.marker), ("log", self.log)):
                if any(under(value, root) or value == root for root in roots):
                    raise RecipeError(
                        "%s (%s) must not live under a commit root" % (name, value)
                    )

    def commit_roots(self, abi):
        return [
            {"path": template(entry["path"], abi), "exclusive": entry["exclusive"]}
            for entry in self.commit
        ]


def template(value, abi):
    """Literal substitution of the known keys only.

    str.format would let a recipe navigate __class__ / __globals__ from any
    value; against an already-trusted recipe that adds no power, but it is a
    free gadget not worth handing out.
    """
    if not isinstance(value, str):
        return value
    return value.replace("{abi}", abi)


def under(path, root):
    return path.startswith(root.rstrip("/") + "/")


# --------------------------------------------------------------------------
# archives and discovery
# --------------------------------------------------------------------------


class Archive:
    """A zip opened exactly once.

    The design we inherited opened each candidate three times -- is_zipfile,
    classify, then read -- and in a 2 GB XAPK the central directory lives at
    the end of the file, so each open is an expensive seek on SD.
    """

    def __init__(self, path, warn=None):
        self.path = os.path.realpath(path)
        self.zip = zipfile.ZipFile(self.path)
        self.entries = {}
        label = os.path.basename(self.path)

        def skip(name, reason):
            if warn is not None:
                warn("%s: ignoring entry %r (%s)" % (label, name, reason))

        for info in self.zip.infolist():
            if info.is_dir():
                continue
            # A single hostile or malformed entry disqualifies that entry, not
            # the whole archive: an otherwise good APK must stay usable.
            name = safe_zip_name(info.filename)
            if name is None:
                skip(info.filename, "unsafe name")
                continue
            if name in self.entries:
                skip(name, "duplicate")
                continue
            if info.flag_bits & 0x1:
                skip(name, "encrypted")
                continue
            if stat.S_ISLNK(info.external_attr >> 16):
                skip(name, "symlink")
                continue
            self.entries[name] = info
        self.is_apk = "AndroidManifest.xml" in self.entries
        self.inner_apks = sorted(
            name for name in self.entries if name.lower().endswith(".apk")
        )
        self._package = False

    @property
    def package(self):
        """Android package name, read once and cached.

        Splits of one game all declare the same package, which is how loose
        base.apk + config.arm64_v8a.apk files get recognised as a single game
        without anyone relying on their filenames.
        """
        if self._package is False:
            self._package = None
            if self.is_apk:
                try:
                    info = self.entries["AndroidManifest.xml"]
                    if info.file_size <= 4 << 20:
                        with self.open("AndroidManifest.xml") as stream:
                            self._package = android_package(stream.read())
                except (OSError, KeyError, zipfile.BadZipFile):
                    self._package = None
        return self._package

    def info(self, name):
        return self.entries[name]

    def open(self, name):
        return self.zip.open(self.entries[name])

    def close(self):
        try:
            self.zip.close()
        except Exception:
            pass


def _u16(data, offset):
    if offset + 2 > len(data):
        raise SourceError("truncated Android manifest")
    return struct.unpack_from("<H", data, offset)[0]


def _u32(data, offset):
    if offset + 4 > len(data):
        raise SourceError("truncated Android manifest")
    return struct.unpack_from("<I", data, offset)[0]


def _pool_string(data, pool_start, offsets, index, utf8):
    """Read one entry from a binary XML string pool, bounds-checked throughout.

    Every read here is on attacker-shaped input. The parser this replaces let
    a truncated pool escape as IndexError or struct.error, which no handler
    caught, so a malformed APK printed a Python traceback on the console.
    """
    if index < 0 or index >= len(offsets):
        return None
    position = pool_start + offsets[index]
    if position < 0 or position >= len(data):
        return None
    if utf8:
        length = data[position]
        position += 1
        if length & 0x80:
            length = ((length & 0x7F) << 8) | data[position]
            position += 1
        length = data[position]  # byte length follows the character length
        position += 1
        if length & 0x80:
            length = ((length & 0x7F) << 8) | data[position]
            position += 1
        return data[position:position + length].decode("utf-8", "replace")
    length = _u16(data, position)
    position += 2
    if length & 0x8000:
        length = ((length & 0x7FFF) << 16) | _u16(data, position)
        position += 2
    return data[position:position + length * 2].decode("utf-16-le", "replace")


def android_package(data):
    """Extract the package name from a binary AndroidManifest.xml.

    Returns None for anything we cannot read with confidence -- an unknown
    package is a grouping hint we do without, never a reason to fail.
    """
    try:
        if len(data) < 8 or _u16(data, 0) != 0x0003:
            return None
        offsets, pool_start, utf8 = [], 0, False
        position = _u16(data, 2)
        limit = min(_u32(data, 4), len(data))
        while position + 8 <= limit:
            chunk_type = _u16(data, position)
            header_size = _u16(data, position + 2)
            chunk_size = _u32(data, position + 4)
            if chunk_size < 8 or header_size < 8:
                return None

            if chunk_type == 0x0001 and not offsets:
                count = _u32(data, position + 8)
                flags = _u32(data, position + 16)
                strings_start = _u32(data, position + 20)
                if count > 200000:
                    return None
                utf8 = bool(flags & 0x100)
                pool_start = position + strings_start
                offsets = [
                    _u32(data, position + header_size + i * 4) for i in range(count)
                ]

            elif chunk_type == 0x0102:  # START_ELEMENT
                name_index = _u32(data, position + 20)
                if _pool_string(data, pool_start, offsets, name_index, utf8) == "manifest":
                    attribute_start = _u16(data, position + 24)
                    attribute_size = _u16(data, position + 26)
                    attribute_count = _u16(data, position + 28)
                    # attributeStart counts from the extended header, which
                    # itself begins where the chunk header ends.
                    base = position + header_size + attribute_start
                    for index in range(min(attribute_count, 256)):
                        entry = base + index * attribute_size
                        key = _pool_string(
                            data, pool_start, offsets, _u32(data, entry + 4), utf8
                        )
                        if key != "package":
                            continue
                        value = _pool_string(
                            data, pool_start, offsets, _u32(data, entry + 8), utf8
                        )
                        return value or None
                    return None

            position += chunk_size
    except (SourceError, IndexError, struct.error, UnicodeDecodeError, ValueError):
        return None
    return None


class TreeEntry:
    def __init__(self, size):
        self.file_size = size
        self.CRC = None  # a directory carries no stored checksum


class Tree:
    """A plain directory used as a package source.

    Some donors are not distributed as a zip at all. Rather than grow archive
    dependencies, a port can unpack them before invoking the engine. From here
    a directory behaves exactly like an archive, so every rule kind works
    against it unchanged.
    """

    def __init__(self, path):
        self.path = os.path.realpath(path)
        self.entries = {}
        for base, directories, files in os.walk(self.path, followlinks=False):
            directories[:] = sorted(d for d in directories if d != ".eapx")
            for name in sorted(files):
                full = os.path.join(base, name)
                if not is_regular_file(full):
                    continue
                relative = os.path.relpath(full, self.path).replace(os.sep, "/")
                # macOS writes AppleDouble sidecars on FAT volumes; a card
                # prepared on a Mac would otherwise carry 1795 phantom files.
                if os.path.basename(relative).startswith("._"):
                    continue
                if safe_zip_name(relative) is None:
                    continue
                self.entries[relative] = TreeEntry(os.path.getsize(full))
        self.is_apk = "AndroidManifest.xml" in self.entries
        self.inner_apks = []  # an unpacked folder is already flat

    @property
    def package(self):
        if not self.is_apk:
            return None
        try:
            with self.open("AndroidManifest.xml") as stream:
                return android_package(stream.read(4 << 20))
        except OSError:
            return None

    def info(self, name):
        return self.entries[name]

    def open(self, name):
        return open(os.path.join(self.path, name), "rb")

    def close(self):
        pass


class Candidate:
    """A discovered input. It can act as a container, a blob, or both.

    Collapsing 'archive' and 'loose' into one concept is what makes a
    zip-format OBB -- which is most Unity and Unreal OBBs -- reachable by a
    rule that wants to copy it whole. Previously such a file was classified as
    a container and a blob rule could never find it.
    """

    def __init__(self, path, archive=None):
        self.path = os.path.realpath(path)
        self.name = os.path.basename(path)
        self.archive = archive
        if isinstance(archive, Tree):
            self.size = sum(e.file_size for e in archive.entries.values())
        else:
            self.size = os.path.getsize(self.path)

    @property
    def is_container(self):
        return self.archive is not None

    def __repr__(self):
        return "<Candidate %s>" % self.name


def discover(recipe, game_dir, explicit, logger):
    """Union of every search dir. No first-non-empty short circuit.

    A stray .gitkeep in gamedata/ used to stop the scan and make the APK
    sitting in the game dir invisible, reporting 'no package found' while the
    user was looking straight at it.
    """
    paths = []
    search_roots = set()
    for entry in recipe.search_dirs:
        search_roots.add(
            game_dir if entry == "." else safe_join(game_dir, entry, "search dir")
        )
    if explicit:
        paths = [os.path.realpath(p) for p in explicit]
    else:
        for entry in recipe.search_dirs:
            directory = game_dir if entry == "." else safe_join(game_dir, entry, "search dir")
            if not os.path.isdir(directory) or os.path.islink(directory):
                continue
            for name in sorted(os.listdir(directory), key=lambda s: s.casefold()):
                full = os.path.join(directory, name)
                if name == ".eapx" or os.path.islink(full):
                    continue
                if is_regular_file(full) or os.path.isdir(full):
                    paths.append(os.path.realpath(full))

    seen = set()
    candidates = []
    workspace_marker = os.path.join(game_dir, ".eapx")
    # Our own bookkeeping files live in the game dir; they are never input.
    excluded = {
        safe_join(game_dir, recipe.marker, "marker"),
        safe_join(game_dir, recipe.log, "log"),
    }
    for path in paths:
        if path in seen or path in excluded or path.startswith(workspace_marker):
            continue
        # A search directory is where we look, not something we look at.
        if path in search_roots:
            continue
        seen.add(path)
        archive = None
        try:
            if os.path.isdir(path):
                archive = Tree(path)
                if not archive.entries:
                    continue
            elif zipfile.is_zipfile(path):
                archive = Archive(path, warn=logger.log)
        except (OSError, ValueError, IndexError, struct.error,
                zipfile.BadZipFile, SourceError) as error:
            # One bad file must not poison the run: a corrupt zip sitting next
            # to the good XAPK used to abort the whole installation.
            logger.log("skipping %s: %s" % (os.path.basename(path), error))
            continue
        candidates.append(Candidate(path, archive))
    logger.log("discovered %d candidate file(s)" % len(candidates))
    return candidates


# --------------------------------------------------------------------------
# planning
# --------------------------------------------------------------------------


class Item:
    """One file to publish.

    Identity is (size, crc) because a zip's central directory already carries
    both for free. Hashing every member just to build a plan meant reading the
    whole payload once before extraction read it again -- on a 20 MB/s card
    that is a wasted pass over hundreds of megabytes. sha256 is filled in
    during extraction, where the bytes are flowing anyway, and only computed up
    front when the recipe explicitly asks for it.
    """

    def __init__(self, rule_id, destination, candidate, member, size, crc,
                 sha256=None):
        self.rule_id = rule_id
        self.destination = destination
        self.candidate = candidate
        self.member = member
        self.size = size
        self.crc = crc
        self.sha256 = sha256

    @property
    def identity(self):
        return (self.size, self.crc)

    @property
    def key(self):
        return (self.rule_id, self.destination, self.size, self.crc)


class Plan:
    def __init__(self, abi, items, roots):
        self.abi = abi
        self.items = sorted(items, key=lambda item: item.destination.casefold())
        self.roots = roots
        self.profile = None

    @property
    def fingerprint(self):
        payload = canonical_json([list(item.key) for item in self.items])
        return hashlib.sha256(payload.encode("utf-8")).hexdigest()

    @property
    def total_bytes(self):
        return sum(item.size for item in self.items)


def matches(name, pattern):
    return fnmatch.fnmatchcase(name, pattern)


def check_size_spec(spec, size):
    if "size" in spec and size != spec["size"]:
        return "size %d, expected %d" % (size, spec["size"])
    if "min_size" in spec and size < spec["min_size"]:
        return "size %d below min_size %d" % (size, spec["min_size"])
    if "max_size" in spec and size > spec["max_size"]:
        return "size %d above max_size %d" % (size, spec["max_size"])
    return None


def needs_critical_regions(spec, sha256):
    return (
        "critical_regions" in spec
        and ("sha256" not in spec or sha256 not in spec["sha256"])
    )


def check_file_spec(spec, size, sha256, critical_sha256, header, abi, label):
    """Return None when the candidate passes, or a human reason when it does not."""
    size_reason = check_size_spec(spec, size)
    if size_reason:
        return size_reason
    if "elf_machine" in spec:
        expected = spec["elf_machine"]
        expected = abi if expected == "{abi}" else expected
        wanted = ELF_ABIS.get(expected)
        if wanted is None:
            return "unknown elf_machine %s" % expected
        identity = elf_identity(header or b"")
        if identity is None:
            return "not an ELF object"
        if identity[0] != wanted[0]:
            return "ELF machine %d, expected %d for %s" % (identity[0], wanted[0], expected)
        if identity[1] != wanted[1]:
            bits = {1: 32, 2: 64}
            return "ELF is %s-bit, expected %s-bit for %s" % (
                bits.get(identity[1], "?"), bits.get(wanted[1], "?"), expected
            )
    if "sha256" in spec and sha256 in spec["sha256"]:
        return None
    if "critical_regions" in spec:
        expected = spec["critical_regions"]["sha256"]
        if critical_sha256 == expected:
            return None
        actual = (critical_sha256 or "not-evaluated")[:12]
        return (
            "size ok, sha256 %s is unknown and critical_regions sha256 %s "
            "does not match; donor differs in bytes required by this port"
            % ((sha256 or "unavailable")[:12], actual)
        )
    if "sha256" in spec and sha256 not in spec["sha256"]:
        return "sha256 %s not in the accepted list" % sha256[:12]
    return None


def read_header(candidate, member, length=64):
    try:
        if member is None:
            with open(candidate.path, "rb") as stream:
                return stream.read(length)
        with candidate.archive.open(member) as stream:
            return stream.read(length)
    except (OSError, zipfile.BadZipFile):
        return b""


def build_plan(recipe, group, abi, digests, logger):
    """Build the plan for one candidate group at one ABI."""
    roots = recipe.commit_roots(abi)
    items = []
    for rule in recipe.rules:
        found, rejected = collect_rule(rule, group, abi, digests, logger)
        if not found:
            if rule["required"]:
                detail = "; ".join(rejected[:3]) if rejected else "no entry matched"
                raise PlanError(
                    "%s (%s) found nothing for abi %s. Patterns: %s. %s"
                    % (rule["id"], rule["description"], abi,
                       ", ".join(rule["patterns"]), detail)
                )
            continue
        items.extend(found)

    destinations = {}
    for item in items:
        key = item.destination.casefold()
        previous = destinations.get(key)
        if previous is not None and previous.identity != item.identity:
            raise PlanError(
                "rules %s and %s both write %s with different content"
                % (previous.rule_id, item.rule_id, item.destination)
            )
        destinations[key] = item
        if not any(item.destination == r["path"] or under(item.destination, r["path"])
                   for r in roots):
            raise PlanError(
                "%s writes outside every commit root" % item.destination
            )
    return Plan(abi, list(destinations.values()), roots)


def collect_rule(rule, group, abi, digests, logger):
    kind = rule["kind"]
    patterns = [template(p, abi) for p in rule["patterns"]]
    spec = rule["validate"]
    rejected = []

    wants_hash = "sha256" in spec or "critical_regions" in spec

    if kind == "blob":
        matched = [c for c in group if any(matches(c.name, p) for p in patterns)]
        chosen = []
        for candidate in matched:
            size = candidate.size
            size_reason = check_size_spec(spec, size)
            if size_reason:
                rejected.append("%s rejected: %s" % (candidate.name, size_reason))
                continue
            # A loose file carries no stored checksum, so hashing it costs a
            # full read. Only pay for it when the recipe asks, or when there is
            # more than one candidate and we have to tell them apart.
            if wants_hash or len(matched) > 1:
                sha256, crc, size = digests.file(candidate.path)
            else:
                sha256, crc, size = None, None, candidate.size
            critical_sha256 = None
            if needs_critical_regions(spec, sha256):
                critical_sha256 = digests.file_regions(
                    candidate.path, spec["critical_regions"]["regions"]
                )
            reason = check_file_spec(
                spec, size, sha256, critical_sha256,
                read_header(candidate, None), abi, candidate.name
            )
            if reason:
                rejected.append("%s rejected: %s" % (candidate.name, reason))
                continue
            if critical_sha256 is not None:
                logger.log(
                    "accepted %s by critical regions (sha256=%s)"
                    % (candidate.name, sha256)
                )
            chosen.append(Item(rule["id"], template(rule["destination"], abi),
                               candidate, None, size, crc, sha256))
        # A blob can also live inside a container: an Android/obb backup is
        # handed over either as a zip or as that zip extracted to a folder,
        # and in both the OBB sits under <package>/. Candidate collapsed
        # 'archive' and 'loose' into one concept precisely so a rule could
        # reach content either way, but this branch only ever looked at the
        # candidate's own name -- which has no slash, so every pattern
        # written against the backup layout was unmatchable by construction.
        for candidate in group:
            if not candidate.is_container:
                continue
            for name in sorted(candidate.archive.entries):
                if not any(matches(name, p) for p in patterns):
                    continue
                info = candidate.archive.info(name)
                size, crc = info.file_size, info.CRC
                size_reason = check_size_spec(spec, size)
                if size_reason:
                    rejected.append("%s rejected: %s" % (name, size_reason))
                    continue
                sha256 = None
                if wants_hash:
                    sha256, crc, size = digests.member(candidate.archive, name)
                critical_sha256 = None
                if needs_critical_regions(spec, sha256):
                    critical_sha256 = digests.member_regions(
                        candidate.archive, name,
                        spec["critical_regions"]["regions"],
                    )
                reason = check_file_spec(
                    spec, size, sha256, critical_sha256,
                    read_header(candidate, name), abi, name
                )
                if reason:
                    rejected.append("%s rejected: %s" % (name, reason))
                    continue
                if critical_sha256 is not None:
                    logger.log(
                        "accepted %s by critical regions (sha256=%s)"
                        % (name, sha256)
                    )
                chosen.append(Item(rule["id"], template(rule["destination"], abi),
                                   candidate, name, size, crc, sha256))
        return unique_one(rule, chosen, rejected)

    if kind == "entry":
        chosen = []
        for pattern in patterns:
            for candidate in group:
                if not candidate.is_container:
                    continue
                for name in sorted(candidate.archive.entries):
                    if not matches(name, pattern):
                        continue
                    info = candidate.archive.info(name)
                    size, crc = info.file_size, info.CRC
                    size_reason = check_size_spec(spec, size)
                    if size_reason:
                        rejected.append("%s rejected: %s" % (name, size_reason))
                        continue
                    sha256 = None
                    if wants_hash:
                        sha256, crc, size = digests.member(candidate.archive, name)
                    critical_sha256 = None
                    if needs_critical_regions(spec, sha256):
                        critical_sha256 = digests.member_regions(
                            candidate.archive, name,
                            spec["critical_regions"]["regions"],
                        )
                    reason = check_file_spec(
                        spec, size, sha256, critical_sha256,
                        read_header(candidate, name), abi, name
                    )
                    if reason:
                        rejected.append("%s rejected: %s" % (name, reason))
                        continue
                    if critical_sha256 is not None:
                        logger.log(
                            "accepted %s by critical regions (sha256=%s)"
                            % (name, sha256)
                        )
                    chosen.append(Item(rule["id"], template(rule["destination"], abi),
                                       candidate, name, size, crc, sha256))
            if chosen:
                break  # patterns are a preference list; first productive one wins
        return unique_one(rule, chosen, rejected)

    # entries -- a whole tree
    strip = rule["strip_prefix"]
    strip = template(strip, abi) if strip else None
    if strip and not strip.endswith("/"):
        strip += "/"
    root = template(rule["destination"], abi)
    collected = {}
    for candidate in group:
        if not candidate.is_container:
            continue
        for name in sorted(candidate.archive.entries):
            if not any(matches(name, p) for p in patterns):
                continue
            relative = name
            if strip:
                if not name.startswith(strip):
                    continue
                relative = name[len(strip):]
            if not relative:
                continue
            destination = root + "/" + relative
            validate_relative_path(destination, "destination for %s" % rule["id"])
            info = candidate.archive.info(name)
            size, crc = info.file_size, info.CRC
            previous = collected.get(destination.casefold())
            if previous is not None:
                if previous.identity != (size, crc):
                    raise PlanError(
                        "%s: %s and %s both map to %s with different content"
                        % (rule["id"], previous.member, name, destination)
                    )
                continue
            collected[destination.casefold()] = Item(
                rule["id"], destination, candidate, name, size, crc
            )
    items = list(collected.values())
    if items:
        count, total = len(items), sum(i.size for i in items)
        reason = check_tree_spec(spec, count, total)
        if reason:
            raise PlanError("%s: %s" % (rule["id"], reason))
    return items, rejected


def unique_one(rule, chosen, rejected):
    if not chosen:
        return [], rejected
    distinct = {item.identity for item in chosen}
    if len(distinct) > 1:
        listing = ", ".join(
            sorted({"%s (%s)" % (i.member or i.candidate.name, human_bytes(i.size))
                    for i in chosen})
        )
        raise PlanError(
            "%s is ambiguous: %d different files match. Keep one and remove the "
            "others, or pass --input. Candidates: %s"
            % (rule["id"], len(distinct), listing)
        )
    return [chosen[0]], rejected


def check_tree_spec(spec, count, total):
    if "min_files" in spec and count < spec["min_files"]:
        return "%d file(s), expected at least %d" % (count, spec["min_files"])
    if "max_files" in spec and count > spec["max_files"]:
        return "%d file(s), expected at most %d" % (count, spec["max_files"])
    if "min_bytes" in spec and total < spec["min_bytes"]:
        return "%s total, expected at least %s" % (human_bytes(total),
                                                   human_bytes(spec["min_bytes"]))
    if "max_bytes" in spec and total > spec["max_bytes"]:
        return "%s total, expected at most %s" % (human_bytes(total),
                                                  human_bytes(spec["max_bytes"]))
    return None


def expand_bundles(candidates, workspace, logger, limit=64):
    """Unpack the APKs held inside an XAPK/APKM/APKS so their entries are reachable.

    No port in the PortMaster catalogue handles this today -- the user is
    expected to take the bundle apart on a PC first. A bundle is just a zip
    whose interesting members happen to be zips themselves, and Python cannot
    read a zip from a non-seekable stream, so each inner APK is materialised
    once into a cache keyed by the bundle's identity and reused afterwards.
    """
    cache_root = os.path.join(workspace, "cache")
    produced = []
    for candidate in list(candidates):
        archive = candidate.archive
        if not isinstance(archive, Archive) or archive.is_apk:
            continue
        if not archive.inner_apks:
            continue
        if len(archive.inner_apks) > limit:
            raise SourceError(
                "%s holds %d APKs, more than the %d this tool will unpack"
                % (candidate.name, len(archive.inner_apks), limit)
            )
        info = os.stat(candidate.path)
        token = hashlib.sha256(
            ("%s\0%d\0%d" % (candidate.path, info.st_size, info.st_mtime_ns))
            .encode("utf-8")
        ).hexdigest()[:16]
        directory = os.path.join(cache_root, token)
        os.makedirs(directory, exist_ok=True)
        logger.log("unpacking %d APK(s) from %s"
                   % (len(archive.inner_apks), candidate.name))
        for index, name in enumerate(archive.inner_apks):
            target = os.path.join(
                directory,
                "%03d-%s.apk" % (index, hashlib.sha256(name.encode()).hexdigest()[:10]),
            )
            expected = archive.info(name)
            if not (is_regular_file(target)
                    and os.path.getsize(target) == expected.file_size):
                partial = target + ".part"
                remove_path(partial)
                with archive.open(name) as source, open(partial, "xb") as sink:
                    shutil.copyfileobj(source, sink, CHUNK_SIZE)
                if os.path.getsize(partial) != expected.file_size:
                    remove_path(partial)
                    raise SourceError("%s: inner APK %s is truncated"
                                      % (candidate.name, name))
                os.replace(partial, target)
            try:
                inner = Archive(target, warn=logger.log)
            except (OSError, ValueError, zipfile.BadZipFile, SourceError) as error:
                logger.log("ignoring inner APK %s: %s" % (name, error))
                continue
            if not inner.is_apk:
                inner.close()
                continue
            produced.append(Candidate(target, inner))
    return produced


def group_candidates(candidates):
    """One group per primary package, carrying every auxiliary file along.

    A candidate is not either a container or a blob: a zip-format OBB -- which
    is what most Unity and Unreal games ship -- is both, and a rule decides
    which role it plays. Only the primary packages define separate groups;
    everything else rides along with each of them, so an APK plus its OBB is
    one installable set rather than two competing ones.
    """
    apks = [c for c in candidates if c.is_container and c.archive.is_apk]
    if not apks:
        primaries = [c for c in candidates if c.is_container]
        if not primaries:
            return [list(candidates)] if candidates else []
        auxiliaries = [c for c in candidates if c not in primaries]
        return [[primary] + auxiliaries for primary in primaries]

    # Splits of one game share a package name, so base.apk and
    # config.arm64_v8a.apk belong to the same group even when nothing about
    # their filenames says so. APKs whose package cannot be read stay on their
    # own rather than being folded into someone else's game.
    by_package = {}
    for index, candidate in enumerate(apks):
        package = candidate.archive.package
        by_package.setdefault(package or ("unidentified-%d" % index), []).append(
            candidate
        )
    auxiliaries = [c for c in candidates if c not in apks]
    return [members + auxiliaries for members in by_package.values()]


def resolve_plan(recipe, candidates, abi_override, digests, logger, workspace=None):
    """ABI is a preference, not a dimension of ambiguity.

    Building a plan for every (group x ABI) pair and then rejecting when two
    differ means a fat APK -- one carrying both arm64-v8a and armeabi-v7a,
    which is the normal case -- fails to install instead of picking arm64.
    Here the best ABI is chosen inside each group first, and only then are
    groups compared with each other.
    """
    if workspace is not None:
        candidates = list(candidates) + expand_bundles(candidates, workspace, logger)
    groups = group_candidates(candidates)
    if not groups:
        raise SourceError(
            "no game package was found. Put the APK, XAPK or data archive in "
            "the game folder or in gamedata/."
        )
    abis = [abi_override] if abi_override else recipe.abi_order

    winners = []
    failures = []
    for index, group in enumerate(groups):
        label = ", ".join(c.name for c in group[:2])
        for abi in abis:
            try:
                plan = build_plan(recipe, group, abi, digests, logger)
            except (PlanError, ValidationError) as error:
                failures.append("[%s / %s] %s" % (label, abi, error))
                continue
            logger.log("group %d (%s) resolved for abi %s" % (index, label, abi))
            winners.append(plan)
            break

    if not winners:
        raise PlanError(
            "no input matches this recipe:\n  " + "\n  ".join(failures[:6])
        )
    distinct = {plan.fingerprint for plan in winners}
    if len(distinct) > 1:
        raise PlanError(
            "%d different game versions match this recipe. Keep only one and "
            "remove the others, or pass --input." % len(distinct)
        )
    return winners[0]


# --------------------------------------------------------------------------
# staging
# --------------------------------------------------------------------------


def stage_root(workspace):
    return os.path.join(workspace, "stage")


def backup_root(workspace):
    return os.path.join(workspace, "backup")


def journal_path(workspace):
    return os.path.join(workspace, "journal")


def prepare_stage(recipe, plan, workspace, logger, durability):
    """Reuse the stage only when it belongs to this exact recipe and plan."""
    state_path = os.path.join(workspace, "stage-state")
    identity = {
        "format": FORMAT_VERSION,
        "semantic_digest": recipe.semantic_digest,
        "plan_fingerprint": plan.fingerprint,
        "abi": plan.abi,
    }
    stage = stage_root(workspace)
    previous = None
    if is_regular_file(state_path):
        try:
            previous = load_json(state_path)
        except (OSError, ValueError):
            previous = None
    if previous != identity:
        if os.path.isdir(stage):
            logger.log("discarding a stage built for a different recipe or payload")
        remove_path(stage)
        remove_path(os.path.join(workspace, "hooks"))
    os.makedirs(stage, exist_ok=True)
    atomic_write_json(state_path, identity, durability)
    return stage


def item_already_staged(stage, item, digests):
    path = os.path.join(stage, item.destination)
    if not is_regular_file(path):
        return False
    try:
        sha256, crc, size = digests.file(path)
    except SourceError:
        return False
    if size != item.size:
        return False
    if item.crc is not None and crc != item.crc:
        return False
    if item.sha256 is not None and sha256 != item.sha256:
        return False
    item.sha256 = sha256          # the marker records what is actually on disk
    item.crc = crc
    return True


def preflight_space(recipe, plan, stage, digests, logger):
    missing = sum(
        item.size for item in plan.items if not item_already_staged(stage, item, digests)
    )
    required = missing + recipe.safety_bytes
    free = shutil.disk_usage(os.path.dirname(stage)).free
    if free < required:
        raise EapxError(
            "not enough free space: need %s (%s of payload plus %s of headroom), "
            "%s available"
            % (human_bytes(required), human_bytes(missing),
               human_bytes(recipe.safety_bytes), human_bytes(free))
        )
    logger.log("space preflight ok: %s needed, %s free" % (human_bytes(required),
                                                           human_bytes(free)))
    return missing


def extract(plan, stage, digests, progress, logger, durability):
    total = plan.total_bytes
    progress.total_bytes = total
    done = 0
    touched = set()
    for item in plan.items:
        if item_already_staged(stage, item, digests):
            done += item.size
            progress.done_bytes = done
            continue
        destination = safe_join(stage, item.destination, "stage destination")
        ensure_no_symlink_parents(stage, item.destination)
        os.makedirs(os.path.dirname(destination), exist_ok=True)
        partial = destination + ".part"
        remove_path(partial)
        sha = hashlib.sha256()
        crc = 0
        written = 0
        try:
            source = (
                open(item.candidate.path, "rb") if item.member is None
                else item.candidate.archive.open(item.member)
            )
            with source, open(partial, "xb") as target:
                while True:
                    block = source.read(CHUNK_SIZE)
                    if not block:
                        break
                    target.write(block)
                    sha.update(block)
                    crc = binascii.crc32(block, crc)
                    written += len(block)
                    done += len(block)
                    progress.done_bytes = done
                    progress.update(
                        overall=200 + (done * 500 // total if total else 0),
                        message="EXTRACTING GAME DATA",
                        detail="%s  %s / %s" % (
                            os.path.basename(item.destination),
                            human_bytes(done), human_bytes(total)
                        ),
                    )
                target.flush()
            if written != item.size:
                raise ValidationError(
                    "%s: extracted %d bytes, expected %d"
                    % (item.destination, written, item.size)
                )
            if item.crc is not None and (crc & 0xFFFFFFFF) != item.crc:
                raise ValidationError("%s: checksum mismatch" % item.destination)
            if item.sha256 is not None and sha.hexdigest() != item.sha256:
                raise ValidationError("%s: content hash mismatch" % item.destination)
            item.sha256 = sha.hexdigest()
            item.crc = crc & 0xFFFFFFFF
            os.replace(partial, destination)
            # We just hashed every byte on the way in; handing the result to the
            # cache is what keeps the stage validation from reading it all again.
            digests.remember(destination, sha.hexdigest(), crc, written)
            touched.add(os.path.dirname(destination))
        finally:
            remove_path(partial)

    # Durability is deferred to a single flush rather than paid per file.
    # Nothing recovers from the stage by trusting it -- resume revalidates every
    # staged file by hash, so a torn write is simply re-extracted -- but the
    # data must be on the card before the commit publishes it, or a power cut
    # could leave a file present with garbage inside. fsyncing 1770 files one by
    # one rewrites the same FAT directory tables over and over and costs far
    # more than the copy itself; one sync at the end buys the same guarantee.
    if touched:
        os.sync()
    logger.log("extracted %s to the stage" % human_bytes(total))


# --------------------------------------------------------------------------
# validation
# --------------------------------------------------------------------------


def tree_stats(root, digests, want_digest=False):
    count = 0
    total = 0
    sha = hashlib.sha256() if want_digest else None
    for base, directories, files in os.walk(root, followlinks=False):
        directories.sort()
        for name in sorted(directories):
            if os.path.islink(os.path.join(base, name)):
                raise ValidationError("symlink directory inside %s: %s" % (root, name))
        for name in sorted(files):
            path = os.path.join(base, name)
            if os.path.islink(path) or not is_regular_file(path):
                raise ValidationError("non-regular file inside %s: %s" % (root, name))
            if name.endswith(".part"):
                raise ValidationError("leftover partial file inside %s: %s" % (root, name))
            relative = os.path.relpath(path, root).replace(os.sep, "/")
            size = os.path.getsize(path)
            count += 1
            total += size
            if sha is not None:
                digest, _crc, _size = digests.file(path)
                sha.update(relative.encode("utf-8") + b"\0")
                sha.update(struct.pack("<Q", size))
                sha.update(bytes.fromhex(digest))
    return count, total, (sha.hexdigest() if sha else None)


def digest_of(path, digests):
    """Content identity of a file or a whole tree."""
    if os.path.isdir(path) and not os.path.islink(path):
        return "tree:" + tree_stats(path, digests, want_digest=True)[2]
    if is_regular_file(path):
        return "file:" + digests.file(path)[0]
    return None


def validate_checks(root, checks, abi, digests, label="validate", logger=None):
    """Apply a prepared list of output checks to an extracted tree."""
    for check in checks:
        relative = template(check["path"], abi)
        path = os.path.join(root, relative)
        spec = check["spec"]
        if set(spec) & FILE_VALIDATORS:
            if not is_regular_file(path):
                raise ValidationError("%s: missing file %s" % (label, relative))
            size = os.path.getsize(path)
            size_reason = check_size_spec(spec, size)
            if size_reason:
                raise ValidationError(
                    "%s %s: %s" % (label, relative, size_reason)
                )
            sha256, _crc, size = digests.file(path)
            critical_sha256 = None
            if needs_critical_regions(spec, sha256):
                critical_sha256 = digests.file_regions(
                    path, spec["critical_regions"]["regions"]
                )
            with open(path, "rb") as stream:
                header = stream.read(64)
            reason = check_file_spec(
                spec, size, sha256, critical_sha256, header, abi, relative
            )
            if reason:
                raise ValidationError("%s %s: %s" % (label, relative, reason))
            if critical_sha256 is not None and logger is not None:
                logger.log(
                    "accepted %s by critical regions (sha256=%s)"
                    % (relative, sha256)
                )
        else:
            if not os.path.isdir(path):
                raise ValidationError("%s: missing directory %s" % (label, relative))
            count, total, _ = tree_stats(path, digests)
            reason = check_tree_spec(spec, count, total)
            if reason:
                raise ValidationError("%s %s: %s" % (label, relative, reason))


def validate_output_checks(root, recipe, abi, digests, logger=None):
    """Apply the recipe's `validate` block. Needs no plan, so adoption can use it."""
    validate_checks(root, recipe.output_checks, abi, digests, logger=logger)


def identify_profile(root, recipe, abi, digests, logger=None):
    """Return the one coherent donor profile matching *root*."""
    if not recipe.profiles:
        return None
    matches = []
    rejected = []
    for profile in recipe.profiles:
        try:
            validate_checks(
                root, profile["validate"], abi, digests,
                label="profile %s" % profile["id"],
                logger=logger,
            )
            matches.append(profile["id"])
        except ValidationError as error:
            rejected.append("%s: %s" % (profile["id"], error))
    if not matches:
        raise ValidationError(
            "donor assets do not match a supported coherent profile; do not "
            "mix payload trees. " + "; ".join(rejected[:3])
        )
    if len(matches) != 1:
        raise ValidationError(
            "donor profile is ambiguous: %s" % ", ".join(sorted(matches))
        )
    return matches[0]


def adopt_existing(recipe, game_dir, marker_path, digests, logger, abi_override,
                   durability):
    """Accept game data that is already installed but has lost its marker.

    Without the package we have no reference hashes, so this can only ask the
    recipe's own `validate` block whether the tree looks like a finished
    install. That is exactly why a recipe with no output checks is refused
    rather than trusted: it would adopt literally anything.

    The synthetic marker records paths and sizes, not hashes -- claiming
    byte-level knowledge of data we never saw arrive would be a lie, and the
    honest alternative costs a full read of the payload for no real guarantee.
    """
    if not recipe.output_checks:
        logger.log(
            "not adopting the installed data: this recipe declares no validate "
            "checks, so there is nothing to judge it by"
        )
        return None

    for abi in ([abi_override] if abi_override else recipe.abi_order):
        roots = recipe.commit_roots(abi)
        try:
            for entry in roots:
                if not os.path.exists(os.path.join(game_dir, entry["path"])):
                    raise ValidationError("%s is not installed" % entry["path"])
            validate_output_checks(game_dir, recipe, abi, digests, logger)
        except ValidationError as error:
            logger.log("no adoptable install for %s (%s); this is normal on "
                       "a fresh install, continuing with the import" % (abi, error))
            continue

        items = []
        for entry in roots:
            base = os.path.join(game_dir, entry["path"])
            if os.path.isdir(base) and not os.path.islink(base):
                for current, directories, files in os.walk(base, followlinks=False):
                    directories.sort()
                    for name in sorted(files):
                        path = os.path.join(current, name)
                        relative = os.path.relpath(path, game_dir).replace(os.sep, "/")
                        items.append(Item("adopted", relative, None, None,
                                          os.path.getsize(path), None))
            else:
                items.append(Item("adopted", entry["path"], None, None,
                                  os.path.getsize(base), None))
        plan = Plan(abi, items, roots)
        plan.profile = identify_profile(game_dir, recipe, abi, digests, logger)
        marker = write_marker(marker_path, recipe, plan, uuid.uuid4().hex,
                              durability, adopted=True)
        logger.log("adopted %d already-installed file(s) for abi %s"
                   % (len(items), abi))
        return marker
    return None


def validate_tree(root, recipe, plan, digests, full=True, logger=None):
    """Validate an installed or staged tree against the recipe."""
    for item in plan.items:
        path = os.path.join(root, item.destination)
        if not is_regular_file(path):
            raise ValidationError("missing payload: %s" % item.destination)
        size = os.path.getsize(path)
        if size != item.size:
            raise ValidationError(
                "%s: size %d, expected %d" % (item.destination, size, item.size)
            )
        if full:
            sha256, crc, _size = digests.file(path)
            if item.sha256 is not None and sha256 != item.sha256:
                raise ValidationError("%s: content hash mismatch" % item.destination)
            if item.crc is not None and crc != item.crc:
                raise ValidationError("%s: checksum mismatch" % item.destination)
    validate_output_checks(root, recipe, plan.abi, digests, logger)
    for root_entry in plan.roots:
        if not os.path.exists(os.path.join(root, root_entry["path"])):
            raise ValidationError("commit root is missing: %s" % root_entry["path"])


def check_foreign_files(game_dir, plan, logger):
    """Refuse to destroy files nobody planned for.

    The commit republishes whole roots by rename, so anything living under a
    root that the extractor did not produce -- saves, config, mods -- would
    disappear without warning. Opt in with "exclusive": true.
    """
    planned = {item.destination for item in plan.items}
    problems = []
    for entry in plan.roots:
        if entry["exclusive"]:
            continue
        base = os.path.join(game_dir, entry["path"])
        if not os.path.isdir(base) or os.path.islink(base):
            continue
        for current, _dirs, files in os.walk(base, followlinks=False):
            for name in files:
                path = os.path.join(current, name)
                relative = os.path.relpath(path, game_dir).replace(os.sep, "/")
                if relative not in planned:
                    problems.append(relative)
    if problems:
        shown = ", ".join(sorted(problems)[:8])
        more = "" if len(problems) <= 8 else " (and %d more)" % (len(problems) - 8)
        raise EapxError(
            "%d file(s) under the install roots were not produced by this recipe "
            "and would be destroyed: %s%s. Move them elsewhere, or mark the root "
            'as "exclusive": true if replacing everything is intended.'
            % (len(problems), shown, more)
        )


# --------------------------------------------------------------------------
# transaction
# --------------------------------------------------------------------------

PENDING = "pending"
BACKING_UP = "backing-up"
BACKED_UP = "backed-up"
INSTALLING = "installing"
INSTALLED = "installed"


class Transaction:
    """Intent journal: every state is written BEFORE the operation it describes.

    A journal that runs ahead of reality is then harmless -- recovery knows
    'this may or may not have happened' and resolves by content rather than by
    trusting a flag. Recording the fact afterwards, as the design we started
    from did, means a crash in the gap makes rollback skip a restore and then
    delete the backup, losing the user's original data.
    """

    def __init__(self, workspace, game_dir, durability, logger):
        self.workspace = workspace
        self.game_dir = game_dir
        self.durability = durability
        self.logger = logger
        self.data = None

    def begin(self, plan, digests):
        paths = []
        for entry in plan.roots:
            relative = entry["path"]
            live = os.path.join(self.game_dir, relative)
            staged = os.path.join(stage_root(self.workspace), relative)
            paths.append({
                "path": relative,
                "state": PENDING,
                "digest_new": digest_of(staged, digests),
                "digest_old": digest_of(live, digests),
            })
        self.data = {
            "format": FORMAT_VERSION,
            "transaction_id": uuid.uuid4().hex,
            "semantic_digest": None,
            "paths": paths,
        }
        self._write()
        return self.data["transaction_id"]

    def _write(self):
        atomic_write_json(journal_path(self.workspace), self.data, self.durability)

    def set_state(self, record, state):
        record["state"] = state
        self._write()

    def load(self):
        path = journal_path(self.workspace)
        if not is_regular_file(path):
            return None
        try:
            data = load_json(path)
        except (OSError, ValueError) as error:
            quarantine = "%s.corrupt-%d" % (path, int(time.time()))
            os.replace(path, quarantine)
            self.logger.log(
                "journal was unreadable (%s); moved to %s and continuing from the "
                "observable state of the tree" % (error, quarantine)
            )
            return None
        if data.get("format") != FORMAT_VERSION:
            raise EapxError("unsupported journal format at %s" % path)
        self.data = data
        return data

    def discard(self):
        try:
            os.unlink(journal_path(self.workspace))
        except FileNotFoundError:
            pass
        self.durability.sync_dir(self.workspace)


def rollback(transaction, digests, logger):
    """Undo by content identity, never by mere existence.

    Every decision compares what is on disk against digest_new / digest_old, so
    the whole procedure is idempotent: interrupting a rollback and re-running it
    cannot destroy what the previous attempt already restored.
    """
    game_dir = transaction.game_dir
    workspace = transaction.workspace
    stage = stage_root(workspace)
    backup = backup_root(workspace)
    logger.log("rolling back an interrupted transaction")

    for record in reversed(transaction.data["paths"]):
        relative = record["path"]
        validate_relative_path(relative, "journal path")
        live = safe_join(game_dir, relative, "journal destination")
        staged = safe_join(stage, relative, "journal stage")
        saved = safe_join(backup, relative, "journal backup")
        current = digest_of(live, digests)

        if current is not None and current == record["digest_new"]:
            # The new payload landed. Put it back in the stage so the work is
            # not lost, then fall through to restoring the original.
            if digest_of(staged, digests) != record["digest_new"]:
                remove_path(staged)
                os.makedirs(os.path.dirname(staged), exist_ok=True)
                os.rename(live, staged)
            else:
                remove_path(live)
            current = None

        if current is None:
            if digest_of(saved, digests) == record["digest_old"] and record["digest_old"]:
                os.makedirs(os.path.dirname(live), exist_ok=True)
                os.rename(saved, live)
        elif current != record["digest_old"]:
            raise EapxError(
                "%s was modified by something else during the transaction; "
                "refusing to touch it. Inspect it by hand." % relative
            )
        transaction.set_state(record, PENDING)

    transaction.durability.sync_parents(
        game_dir, [r["path"] for r in transaction.data["paths"]]
    )
    transaction.discard()
    remove_path(backup)
    logger.log("rollback complete; staged work preserved")


def commit(recipe, plan, game_dir, workspace, marker_path, digests, progress,
           logger, durability):
    stage = stage_root(workspace)
    backup = backup_root(workspace)
    remove_path(backup)
    os.makedirs(backup, exist_ok=True)

    for entry in plan.roots:
        relative = entry["path"]
        if not os.path.exists(os.path.join(stage, relative)):
            raise ValidationError("staged commit root is missing: %s" % relative)
        ensure_no_symlink_parents(stage, relative)
        ensure_no_symlink_parents(game_dir, relative)
        live = os.path.join(game_dir, relative)
        if os.path.exists(live):
            if os.stat(live).st_dev != os.stat(workspace).st_dev:
                raise EapxError(
                    "%s lives on a different filesystem than the workspace; "
                    "the atomic rename would fail mid-commit" % relative
                )

    transaction = Transaction(workspace, game_dir, durability, logger)
    transaction_id = transaction.begin(plan, digests)
    progress.update(overall=900, message="INSTALLING GAME DATA", force=True)

    try:
        for index, record in enumerate(transaction.data["paths"]):
            relative = record["path"]
            live = safe_join(game_dir, relative, "commit destination")
            staged = safe_join(stage, relative, "commit stage")
            saved = safe_join(backup, relative, "commit backup")

            if record["digest_old"] is not None:
                transaction.set_state(record, BACKING_UP)
                os.makedirs(os.path.dirname(saved), exist_ok=True)
                os.rename(live, saved)
                transaction.set_state(record, BACKED_UP)

            transaction.set_state(record, INSTALLING)
            os.makedirs(os.path.dirname(live), exist_ok=True)
            os.rename(staged, live)
            transaction.set_state(record, INSTALLED)
            progress.update(
                overall=900 + (index + 1) * 50 // len(transaction.data["paths"]),
                message="INSTALLING GAME DATA",
                detail=relative,
            )

        progress.update(overall=960, message="VERIFYING INSTALLED DATA", force=True)
        # Cheap check on purpose. The stage was fully validated byte for byte a
        # moment ago and publishing is a rename of those same inodes, so the
        # content cannot have changed; re-hashing would prove nothing and costs
        # a whole extra pass over the payload on a slow card.
        validate_tree(game_dir, recipe, plan, digests, full=False, logger=logger)

        # Everything the marker certifies must be durable BEFORE the marker is.
        durability.sync_parents(game_dir, [r["path"] for r in transaction.data["paths"]])
        write_marker(marker_path, recipe, plan, transaction_id, durability)
        transaction.discard()
    except BaseException:
        rollback(transaction, digests, logger)
        raise

    # The backup is the last thing to go, and only after a positive check.
    remove_path(backup)
    remove_path(stage)
    remove_path(os.path.join(workspace, "cache"))
    durability.sync_dir(workspace)
    logger.log("payload committed")


def recover(workspace, game_dir, marker_path, digests, durability, logger):
    transaction = Transaction(workspace, game_dir, durability, logger)
    data = transaction.load()
    if data is None:
        return
    marker = read_marker(marker_path)
    if marker is not None and marker.get("transaction_id") == data.get("transaction_id"):
        logger.log("a published transaction was interrupted during cleanup")
        remove_path(backup_root(workspace))
        remove_path(stage_root(workspace))
        transaction.discard()
        return
    rollback(transaction, digests, logger)


def write_marker(path, recipe, plan, transaction_id, durability, adopted=False):
    marker = {
        "format": FORMAT_VERSION,
        "eapx_version": VERSION,
        "recipe_id": recipe.identifier,
        "recipe_version": recipe.version,
        "semantic_digest": recipe.semantic_digest,
        "abi": plan.abi,
        "plan_fingerprint": plan.fingerprint,
        "transaction_id": transaction_id,
        "adopted": adopted,
        "completed": int(time.time()),
        "items": [
            {"rule": i.rule_id, "destination": i.destination,
             "size": i.size, "sha256": i.sha256}
            for i in plan.items
        ],
    }
    if recipe.profiles:
        marker["donor_profile"] = plan.profile
    atomic_write_json(path, marker, durability)
    return marker


def read_marker(path):
    if not is_regular_file(path):
        return None
    try:
        data = load_json(path)
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) else None


def marker_fast_path(marker_path, recipe, game_dir, logger):
    """Cheap revalidation: size and presence, no rehashing.

    The recorded sizes come from the marker rather than the recipe, so a
    truncated or swapped file is still caught without reading every byte.
    """
    marker = read_marker(marker_path)
    if marker is None:
        return None
    if marker.get("format") != FORMAT_VERSION:
        return None
    if marker.get("recipe_id") != recipe.identifier:
        return None
    if marker.get("semantic_digest") != recipe.semantic_digest:
        logger.log("marker was written for a different recipe; revalidating")
        return None
    if recipe.profiles:
        known_profiles = {profile["id"] for profile in recipe.profiles}
        if marker.get("donor_profile") not in known_profiles:
            logger.log("marker has no current donor profile; revalidating")
            return None
    for item in marker.get("items", []):
        path = os.path.join(game_dir, item.get("destination", ""))
        if not is_regular_file(path):
            return None
        if os.path.getsize(path) != item.get("size"):
            return None
    return marker


# --------------------------------------------------------------------------
# hooks
# --------------------------------------------------------------------------


def run_hooks(recipe, plan, game_dir, workspace, stage, progress, logger):
    if not recipe.hooks:
        return
    mapping = {
        "game_dir": game_dir,
        "stage": stage,
        "workspace": workspace,
        "recipe_dir": recipe.directory,
        "abi": plan.abi,
    }

    def expand(value):
        result = value
        for key, replacement in mapping.items():
            result = result.replace("{%s}" % key, replacement)
        return result

    environment = dict(os.environ)
    environment.update({
        "EAPX_GAME_DIR": game_dir,
        "EAPX_STAGE": stage,
        "EAPX_WORKSPACE": workspace,
        "EAPX_ABI": plan.abi,
    })

    for index, hook in enumerate(recipe.hooks):
        argv = [expand(a) for a in hook["argv"]]
        cwd = os.path.realpath(expand(hook["cwd"]))
        if os.path.commonpath([cwd, game_dir]) != game_dir:
            raise RecipeError("hook %s: cwd escapes the game directory" % hook["id"])
        hook_env = dict(environment)
        for name, value in hook["env"].items():
            hook_env[name] = expand(value)
        progress.update(
            overall=700 + index * 50 // max(len(recipe.hooks), 1),
            message="PROCESSING GAME DATA",
            detail=hook["id"],
            force=True,
        )
        logger.log("running hook %s: %s" % (hook["id"], " ".join(argv)))
        try:
            completed = subprocess.run(
                argv, cwd=cwd, env=hook_env, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                timeout=hook["timeout_seconds"],
            )
        except subprocess.TimeoutExpired:
            # Without this the engine blocks forever holding the lock, so no
            # future run can even start, and the device hangs until reboot.
            raise EapxError(
                "hook %s exceeded its %d second timeout and was killed"
                % (hook["id"], hook["timeout_seconds"])
            )
        except OSError as error:
            raise EapxError("hook %s could not run: %s" % (hook["id"], error))
        for line in completed.stdout.decode("utf-8", "replace").splitlines():
            logger.log("  [%s] %s" % (hook["id"], line))
        if completed.returncode != 0:
            raise EapxError(
                "hook %s failed with exit code %d" % (hook["id"], completed.returncode)
            )


# --------------------------------------------------------------------------
# workspace
# --------------------------------------------------------------------------


class GameDirLock:
    """One lock per game dir, not per recipe.

    Two recipes with different ids can overlap in their commit roots; keeping
    the lock at recipe granularity lets them interleave renames in the same
    tree, and serialising costs nothing here.
    """

    def __init__(self, game_dir):
        self.path = os.path.join(game_dir, ".eapx", "lock")
        self.handle = None

    def __enter__(self):
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        self.handle = open(self.path, "a+")
        try:
            fcntl.flock(self.handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            self.handle.close()
            self.handle = None
            if error.errno in (errno.EACCES, errno.EAGAIN):
                raise EapxError("another extraction is already running")
            raise EapxError("cannot lock the game directory: %s" % error)
        return self

    def __exit__(self, *exc):
        if self.handle is not None:
            fcntl.flock(self.handle.fileno(), fcntl.LOCK_UN)
            self.handle.close()
            self.handle = None


def ensure_real_directory(path, label):
    os.makedirs(path, exist_ok=True)
    info = os.lstat(path)
    if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
        raise EapxError("%s must be a real directory: %s" % (label, path))
    return path


def prepare_workspace(game_dir, identifier):
    ensure_real_directory(os.path.join(game_dir, ".eapx"), "workspace root")
    return ensure_real_directory(
        os.path.join(game_dir, ".eapx", identifier), "workspace"
    )


# --------------------------------------------------------------------------
# commands
# --------------------------------------------------------------------------


def under_patcher():
    """True when our stdout is being rendered by the PortMaster patcher."""
    return bool(
        os.environ.get("PATCHER_FILE")
        or os.environ.get("PATCHER_GAME")
        or os.path.exists(PortMaster.INPUT)
    )


def report_failure(error):
    """Fail the way the PortMaster patcher expects.

    Without this the user gets the generic "Patching failed! Please go to the
    PortMaster Discord for help." PATCH_FAIL_MSG overrides that with something
    actionable, and it goes to fd 3 so it never lands in the log the way the
    deltarune port works around.
    """
    message = clamp_text(error)
    if not under_patcher():
        sys.stderr.write("error: %s\n" % message)
        return
    try:
        with os.fdopen(os.dup(3), "w") as channel:
            channel.write("PATCH_FAIL_MSG:%s\n" % message)
    except OSError:
        pass
    sys.stdout.write("Patching process failed!\n")
    sys.stdout.flush()
    sys.stderr.write("error: %s\n" % message)


def open_context(args):
    game_dir = os.path.realpath(args.game_dir)
    if not os.path.isdir(game_dir) or os.path.islink(game_dir):
        raise EapxError("game directory is missing, a symlink, or not a directory")
    recipe = Recipe(args.recipe)
    workspace = prepare_workspace(game_dir, recipe.identifier)
    logger = Logger(safe_join(game_dir, recipe.log, "log"), verbose=not args.quiet)
    return game_dir, recipe, workspace, logger


def install_command(args):
    game_dir, recipe, workspace, logger = open_context(args)
    digests = DigestCache()
    durability = Durability(logger)
    portmaster = PortMaster(logger, enabled=not args.no_portmaster)
    progress = Progress(
        args.progress_file or os.path.join(workspace, "progress"),
        logger, title=recipe.title, tty=args.tty, portmaster=portmaster
    )
    marker_path = safe_join(game_dir, recipe.marker, "marker")
    ensure_no_symlink_parents(game_dir, recipe.marker)
    candidates = []
    try:
        with GameDirLock(game_dir):
            logger.log("=== eapx %s recipe=%s v%s ==="
                       % (VERSION, recipe.identifier, recipe.version))
            durability.probe(workspace)
            recover(workspace, game_dir, marker_path, digests, durability, logger)

            if marker_fast_path(marker_path, recipe, game_dir, logger) is not None:
                progress.done("GAME DATA ALREADY READY")
                logger.log("marker accepted; nothing to do")
                return 0

            if not args.no_adopt and adopt_existing(
                recipe, game_dir, marker_path, digests, logger, args.abi, durability
            ) is not None:
                progress.done("GAME DATA ALREADY READY")
                return 0

            progress.update(overall=20, message="LOOKING FOR GAME DATA", force=True)
            candidates = discover(recipe, game_dir, args.input, logger)
            progress.update(overall=100, message="READING PACKAGE CONTENTS", force=True)
            plan = resolve_plan(recipe, candidates, args.abi, digests, logger,
                                workspace)
            logger.log("plan: abi=%s items=%d bytes=%s"
                       % (plan.abi, len(plan.items), human_bytes(plan.total_bytes)))

            check_foreign_files(game_dir, plan, logger)
            stage = prepare_stage(recipe, plan, workspace, logger, durability)
            preflight_space(recipe, plan, stage, digests, logger)
            extract(plan, stage, digests, progress, logger, durability)
            if recipe.hooks:
                run_hooks(recipe, plan, game_dir, workspace, stage, progress, logger)
                # A hook can rewrite staged files; on a coarse-timestamp
                # filesystem the cache cannot tell, so it must not try.
                digests.invalidate(stage)

            progress.update(overall=850, message="VALIDATING GAME DATA", force=True)
            validate_tree(stage, recipe, plan, digests, full=True, logger=logger)
            plan.profile = identify_profile(
                stage, recipe, plan.abi, digests, logger=logger
            )
            if plan.profile:
                logger.log("donor profile=%s" % plan.profile)
            commit(recipe, plan, game_dir, workspace, marker_path, digests,
                   progress, logger, durability)

            # The placeholder is how the user was told where to put the game;
            # removing it is the cheapest possible "this is done" signal, and
            # it is what the ports that use one already do.
            if recipe.placeholder:
                remove_path(safe_join(game_dir, recipe.placeholder, "placeholder"))

            portmaster.clear()
            progress.done()
            logger.log("installation complete (%s read)"
                       % human_bytes(digests.bytes_read))
            # The patcher watches stdout for exactly this sentence.
            if under_patcher():
                sys.stdout.write("Patching completed successfully!\n")
                sys.stdout.flush()
            return 0
    except EapxError as error:
        logger.log("ERROR: %s" % error)
        progress.fail(str(error))
        portmaster.clear()
        report_failure(error)
        return 1
    finally:
        for candidate in candidates:
            if candidate.archive is not None:
                candidate.archive.close()
        logger.close()


def plan_command(args):
    game_dir, recipe, workspace, logger = open_context(args)
    digests = DigestCache()
    candidates = discover(recipe, game_dir, args.input, logger)
    try:
        plan = resolve_plan(recipe, candidates, args.abi, digests, logger, workspace)
        report = {
            "abi": plan.abi,
            "fingerprint": plan.fingerprint,
            "total_bytes": plan.total_bytes,
            "items": [
                {"rule": i.rule_id, "destination": i.destination, "size": i.size,
                 "source": i.member or os.path.basename(i.candidate.path)}
                for i in plan.items
            ],
        }
        sys.stdout.write(json.dumps(report, indent=2, sort_keys=True) + "\n")
        return 0
    except EapxError as error:
        sys.stderr.write("error: %s\n" % error)
        return 1
    finally:
        for candidate in candidates:
            if candidate.archive is not None:
                candidate.archive.close()
        remove_path(os.path.join(workspace, "cache"))
        logger.close()


def check_command(args):
    """Static recipe review that needs no APK."""
    try:
        recipe = Recipe(args.recipe)
    except RecipeError as error:
        sys.stderr.write("error: %s\n" % error)
        return 1
    warnings = []
    for rule in recipe.rules:
        if not rule["validate"]:
            warnings.append(
                "%s has no content validation at all: it will accept anything "
                "matching %s" % (rule["id"], ", ".join(rule["patterns"]))
            )
    sys.stdout.write("ok: %s v%s (%d rules, %d commit roots)\n"
                     % (recipe.identifier, recipe.version,
                        len(recipe.rules), len(recipe.commit)))
    sys.stdout.write("semantic digest: %s\n" % recipe.semantic_digest[:16])
    for warning in warnings:
        sys.stdout.write("warning: %s\n" % warning)
    return 0


def verify_command(args):
    game_dir, recipe, _workspace, logger = open_context(args)
    marker_path = safe_join(game_dir, recipe.marker, "marker")
    marker = read_marker(marker_path)
    if marker is None:
        sys.stderr.write("error: no marker; the game data is not installed\n")
        return 1
    digests = DigestCache()
    try:
        for item in marker.get("items", []):
            path = os.path.join(game_dir, item["destination"])
            if not is_regular_file(path):
                raise ValidationError("missing: %s" % item["destination"])
            sha256, _crc, size = digests.file(path)
            if size != item["size"]:
                raise ValidationError("modified: %s" % item["destination"])
            if item.get("sha256") and sha256 != item["sha256"]:
                raise ValidationError("modified: %s" % item["destination"])
        profile = identify_profile(
            game_dir, recipe, marker.get("abi"), digests, logger=logger
        )
        if recipe.profiles and profile != marker.get("donor_profile"):
            raise ValidationError(
                "donor profile changed: marker=%s current=%s"
                % (marker.get("donor_profile"), profile)
            )
        note = ""
        if marker.get("adopted"):
            note = ("  (adopted install: sizes checked, content unverified -- "
                    "eapx never saw the original package)")
        if recipe.profiles:
            sys.stdout.write("ok: %d item(s) verified profile=%s (%s read)%s\n"
                             % (len(marker.get("items", [])), profile,
                                human_bytes(digests.bytes_read), note))
        else:
            sys.stdout.write("ok: %d item(s) verified (%s read)%s\n"
                             % (len(marker.get("items", [])),
                                human_bytes(digests.bytes_read), note))
        return 0
    except EapxError as error:
        sys.stderr.write("error: %s\n" % error)
        return 1
    finally:
        logger.close()


def build_parser():
    parser = argparse.ArgumentParser(prog="eapx", description=__doc__)
    parser.add_argument("--version", action="version", version=VERSION)
    subparsers = parser.add_subparsers(dest="command")

    def common(sub):
        sub.add_argument("--recipe", required=True)
        sub.add_argument("--game-dir", default=".")
        sub.add_argument("--quiet", action="store_true")

    install = subparsers.add_parser("install")
    common(install)
    install.add_argument("--input", action="append")
    install.add_argument("--abi")
    install.add_argument("--progress-file")
    install.add_argument("--tty", help="console device, or 'none' to stay silent")
    install.add_argument("--no-adopt", action="store_true",
                         help="never accept already-installed data without a marker")
    install.add_argument("--no-portmaster", action="store_true",
                         help="do not talk to the PortMaster progress bar")
    install.set_defaults(func=install_command)

    plan = subparsers.add_parser("plan")
    common(plan)
    plan.add_argument("--input", action="append")
    plan.add_argument("--abi")
    plan.set_defaults(func=plan_command)

    check = subparsers.add_parser("check")
    check.add_argument("--recipe", required=True)
    check.set_defaults(func=check_command)

    verify = subparsers.add_parser("verify")
    common(verify)
    verify.set_defaults(func=verify_command)
    return parser


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if not getattr(args, "func", None):
        parser.print_help()
        return 2
    try:
        return args.func(args)
    except EapxError as error:
        sys.stderr.write("error: %s\n" % error)
        return 1


if __name__ == "__main__":
    sys.exit(main())
