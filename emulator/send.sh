#!/usr/bin/env bash
# Append one validated command to a running emulator.
set -euo pipefail

PORT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONTROL_DIR="${DEADSPACE_CONTROL_DIR:-$PORT_DIR/emulator/runtime}"
COMMANDS="$CONTROL_DIR/commands"

if [ "$#" -lt 1 ]; then
    echo "usage: $0 cursor X Y | click down|up | button NAME down|up | stick left|right X Y | screenshot TOKEN | quit" >&2
    echo "       $0 seq 'start:down,start:up,sleep:2,a:down,a:up'" >&2
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
        [[ "$2" =~ ^(a|b|x|y|l1|l2|r1|r2|l3|r3|start|select|up|down|left|right)$ ]] || exit 2
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
    seq)
        # A whole gesture in one invocation. Getting to a menu is a dozen
        # button/sleep pairs, and a dozen shell round trips through docker is
        # slower than the frames they are pacing.
        #
        # Every step re-enters this script as the standalone command it stands
        # for, so the control names and the number formats are validated in
        # exactly one place. A step that does not validate stops the sequence
        # where it failed rather than skipping to the next one - a half-pressed
        # button left down is worse than a short gesture.
        [ "$#" -eq 2 ] || exit 2
        REST="$2"
        while [ -n "$REST" ]; do
            STEP="${REST%%,*}"
            case "$REST" in
                *,*) REST="${REST#*,}" ;;
                *)   REST="" ;;
            esac
            [ -n "$STEP" ] || exit 2
            case "$STEP" in
                # Host-side, not a command the emulator understands: it spaces
                # the actions out over the game's frames, which at two frames a
                # second is the only way a press lands on a different one than
                # the release.
                sleep:*)
                    DELAY="${STEP#sleep:}"
                    [[ "$DELAY" =~ ^[0-9]+([.][0-9]+)?$ ]] || exit 2
                    sleep "$DELAY"
                    ;;
                click:*)
                    "$0" click "${STEP#click:}"
                    ;;
                cursor:*)
                    XY="${STEP#cursor:}"
                    [ "$XY" != "${XY#*:}" ] || exit 2
                    "$0" cursor "${XY%%:*}" "${XY#*:}"
                    ;;
                screenshot:*)
                    "$0" screenshot "${STEP#screenshot:}"
                    ;;
                quit)
                    "$0" quit
                    ;;
                *:down|*:up)
                    "$0" button "${STEP%:*}" "${STEP##*:}"
                    ;;
                *)
                    echo "unknown step: $STEP" >&2
                    exit 2
                    ;;
            esac
        done
        ;;
    *)
        echo "unknown command: $1" >&2
        exit 2
        ;;
esac
