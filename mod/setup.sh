#!/usr/bin/env bash
# Fetches the AML mod headers (same source and layout ProMenu uses). Run from anywhere; works on the repo root.
set -euo pipefail
cd "$(dirname "$0")/.."

rm -rf aml mod/include
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/RusJJ/AndroidModLoader aml

# AML headers must be reachable as <mod/amlmod.h>
mkdir -p mod/include
cp -r aml/mod mod/include/mod

# extra include dirs some AML headers need (gloss.h etc.) - paths are relative to mod/ (LOCAL_PATH)
INC=""
for d in aml/include aml/ARMPatch aml/ARMPatch/include; do
  if [ -d "$d" ]; then INC="$INC \$(LOCAL_PATH)/../$d"; fi
done
G="$(find aml -name gloss.h -print -quit || true)"
if [ -n "$G" ]; then INC="$INC \$(LOCAL_PATH)/../$(dirname "$G")"; fi
echo "AML_EXTRA_INC :=$INC" > mod/aml_inc.mk

echo "--- AML mod/ folder:"; ls mod/include/mod
echo "OK: AML headers ready"
