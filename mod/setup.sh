#!/usr/bin/env bash
# Fetches ImGui (for the in-game menu) and the AML mod headers (same sources and layout ProMenu uses).
# Run from anywhere; works on the repo root.
set -euo pipefail
cd "$(dirname "$0")/.."

IMGUI_TAG="${IMGUI_TAG:-v1.91.9}"

rm -rf aml mod/include mod/imgui
git clone --depth 1 --branch "$IMGUI_TAG" https://github.com/ocornut/imgui mod/imgui
git clone --depth 1 --recurse-submodules --shallow-submodules https://github.com/RusJJ/AndroidModLoader aml

# keep ImGui symbols hidden: another mod (ProMenu) ships its own ImGui copy in the same process
printf '\n#define IMGUI_API __attribute__((visibility("hidden")))\n' >> mod/imgui/imconfig.h

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
echo "OK: imgui $IMGUI_TAG + AML headers ready"
