#!/usr/bin/env bash
# Compatibility entry point. The maintained release builder lives at the
# repository root so there is only one definition of the PortMaster layout.
set -euo pipefail

PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
exec "$PORT_DIR/package_portmaster.sh" "$@"
