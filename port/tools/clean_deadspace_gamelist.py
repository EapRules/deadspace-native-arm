#!/usr/bin/env python3
"""Remove only Dead Space entries from an EmulationStation gamelist."""

from __future__ import annotations

import argparse
import xml.etree.ElementTree as ET


TARGET_PATHS = {"./Dead Space.sh", "./deadspace/grab_screen.sh"}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source")
    parser.add_argument("destination")
    parser.add_argument("--expect", type=int, default=2)
    args = parser.parse_args()

    tree = ET.parse(args.source)
    root = tree.getroot()
    before = len(root.findall("game"))
    removed: list[str] = []

    for game in list(root.findall("game")):
        path = game.findtext("path")
        if path in TARGET_PATHS:
            root.remove(game)
            removed.append(path)

    if len(removed) != args.expect:
        raise SystemExit(
            f"refusing to write: expected {args.expect} removals, got "
            f"{len(removed)} ({removed!r})"
        )

    after = len(root.findall("game"))
    if before - after != len(removed):
        raise SystemExit("entry-count invariant failed")

    tree.write(args.destination, encoding="UTF-8", xml_declaration=True)
    print(f"entries: {before} -> {after}; removed: {removed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
