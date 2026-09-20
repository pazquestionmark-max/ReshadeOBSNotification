#!/usr/bin/env bash
# Cross-compiles the Windows-only sources with MinGW as an early warning on non-Windows hosts.
#
# What this proves: the add-on's renderer, settings UI, icons, font engine and the Win32
# named-pipe transport all compile. That catches the ordinary mistakes -- missing includes,
# wrong signatures, non-copyable types passed by value -- without waiting for a Windows machine.
#
# What it does NOT prove:
#   * That anything links. This is -fsyntax-only, so unresolved symbols are invisible to it. A
#     missing #include <reshade.hpp> in a file that calls ImGui passes here and then fails at
#     link on Windows. Only the MSVC job catches that.
#   * That the shipped binary is correct. MSVC is the only supported compiler for a ReShade
#     add-on and the Windows CI job is authoritative.
#
# addon.cpp additionally cannot fully compile here: ReShade's own reshade.hpp casts a function
# pointer to void* with static_cast, which MSVC accepts as an extension and ISO C++ rejects.
# That error comes from ReShade's header, not from this project, so the script reports
# diagnostics originating in our own file separately and ignores that one.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${MINGW_CXX:-x86_64-w64-mingw32-g++}"

if ! command -v "$CXX" >/dev/null 2>&1; then
  echo "SKIP: $CXX not found (install mingw-w64, or set MINGW_CXX)"
  exit 0
fi
for dep in "$ROOT/third_party/reshade/include/reshade.hpp" "$ROOT/third_party/imgui/imgui.h"; do
  if [[ ! -f "$dep" ]]; then
    echo "SKIP: missing $dep -- run scripts/fetch-deps.sh first"
    exit 0
  fi
done

# MinGW's headers are lower-case; ReShade includes <Windows.h>. On a case-sensitive filesystem
# that needs a shim, which is a property of cross-compiling, not of the code.
SHIM="$(mktemp -d)"
trap 'rm -rf "$SHIM"' EXIT
echo '#include <windows.h>' > "$SHIM/Windows.h"

INCLUDES=(
  -I"$SHIM"
  -I"$ROOT/shared/include"
  -I"$ROOT/reshade-integration/include"
  -I"$ROOT/third_party/imgui"
  -I"$ROOT/third_party/reshade/include"
)
# -Wshadow earns its place: MSVC's C4457 (a local hiding a parameter) is an error under
# warnings-as-errors in CI, and -Wall -Wextra alone does not report it.
FLAGS=(-std=c++17 -fsyntax-only -Wall -Wextra -Wshadow
       -DOBSN_VERSION='"1.0.0"' -DOBSN_BUILD_ID='"check"'
       -DWIN32_LEAN_AND_MEAN -DNOMINMAX -include objbase.h)

SOURCES=(
  shared/src/transport_win32.cpp
  shared/src/profile_store.cpp
  reshade-integration/src/icons.cpp
  reshade-integration/src/font_engine.cpp
  reshade-integration/src/renderer.cpp
  reshade-integration/src/settings_ui.cpp
)

status=0
for source in "${SOURCES[@]}"; do
  printf '%-44s ' "$source"
  if output="$("$CXX" "${FLAGS[@]}" "${INCLUDES[@]}" "$ROOT/$source" 2>&1)"; then
    echo "OK"
  else
    echo "FAIL"
    echo "$output" | head -20
    status=1
  fi
done

# addon.cpp is checked differently: only diagnostics from our own file count.
printf '%-44s ' "reshade-integration/src/addon.cpp"
output="$("$CXX" "${FLAGS[@]}" "${INCLUDES[@]}" "$ROOT/reshade-integration/src/addon.cpp" 2>&1)"
ours="$(echo "$output" | grep ": error" | grep "addon.cpp" || true)"
if [[ -n "$ours" ]]; then
  echo "FAIL"
  echo "$ours"
  status=1
else
  echo "OK (ReShade header errors under MinGW are expected; MSVC is authoritative)"
fi

exit $status
