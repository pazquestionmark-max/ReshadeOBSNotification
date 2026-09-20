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

# Diagnostics are matched against repository-relative paths, so compile from the root.
cd "$ROOT"

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
# The warning set is chosen to approximate MSVC /W4, which CI builds the add-on under with
# /WX. Each of these stands in for a specific MSVC warning that would otherwise fail that build
# minutes later, on the one job this script exists to pre-empt:
#
#   -Wshadow                       C4456/C4457/C4458, a local hiding a local, parameter or member
#   -Wconversion                   C4244/C4267, a narrowing conversion that may lose data
#   -Wsign-conversion              C4245, a signed/unsigned mismatch
#   -Wunused-parameter (-Wextra)   C4100, an unreferenced formal parameter
#
# CMake already applies the conversion warnings to everything it builds; the add-on's own
# sources are not in that build on a Linux host, which is exactly why they are applied here.
FLAGS=(-std=c++17 -fsyntax-only -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion
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
  output="$("$CXX" "${FLAGS[@]}" "${INCLUDES[@]}" "$source" 2>&1)"
  compiled=$?
  # Diagnostics from third_party are not ours to fix and are filtered out; anything left that
  # names one of our own files counts, warning or error, because CI builds this with /WX.
  ours="$(echo "$output" | grep -E '^(shared|reshade-integration)/' || true)"
  if [[ $compiled -eq 0 && -z "$ours" ]]; then
    echo "OK"
  else
    echo "FAIL"
    echo "${ours:-$output}" | head -20
    status=1
  fi
done

# addon.cpp is checked differently, because two of the diagnostics it produces under MinGW say
# nothing about whether it is correct:
#
#  * ReShade's own reshade.hpp static_casts a function pointer to void*, which MSVC accepts as
#    an extension and ISO C++ rejects. The error names reshade.hpp, so filtering to our own file
#    removes it -- but GCC also prints "required from here" notes that *do* name addon.cpp, as
#    instantiation context. Those are notes, not diagnostics, and are excluded by kind.
#
#  * `extern "C" __declspec(dllexport) const char* NAME = "..."` is the declaration form ReShade
#    requires of every add-on; without it the add-on has no name in ReShade's list. GCC warns
#    that an extern is initialised at its declaration. MSVC does not, and changing it to satisfy
#    GCC would break the thing it exists to do.
#
# Everything else originating in addon.cpp counts, warning or error.
printf '%-44s ' "reshade-integration/src/addon.cpp"
output="$("$CXX" "${FLAGS[@]}" "${INCLUDES[@]}" "reshade-integration/src/addon.cpp" 2>&1)"
ours="$(echo "$output" \
  | grep -E '^reshade-integration/src/addon\.cpp:[0-9]+:[0-9]+: (error|warning):' \
  | grep -v "initialized and declared 'extern'" || true)"
if [[ -n "$ours" ]]; then
  echo "FAIL"
  echo "$ours"
  status=1
else
  echo "OK (ReShade header errors under MinGW are expected; MSVC is authoritative)"
fi

exit $status
