# Fetches the third-party headers the add-on builds against.
#
# Nothing here is redistributed in the repository; each dependency is pinned to an exact tag so
# a build is reproducible.
[CmdletBinding()]
param(
  # Pinned pair. Two separate constraints, both load-bearing:
  #   1. ReShade resolves the ImGui function table by exact IMGUI_VERSION_NUM, so the two
  #      versions must match.
  #   2. It must be ImGui's *docking* branch. ReShade's imgui_function_table declares DockSpace,
  #      ImGuiDockNodeFlags and ImGuiWindowClass, which exist only there.
  [string]$ReshadeTag = "v6.4.1",
  [string]$ImguiTag = "v1.91.8-docking"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$vendor = Join-Path $root "third_party"
New-Item -ItemType Directory -Force -Path $vendor | Out-Null

$reshade = Join-Path $vendor "reshade"
if (-not (Test-Path (Join-Path $reshade ".git"))) {
  Write-Host "==> fetching the ReShade SDK @ $ReshadeTag"
  if (Test-Path $reshade) { Remove-Item -Recurse -Force $reshade }
  git clone --depth 1 --filter=blob:none --sparse --branch $ReshadeTag `
    https://github.com/crosire/reshade.git $reshade
  git -C $reshade sparse-checkout set include
} else {
  Write-Host "==> $reshade already present; skipping"
}

$imgui = Join-Path $vendor "imgui"
if (-not (Test-Path (Join-Path $imgui ".git"))) {
  Write-Host "==> fetching Dear ImGui @ $ImguiTag"
  if (Test-Path $imgui) { Remove-Item -Recurse -Force $imgui }
  git clone --depth 1 --branch $ImguiTag https://github.com/ocornut/imgui.git $imgui
} else {
  Write-Host "==> $imgui already present; skipping"
}

Write-Host ""
Write-Host "Dependencies are in ${vendor}:"
Write-Host "  ReShade SDK   $ReshadeTag"
Write-Host "  Dear ImGui    $ImguiTag"
Write-Host ""
Write-Host "Verify the pairing before building the add-on: these two numbers must be equal."
Select-String -Path (Join-Path $reshade "include\reshade_overlay.hpp") -Pattern "IMGUI_VERSION_NUM !=" | Select-Object -First 1
Select-String -Path (Join-Path $imgui "imgui.h") -Pattern "define IMGUI_VERSION_NUM" | Select-Object -First 1
