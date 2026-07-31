#!/usr/bin/env bash
# Append one validated command to a running emulator.
set -euo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTROL_DIR="${DEADSPACE_CONTROL_DIR:-$PORT_DIR/emulator/runtime}"
COMMANDS="$CONTROL_DIR/commands"

if [ "$#" -lt 1 ]; then
    echo "usage: $0 cursor X Y | click down|up | button NAME down|up | stick left|right X Y | screenshot TOKEN | quit" >&2
    exit 2
fi

case "$1" in
    cursor)
        [ "$#" -eq 3 ] || exit 2
        [[ "$2" =~ ^[0-9]+([.][0-9]+)?$ ]] || exit 2
        [[ "$3" =~ ^[0-9]+([.][0-9]+)?$ ]] || exit 2
        printf 'cursor %s %s\n' "$2" "$3" >> "$COMMANDS"
        ;;
    click)
        [ "$#" -eq 2 ] || exit 2
        [[ "$2" == "down" || "$2" == "up" ]] || exit 2
        printf 'click %s\n' "$2" >> "$COMMANDS"
        ;;
    button)
        [ "$#" -eq 3 ] || exit 2
        [[ "$2" =~ ^(a|b|x|y|l1|r1|l3|r3|start|select|up|down|left|right)$ ]] || exit 2
        [[ "$3" == "down" || "$3" == "up" ]] || exit 2
        printf 'button %s %s\n' "$2" "$3" >> "$COMMANDS"
        ;;
    stick)
        [ "$#" -eq 4 ] || exit 2
        [[ "$2" == "left" || "$2" == "right" ]] || exit 2
        [[ "$3" =~ ^-?(0([.][0-9]+)?|1([.]0+)?)$ ]] || exit 2
        [[ "$4" =~ ^-?(0([.][0-9]+)?|1([.]0+)?)$ ]] || exit 2
        printf 'stick %s %s %s\n' "$2" "$3" "$4" >> "$COMMANDS"
        ;;
    screenshot)
        [ "$#" -eq 2 ] || exit 2
        [[ "$2" =~ ^[A-Za-z0-9_-]+$ ]] || exit 2
        printf 'screenshot %s\n' "$2" >> "$COMMANDS"
        ;;
    quit)
        [ "$#" -eq 1 ] || exit 2
        printf 'quit\n' >> "$COMMANDS"
        ;;
    *)
        echo "unknown command: $1" >&2
        exit 2
        ;;
esac
