# Architecture

```
OBS ──► obs_nvidia_notify.py ──► named pipe ──► OBSNotifications.addon64 ──► the game's frame
        (producer, listens)      (local only)   (consumer, connects)
```

## Why an add-on and not a shader

A ReShade effect (`.fx`) is a GPU shader. It cannot open a pipe, read a file, hold a string or
lay out text, so it cannot display a file name — no amount of cleverness changes that, and this
repository ships no `.fx` file pretending otherwise.

A ReShade **add-on** is native code loaded into the game process, handed ReShade's own Dear ImGui
context. It can open a pipe and draw text.

Two registrations, each chosen for when ReShade invokes it:

* **`addon_event::reshade_overlay`** — called every frame, between `ImGui::NewFrame` and
  `ImGui::EndFrame`. ReShade's early-out explicitly keeps building an ImGui frame when an add-on
  has subscribed to this event, which is what makes an always-on overlay possible. We draw into
  the *background* draw list, so we add no draw call batch of our own and take no input.
* **`register_overlay("OBS Notifications")`** — called only while ReShade's menu is open. That
  is exactly when the user is configuring and when ReShade is already blocking game input, so it
  is the right home for the settings window.

## Why OBS listens and the game connects

OBS runs for a whole session; games start and stop. The long-lived side listens, the transient
side carries the reconnect logic, and several games can attach at once. Reversing it would put
an accepting server inside every game process and leave OBS polling for games that might exist.

## Threading

**The render thread never blocks.** Its entire interaction with the link is `latest()`, one
atomic exchange against a triple buffer, and `drain_events()`, one short mutex. Connecting,
blocking reads, JSON parsing and state application all happen on `OverlayClient`'s own thread.

**OBS's UI thread never blocks.** The frontend callback builds a dictionary and puts it on a
queue. Everything after that — stat'ing a just-written file, serialising a snapshot, writing to
a pipe — happens on the script's dispatcher thread. Per-client write queues are bounded and drop
the oldest on overflow, then force a resynchronisation.

**Neither end can stall the other.** A game that stops reading costs the script a few dropped
events. A script that goes quiet costs the overlay a `stale` flag, not a frozen frame.

## Layers

```
shared/                  platform-independent C++17 core. No OBS, ReShade, ImGui or Win32.
  json, framing          bounded parsing, bounded accumulation
  model                  what OBS is doing, and what just happened
  protocol               the envelope and its payloads
  config                 versioned, self-repairing, round-trip-lossless
  layout                 placement, text fitting, template expansion, motion
  notifications          the toast lifecycle
  transport              named pipe (Windows) / AF_UNIX (POSIX)
  overlay_client         the link, with reconnect and staleness
  profile_store          named profiles, atomic saves, per-game selection

reshade-integration/     the add-on. Everything that touches ImGui lives here.
  renderer               the toast painter and the status indicator
  icons                  vector glyphs, no image assets
  font_engine            the overlay's own typeface, rasterised with stb_truetype
  settings_ui            the in-game settings window
  addon                  entry point, event registration, lifetime

obs-script/              the OBS script. Python 3.6, standard library only.
tools/config-tool/       obsn-config: defaults, validation, and a probe
tools/obsn-cli/          TypeScript: a mock OBS, and a second implementation to check against
```

`shared/` has no third-party dependency. Every byte of it is loaded into someone else's game,
which is reason enough; it is also what makes the protocol, configuration, layout and
notification lifecycle testable on any machine, which is why the test suite genuinely runs in CI
rather than being aspirational.

## Design commitments

These are testable claims, not aspirations. Each is enforced by a test.

* **The renderer never blocks.** One atomic exchange per frame; no pipe handle is ever touched
  on the render thread.
* **OBS's callback thread never blocks.** Events are queued, never written synchronously.
* **Absent is not zero.** A duration OBS did not report is `-1`, not `0`, all the way through —
  and a placeholder with nothing to put in it disappears rather than printing `0:00`.
* **A stale state is never presented as live.** After the configured window with no traffic, the
  overlay stops claiming to know what OBS is doing.
* **Nothing is removed mid-animation.** Expiry marks a toast for its exit; removal happens only
  once the exit has finished.
* **A flood cannot consume the screen.** The stack is hard-bounded and evicts by priority.
* **Paths do not leave OBS unless asked.** The script sends the file name alone by default.
* **Nothing reaches the network.** Neither component opens a socket.

## Why the ImGui font atlas is not touched

ReShade owns the Dear ImGui font atlas, and `ImFontAtlas` is deliberately absent from the
function table ReShade exports to add-ons. Reading `io.Fonts` from an add-on means dereferencing
struct offsets taken from *this* build's `imgui.h` against memory laid out by ReShade's own ImGui
build; when those differ it is a wild pointer read.

So the add-on does not. It rasterises a `.ttf` itself, uploads the result through ReShade's
device API, and draws glyph quads from it. Two consequences worth stating: the font applies to
the overlay alone, and text measurement becomes ours — widths come from the same advance table
the glyphs are drawn from, so right-alignment cannot disagree with what is on screen.

The same reasoning applies to the viewport size, which comes from
`get_screenshot_width_and_height` — a virtual call across a versioned interface — rather than
from `ImGuiIO::DisplaySize`.
