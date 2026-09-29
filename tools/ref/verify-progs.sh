#!/bin/sh
# Dev-only: assembles each .prog for every tier and checks that an identical program (vertex
# and fragment text) exists among the shaders captured from the JS app for that tier.
#   tools/ref/verify-progs.sh [shaders/programs/X.prog ...]     (run from c/)
set -u
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
progs=${*:-shaders/programs/*.prog}
fail=0
for p in $progs; do
  line="$(basename "$p" .prog):"
  for t in high medium low ultra; do
    cap=tools/ref/out/shaders-$t
    [ -d "$cap" ] || { line="$line $t=nocapture"; continue; }
    build/shadergen assemble "$p" "$t" "$tmp/a.vert" "$tmp/a.frag" 2>"$tmp/err"
    rc=$?
    if [ $rc = 3 ]; then line="$line $t=-"; continue; fi
    if [ $rc != 0 ]; then line="$line $t=ERROR($(head -c 200 "$tmp/err"))"; fail=1; continue; fi
    hit=""
    for v in "$cap"/*.vert; do
      id=$(basename "$v" .vert)
      if cmp -s "$tmp/a.vert" "$v" && cmp -s "$tmp/a.frag" "$cap/$id.frag"; then hit=$id; break; fi
    done
    if [ -n "$hit" ]; then line="$line $t=$hit"
    elif grep -q "HAND-DERIVED" "$p"; then line="$line $t=hand"   # no capture exists (see its header)
    else line="$line $t=MISMATCH"; fail=1; fi
  done
  echo "$line"
done
exit $fail
