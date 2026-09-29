#!/bin/sh
# Cross-checks the std140 offsets shadergen computed (gen/programs.c) against glslang's own
# reflection of each compiled stage. Only active uniforms appear in glslang's reflection;
# every one of them must match. Usage: tools/check-layout.sh VKDIR GEN_C
set -u
vkdir=$1 genc=$2
fail=0
for f in "$vkdir"/*.vert "$vkdir"/*.frag; do
  [ -e "$f" ] || continue
  base=$(basename "$f")
  ident=$(echo "$base" | sed 's/\.vert$/_v/; s/\.frag$/_f/' | tr 'a-z.-' 'A-Z__')
  stem=$(echo "$ident" | sed 's/_[VF]$//')
  st=$(echo "$ident" | sed 's/.*_\([VF]\)$/\1/' | tr 'VF' 'vf')
  # glslang: "name: offset N, ..." for block members (plain names; arrays of structs as a.b with stride)
  ${GLSLANG:-glslangValidator} -V -q --target-env vulkan1.0 -o /dev/null "$f" 2>/dev/null |
    sed -n '/^Uniform reflection:/,/^Uniform block reflection:/p' | grep 'offset [0-9]' |
    sed 's/^\([^:]*\): offset \([0-9]*\),.*/\1 \2/' > /tmp/od-layout.$$
  while read -r name off; do
    # a.b (struct array member): glslang reports element 0
    probe=$(echo "$name" | sed 's/^\([^.]*\)\./\1[0]./')
    ours=$(sed -n "/static const UniformLeaf ${stem}_${st}_u[01]\[\]/,/nullptr/p" "$genc" |
      grep -F "\"$probe\"" | head -1 | sed 's/.*", \([0-9]*\),.*/\1/')
    if [ -z "$ours" ]; then
      ours=$(sed -n "/static const UniformLeaf ${stem}_${st}_u[01]\[\]/,/nullptr/p" "$genc" |
        grep -F "\"$name[0]\"" | head -1 | sed 's/.*", \([0-9]*\),.*/\1/')
    fi
    if [ "$ours" != "$off" ]; then
      echo "LAYOUT MISMATCH $base: $name glslang=$off shadergen=${ours:-missing}"
      fail=1
    fi
  done < /tmp/od-layout.$$
  rm -f /tmp/od-layout.$$
done
[ $fail = 0 ] && echo "uniform layouts match glslang reflection"
exit $fail
