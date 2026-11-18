#!/bin/sh
#
# stage_dist.sh -- fill dist/ (DEV: in the FS-UAE autotest machine).
#
#   dist/Zod/            the game: its data from $ZOD_DATA (the user's own CD,
#                        extracted -- never in the repository) and the builds
#   dist/Libs/RadeonGL/  minigl.library; under FS-UAE it runs its null card
#   dist/Test/           the run scripts and helpers
#
# ZOD_DATA     a game drawer with assets/ Data/ packs/ (default
#              ../ZodEngine-data/run: cego's release + tools/cd/extract.py)
# ZOD_MGL_LIB  the minigl.library to test with (default: RadeonGL 0.16 as
#              released, the 040/060 build; never build in the RadeonGL trees)
# ZOD_BUILD    the build drawer with zod_040/zod_060 (default build/amiga-g6)
set -e
cd "$(dirname "$0")/.."

DATA="${ZOD_DATA:-../ZodEngine-data/run}"
LIB="${ZOD_MGL_LIB:-../RadeonGL/docs/hw/a4000d/minigl-r016.library}"
BUILD="${ZOD_BUILD:-build/amiga-g6}"

[ -d "$DATA/packs" ] || { echo "stage_dist: no game data in $DATA (ZOD_DATA)"; exit 2; }
[ -f "$LIB" ] || { echo "stage_dist: no minigl.library at $LIB (ZOD_MGL_LIB)"; exit 2; }

mkdir -p dist/Zod dist/Libs/RadeonGL dist/Test

for d in assets Data packs; do
	rsync -a "$DATA/$d" dist/Zod/
done

# fresh each time: the game writes its settings back
cp "$DATA/map_list.txt" "$DATA/story_list.txt" "$DATA/default_settings.txt" dist/Zod/
cp tests/golden/psettings_nopause.txt dist/Zod/

for b in "$BUILD/zod_040" "$BUILD/zod_060"; do
	[ -f "$b" ] && cp "$b" dist/Zod/
done

cp "$LIB" dist/Libs/RadeonGL/minigl.library
cp tests/auto/RunZod tests/auto/RunZodNoGL tests/auto/WaitDone dist/Test/
[ -f dist/Test/Grab ] || cp ../RadeonGL/dist/Test/Grab dist/Test/ 2>/dev/null || true

echo "stage_dist: dist/Zod from $DATA, minigl.library from $LIB, builds from $BUILD"
