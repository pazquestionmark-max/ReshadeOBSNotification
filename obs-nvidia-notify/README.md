# OBS Notifications for ReShade

NVIDIA-style capture notifications, in game, driven by OBS.

Start a recording, save a replay, arm the buffer — and the notification appears over the game,
in the same shape, the same colours and the same motion as the overlay you already know.

```
┌──────────┬────────────────────────────────┐
│          │  Replay saved                  │
│   save   │  Replay 2026-09-20 21-16-42.mkv│
│          │                                │
└──────────┴────────────────────────────────┘
   accent          title over detail
    tile
```

> **Status: the logic is thoroughly tested; the Windows binaries are not yet field-tested.**
> The protocol, configuration, layout, motion and notification lifecycle are covered by
> automated tests on every push, including the real OBS script driving the real overlay client
> over a real socket and asserting the exact toast each event produces. The Windows build is
> defined in CI and every Windows-only source compiles, but the add-on has not been rendered in
> a real game against a live OBS by the author. [`docs/testing.md`](docs/testing.md) is explicit
> about which is which.

## What you get

Notifications for everything OBS can tell us about:

* **Recording** — started, stopped, paused, resumed, and saved, with the file name and how long
  it ran.
* **Replay buffer** — armed, disarmed, and every clip you save, which is the one that matters
  most because it happens mid-play.
* **Streaming** — started, stopped, and the reconnect attempts you want to know about before the
  stream is gone.
* **Virtual camera**, scene changes, and warnings OBS's own overlay cannot give you: dropped
  frames, and a disk filling up while you are still recording to it.

Plus an optional always-on indicator with a recording timer, for when the question is "am I
still recording?" rather than "what just happened?".

Every colour, position, size, icon, wording and animation is configurable **in game**, without
editing a file. Six starting profiles ship with it, and a profile can be selected automatically
per game.

## How faithful is it, really

Faithful enough to be the point of the project, and
[`docs/nvidia-reference.md`](docs/nvidia-reference.md) says exactly how faithful, figure by
figure, including which numbers were measured off screenshots and which were judged by eye.

**Nothing of NVIDIA's is in this repository.** No image, font, sound or line of code. Every
icon is drawn from primitives and they are the ordinary universal marks — a filled circle for
record, a square for stop, an arrow into a tray for save. The logo is not reproduced and no
setting will produce it.

## Install

You need **ReShade with add-on support** (not the "addon-free" download), and OBS with Python
scripting configured.

1. `obs-script/obs_nvidia_notify.py` → OBS, **Tools → Scripts → +**.
2. `OBSNotifications.addon64` → next to the game's ReShade DLL.
3. In game: **Home → Add-ons → OBS Notifications**.

Then press **Send a test notification** in the script's settings to check the chain.

Full instructions, including the anti-cheat warning you should read before putting this in a
multiplayer game: [`docs/installation.md`](docs/installation.md).

## Layout

```
shared/                platform-independent C++17 core — no OBS, ReShade, ImGui or Win32
reshade-integration/   the ReShade add-on: renderer, icons, font engine, settings window
obs-script/            the OBS script: Python 3.6, standard library only
tools/config-tool/     obsn-config: defaults, validation, and a probe for the live link
tools/obsn-cli/        TypeScript: a mock OBS, and a second implementation checked against the first
tests/                 C++ unit and integration tests
examples/profiles/     six starting configurations, validated in CI
docs/                  architecture, protocol, configuration, fidelity, testing, troubleshooting
```

`shared/` carries no third-party dependency. Every byte of it is loaded into someone else's
game, which is reason enough; it is also what makes the whole pipeline short of the draw calls
testable on any machine.

## Design commitments

Testable claims, not aspirations. Each is enforced by a test.

* **The renderer never blocks.** All IPC, parsing and state application happen on a background
  thread; the render thread does one atomic exchange against a triple buffer.
* **OBS's UI thread never blocks.** Frontend events are queued, never written synchronously. A
  game that has stopped reading costs a bounded queue a few dropped events, not OBS a stutter.
* **Absent is not zero.** A duration OBS did not report stays absent all the way through, and
  the placeholder disappears rather than printing `0:00`.
* **A stale state is never presented as live.** After the configured window with no traffic, the
  overlay stops claiming to know what OBS is doing.
* **Nothing is removed mid-animation.** Expiry marks a toast for its exit; removal happens only
  once the exit has finished.
* **Your paths do not leave OBS unless you ask.** The script sends the file name alone by
  default, because a full path contains your account name.
* **Nothing reaches the network.** Neither component opens a socket.

## Development

```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
python3 -m unittest discover -s obs-script/tests -v
cd tools/obsn-cli && npm install && npm test
scripts/cross-language-check.sh           # the OBS script against the overlay client, for real
scripts/check-windows-sources.sh          # cross-compile the Windows-only sources with MinGW
```

See the overlay without OBS and without a game:

```bash
cd tools/obsn-cli && npx tsx src/cli.ts mock --scenario session
```

`scripts/fetch-deps.sh` (or `.ps1`) retrieves the pinned ReShade SDK and Dear ImGui headers.
Neither is redistributed here. They must be a matched pair — including ImGui's **docking**
branch — and CMake fails with an explanation if they are not.

## Documentation

| | |
|---|---|
| [installation.md](docs/installation.md) | Setup, fonts, anti-cheat, building from source |
| [configuration.md](docs/configuration.md) | Every setting, every placeholder |
| [nvidia-reference.md](docs/nvidia-reference.md) | What the defaults reproduce, and how closely |
| [architecture.md](docs/architecture.md) | Why an add-on, threading, why the font atlas is not touched |
| [protocol.md](docs/protocol.md) | The wire format, flow control, security |
| [testing.md](docs/testing.md) | What is automated, and the manual matrix that is not |
| [troubleshooting.md](docs/troubleshooting.md) | Symptom-first diagnosis |

## Licence

MIT — see [LICENSE](LICENSE). Not affiliated with NVIDIA Corporation, the OBS Project, or the
ReShade project. "NVIDIA" and "GeForce" are trademarks of NVIDIA Corporation, used here only to
describe what the default styling resembles.
