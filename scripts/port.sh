#!/usr/bin/env bash
# Ports a PSP game of yours to the browser in one go:
#   scripts/port.sh <name> <game.iso | game.zip | game.7z | extracted_disc_dir> [--native] [--opt N]
#
#   1. extracts the disc to games/<name>/root/disc
#   2. decrypts PSP_GAME/SYSDIR/EBOOT.BIN to games/<name>/root/EBOOT.BIN (see decrypt.sh)
#   3. translates its MIPS code to C++ (profile/generated/<name>)
#   4. writes the streaming manifest and the bundle preloaded by the page
#   5. builds the browser version (and with --native the headless test runner)
#
# Then serve it with scripts/serve.sh <name> and open http://localhost:8613/.
# --opt sets the optimisation level of the generated code (default 1; higher
# levels take much longer to build and barely run faster).
set -euo pipefail
source "$(dirname "$0")/common.sh"
here="$(dirname "$0")"

[ $# -ge 2 ] || { sed -n '2,13p' "$0"; exit 2; }
name="$1"; source_path="$2"; shift 2
native=0; opt=1
while [ $# -gt 0 ]; do
  case "$1" in
    --native) native=1 ;;
    --opt) opt="$2"; shift ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done

game="$ROOT/games/$name"
disc="$game/root/disc"
guest="disc0:/PSP_GAME/SYSDIR/EBOOT.BIN"
[ -d "$FRAMEWORK/.git" ] && [ -f "$EMSDK/emsdk_env.sh" ] || "$here/setup.sh"

echo "== [1/5] disc"
if [ -d "$disc/PSP_GAME" ]; then
  echo "already extracted"
elif [ -d "$source_path" ]; then
  mkdir -p "$game/root"
  cp -r "$source_path" "$disc"
else
  python3 -I "$here/extract_iso.py" "$source_path" "$disc"
fi
[ -f "$disc/PSP_GAME/SYSDIR/EBOOT.BIN" ] || { echo "no PSP_GAME/SYSDIR/EBOOT.BIN in the disc" >&2; exit 1; }

echo "== [2/5] executable"
[ -f "$game/root/EBOOT.BIN" ] || "$here/decrypt.sh" "$disc/PSP_GAME/SYSDIR/EBOOT.BIN" "$game/root/EBOOT.BIN"

echo "== [3/5] MIPS to C++"
"$here/generate.sh" "$name" "$game/root/EBOOT.BIN"

echo "== [4/5] bundle"
mkdir -p "$game/root/ms0" "$game/preload/ms0"
python3 -I "$here/manifest.py" "$disc" "$game/root/disc.manifest"
cp "$game/root/EBOOT.BIN" "$game/root/disc.manifest" "$game/preload/"

echo "== [5/5] build"
GEN_OPT="$opt" "$here/build_web.sh" "$name" "$game/preload" "$guest"
if [ "$native" = 1 ]; then
  GEN_OPT="$opt" "$here/build_native.sh" "$name" "$guest"
  echo "native runner: $BUILD/native-$name/profiles/web/pspweb_$name $game/root/EBOOT.BIN --disc $disc --gl 1 --frames 600 --dump frame.ppm"
fi
echo
echo "Done. Serve it with:  scripts/serve.sh $name"
echo "and open http://localhost:8613/  (add ?profile for a timing breakdown)"
