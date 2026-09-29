#!/bin/sh
# Renders the C port with the camera of a JS reference shot and diffs the images.
#   tools/cmp-shot.sh tools/ref/out/shots/NAME   (NAME.png + NAME.json from ref-shot.mjs)
# Writes NAME.c.png and NAME.diff.png next to it.
set -eu
base=$1
cam=$(node -e "const j=require(require('path').resolve('$base.json'));const c=j.camera;console.log([...c.p,...c.q,c.fov].join(','))")
keep=$(node -e "const j=require(require('path').resolve('$base.json'));console.log(j.keep ?? 'all')")
size=$(node -e "const j=require(require('path').resolve('$base.json'));console.log(j.w+'x'+j.h)")
build/release/oceandrive --shot "$base.c.bmp" --size "$size" --cam "$cam" --keep "$keep" ${EXTRA:-} 2>&1 | grep -v "^GPU driver\|^quality\|^wrote" || true
magick "$base.png" -alpha off "BMP3:$base.ref.bmp"
build/imgdiff "$base.c.bmp" "$base.ref.bmp" "$base.diff.bmp"
magick "$base.c.bmp" "$base.c.png"
magick "$base.diff.bmp" "$base.diff.png"
rm -f "$base.c.bmp" "$base.ref.bmp" "$base.diff.bmp"
