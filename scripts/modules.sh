#!/usr/bin/env bash
# Translates the modules a game loads at run time, as noted in
# games/<name>/root/modules.txt by the native runner (see modules.py):
#   scripts/modules.sh <name> [--siblings]
# Rebuild afterwards (build_native.sh / build_web.sh) to link them in.
set -euo pipefail
source "$(dirname "$0")/common.sh"
name="$1"; shift
[ -x "$TOOLS_BUILD/psp_recomp" ] || "$(dirname "$0")/build_tools.sh"
python3 -I "$(dirname "$0")/modules.py" "$TOOLS_BUILD/psp_recomp" "$ROOT/games/$name/root/disc" \
    "$ROOT/games/$name/root/modules.txt" "$FRAMEWORK/profiles/web/generated/$name" "$@"
