# Installation

Two pieces: a script for OBS, and an add-on for ReShade. Both are needed.

## Before you start

You need **ReShade with add-on support** — the download that says "with full add-on support",
not the "addon-free" one. Add-on support is what lets native code run inside the game; the
addon-free build exists for games whose anti-cheat objects to exactly that, and the notifications
cannot work there. See [Anti-cheat](#anti-cheat) below before installing this in a multiplayer
game.

On Windows, OBS's Python scripting needs **Python 3.6** installed and pointed at in
**Tools → Scripts → Python Settings**. That is an OBS requirement, not this script's. The script
itself needs nothing beyond the standard library — no `pip install`, no pywin32.

## 1. The OBS script

1. Copy `obs-script/obs_nvidia_notify.py` anywhere you like.
2. In OBS: **Tools → Scripts → +**, and pick the file.
3. It should say `OBS Notify 1.0.0 ready` and `listening on \\.\pipe\obsn.v1.<your SID>` in the
   script log.

If the log says it could not listen, see [troubleshooting.md](troubleshooting.md).

## 2. The ReShade add-on

1. Copy `OBSNotifications.addon64` next to the game's ReShade DLL — the same folder as
   `dxgi.dll` / `d3d11.dll` / `opengl32.dll`, which is usually the folder with the game's `.exe`.
   For a 32-bit game, use the `.addon` build instead.
2. Start the game, open ReShade's menu (**Home** by default), and look under **Add-ons** for
   **OBS Notifications**.

## 3. Check it works

First, prove the pipe on its own -- one second, no game needed:

```
python obs_nvidia_notify.py --selftest
```

Use the same Python OBS is pointed at. It should end with `PASS`.

In OBS, with the game running: **Tools → Scripts**, select the script, and press
**Send a test notification**. A "Replay saved" toast should appear in the game.

If nothing appears, the ReShade menu's **OBS Notifications → Diagnostics** tab will say whether
the overlay reached the script.

You can also check the link without a game at all:

```bash
obsn-config probe 10
```

That attaches as the overlay does and prints what arrives.

## Fonts

The add-on rasterises its own typeface rather than borrowing ReShade's, so the overlay's font is
independent of ReShade's own UI. Drop a `.ttf` or `.otf` into either:

* `%APPDATA%\OBSNotifyOverlay\fonts\` — yours, and wins on a name clash; or
* the `fonts` folder beside the add-on — what shipped.

Then pick it in **Appearance → Font**. With no font available it falls back to ReShade's, which
looks fine and simply will not match the reference as closely.

## Anti-cheat

A ReShade add-on is native code injected into the game process. That is what makes it able to
draw text at all, and it is also exactly what anti-cheat systems are built to detect.

**Do not install this in a competitive multiplayer game without checking that game's policy
first.** Some permit ReShade with add-ons, some permit only the addon-free build, and some ban
for either. The consequence of getting it wrong is a ban on your account, and no notification
overlay is worth that. Single-player games and games you are recording alone are the intended
use.

## Uninstalling

* OBS: **Tools → Scripts**, select it, press **−**.
* The game: delete `OBSNotifications.addon64`.
* Settings: delete `%APPDATA%\OBSNotifyOverlay\`. Nothing is written anywhere else, no registry
  keys are created, and nothing is left running.

## Building from source

```bash
# The platform-independent core and its tests, on any OS
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure

# The add-on (Windows, MSVC)
scripts/fetch-deps.ps1
cmake -S . -B build -A x64 -DOBSN_BUILD_ADDON=ON
cmake --build build --config Release -j
```

`fetch-deps` retrieves the pinned ReShade SDK and Dear ImGui headers. Neither is redistributed
here. They must be a matched pair — including ImGui's **docking** branch — and CMake fails at
configure time with an explanation if they are not.
