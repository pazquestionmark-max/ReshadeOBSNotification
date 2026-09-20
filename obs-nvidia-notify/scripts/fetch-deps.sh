#!/usr/bin/env bash
# Fetches the third-party headers the add-on builds against. Nothing here is redistributed in
# the repository; each dependency is pinned to an exact tag so a build is reproducible.
#
# Usage: scripts/fetch-deps.sh [--reshade-tag vX.Y.Z] [--imgui-tag vX.Y.Z-docking]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENDOR="$ROOT/third_party"

# Pinned pair. Two separate constraints, both load-bearing:
#   1. ReShade resolves the ImGui function table by exact IMGUI_VERSION_NUM, so the two versions
#      must match.
#   2. It must be ImGui's *docking* branch. ReShade's imgui_function_table declares DockSpace,
#      ImGuiDockNodeFlags and ImGuiWindowClass, which exist only there; against master the
#      add-on does not compile.
RESHADE_TAG="v6.4.1"          # RESHADE_API_VERSION 16
IMGUI_TAG="v1.91.8-docking"   # IMGUI_VERSION_NUM 19180, docking branch

while [[ $# -gt 0 ]]; do
  case "$1" in
    --reshade-tag) RESHADE_TAG="$2"; shift 2 ;;
    --imgui-tag)   IMGUI_TAG="$2";   shift 2 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

mkdir -p "$VENDOR"

if [[ ! -d "$VENDOR/reshade/.git" ]]; then
  echo "==> fetching the ReShade SDK @ $RESHADE_TAG"
  rm -rf "$VENDOR/reshade"
  git clone --depth 1 --filter=blob:none --sparse --branch "$RESHADE_TAG" \
    https://github.com/crosire/reshade.git "$VENDOR/reshade"
  git -C "$VENDOR/reshade" sparse-checkout set include
else
  echo "==> $VENDOR/reshade already present; skipping"
fi

if [[ ! -d "$VENDOR/imgui/.git" ]]; then
  echo "==> fetching Dear ImGui @ $IMGUI_TAG"
  rm -rf "$VENDOR/imgui"
  git clone --depth 1 --branch "$IMGUI_TAG" https://github.com/ocornut/imgui.git "$VENDOR/imgui"
else
  echo "==> $VENDOR/imgui already present; skipping"
fi

echo
echo "Dependencies are in $VENDOR:"
echo "  ReShade SDK   $RESHADE_TAG"
echo "  Dear ImGui    $IMGUI_TAG"
echo
echo "Verify the pairing before building the add-on:"
echo "  grep -m1 'IMGUI_VERSION_NUM !=' $VENDOR/reshade/include/reshade_overlay.hpp"
echo "  grep -m1 'define IMGUI_VERSION_NUM' $VENDOR/imgui/imgui.h"
echo "These two numbers must be equal."
