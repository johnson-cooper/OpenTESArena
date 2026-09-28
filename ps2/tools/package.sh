#!/usr/bin/env bash
# Assembles a copyable PS2 install folder from a finished `ps2build build`:
#
#   build/OpenTESArena/
#     opentesarena-ps2.elf
#     data/            (OpenTESArena's own engine data: meshes, UI, audio/music definitions, clocks...)
#     data/ARENA/      <- put your own copy of The Elder Scrolls: Arena here (not distributed)
#     options/options-default.txt  (PS2 defaults)
#
# Copy build/OpenTESArena/ to the root of a FAT32 USB stick and launch the ELF with a loader
# (wLaunchELF, OPL's app launcher, etc.). Paths are resolved relative to the ELF's own directory.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ELF="$ROOT/build/bin/opentesarena-ps2.elf"
OUT="$ROOT/build/OpenTESArena"

if [ ! -f "$ELF" ]; then
	echo "Missing $ELF - run 'ps2build build' first." >&2
	exit 1
fi

# Refresh only what this script owns; never touch user-supplied Arena data (data/ARENA*) or logs/options changes.
rm -f "$OUT/opentesarena-ps2.elf"
for entry in "$ROOT"/data/*; do rm -rf "$OUT/data/$(basename "$entry")"; done
mkdir -p "$OUT/data" "$OUT/options" "$OUT/log"
cp "$ELF" "$OUT/"

# Engine data minus desktop-only content (Vulkan shaders).
for entry in "$ROOT"/data/*; do
	name="$(basename "$entry")"
	if [ "$name" = "shaders" ]; then
		continue
	fi
	cp -R "$entry" "$OUT/data/"
done

cp "$ROOT/ps2/dist/options/options-default.txt" "$OUT/options/"

mkdir -p "$OUT/data/ARENA"
if [ -z "$(ls -A "$OUT/data/ARENA" 2>/dev/null)" ] && [ ! -d "$OUT/data/ARENACD" ]; then
cat > "$OUT/data/ARENA/PUT_ARENA_FILES_HERE.txt" <<'EOF'
Copy the contents of your own Arena installation here (the folder containing A.EXE
for the floppy version, or rename this folder to ARENACD for the CD version with ACD.EXE),
e.g. GLOBAL.BSA, A.EXE, *.INF, *.MIF, ... OpenTESArena does not ship Arena's data.
The Elder Scrolls: Arena is available for free from Bethesda.
EOF
fi

echo "Packaged to $OUT"
du -sh "$OUT"
