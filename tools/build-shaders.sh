#!/bin/sh
# Builds every GPU program into gen/:
#   shaders/programs/*.prog --shadergen--> build/vk/*.vert|frag + gen/programs.{h,c}
#   shaders/*.vert|frag|comp (hand-written Vulkan GLSL) and build/vk/* --glslang--> build/spv/
#   build/spv/*.spv --shaderpack--> gen/shaders.{h,c}
# Usage (from c/): tools/build-shaders.sh CC
set -eu
CC=${1:-cc}
GLSLANG=${GLSLANG:-glslangValidator}
mkdir -p build/vk build/spv build/pp gen
$CC -std=c23 -Wall -Wextra -Werror -O2 -o build/shadergen tools/shadergen.c
$CC -std=c23 -Wall -Wextra -Werror -O2 -o build/shaderpack tools/shaderpack.c
rm -f build/vk/*.vert build/vk/*.frag build/spv/*.spv
progs=$(ls shaders/programs/*.prog 2>/dev/null || true)
build/shadergen build build/vk gen/programs.h.tmp gen/programs.c.tmp $progs
compile() {
  if ! $GLSLANG -V --target-env vulkan1.0 -Ishaders/include -o "build/spv/$(basename "$1").spv" "$1" > build/spv/log.txt 2>&1; then
    cat build/spv/log.txt >&2
    echo "shader compile failed: $1" >&2
    exit 1
  fi
}
for f in build/vk/*.vert build/vk/*.frag shaders/*.vert shaders/*.frag shaders/*.comp; do
  [ -e "$f" ] && compile "$f"
done
tools/check-layout.sh build/vk gen/programs.c.tmp > /dev/null || { tools/check-layout.sh build/vk gen/programs.c.tmp; exit 1; }
build/shaderpack gen/shaders.h gen/shaders.c build/spv/*.spv
mv gen/programs.h.tmp gen/programs.h
mv gen/programs.c.tmp gen/programs.c
echo "shaders: $(ls build/spv/*.spv | wc -l) stages, $(echo $progs | wc -w) programs"
