# Configuration

Everything is configured **in game**, in ReShade's menu, under **OBS Notifications**. Nothing
needs to be edited by hand, and nothing needs OBS to be restarted.

Profiles live in `%APPDATA%\OBSNotifyOverlay\` on Windows (`~/.config/obsn/` elsewhere) as JSON.
The settings window prints the path.

## The two halves

| Where | Decides |
|---|---|
| **The OBS script** (Tools → Scripts) | *What is sent.* Which categories of event leave OBS, and whether file paths and sizes go with them. |
| **ReShade's menu** (in game) | *How it looks.* Colours, position, wording, animation, everything else. |

That split is deliberate: the privacy decisions are made where the data is, and the appearance
decisions are made where you can see them.

## Tabs

**Notifications** — placement, the box, the shared motion, and then every category. Each category
has its own enable switch, wording, icon, accent colour, timing and a **Test** button that raises
one sample toast through the real formatter.

**Appearance** — typeface, text sizes and weights, the house accent, shadows and outlines.

**Status** — the optional always-on indicator that something is being captured.

**Preview** — sample state and one toast of every enabled category, on a loop. Nothing is sent
over the pipe and nothing is recorded.

**Profiles** — named profiles, and per-game automatic selection.

**Diagnostics** — link state, what OBS is doing, render timings, and every repair the loader had
to make to your configuration.

**Link** (with "Every setting" on) — pipe name, reconnect timings, logging.

## Placeholders

Any title or detail template may use these. An unknown one is left visible, so a typo is
something you can see and fix rather than a silent gap.

| Placeholder | Expands to |
|---|---|
| `{file}` | The file name alone — `2026-09-20 21-14-03.mkv` |
| `{path}` | The full path. Empty unless the script is set to send it. |
| `{folder}` | The directory the file is in. Same condition. |
| `{duration}` | `12:07`, or `1:04:22` past an hour |
| `{size}` | `1.4 GB` |
| `{scene}` / `{previous_scene}` | Scene names |
| `{profile}` | The OBS profile |
| `{service}` | `Twitch`, `YouTube - RTMPS`, … |
| `{reason}` | Why an output stopped |
| `{detail}` | A warning's text, as OBS described it |
| `{time}` | Wall-clock, as the producer formatted it |
| `{attempt}` | Reconnect attempt number |
| `{replay_seconds}` | Length of a saved clip |
| `{elapsed}` | How long the relevant output has been running |

**A placeholder with nothing to put in it disappears, and takes one adjacent space with it.** So
`Recording saved {file}` reads as `Recording saved` when OBS did not report a name, not
`Recording saved ` with a trailing space.

## Motion

`notifications.motion` governs how a toast arrives and leaves. It is a separate axis from the
fade, so a slide with no fade, or a fade with no slide, are each one setting away.

| Setting | Effect |
|---|---|
| `kind` | `slide_from_edge` (the default — travels in from whichever screen edge it is anchored to), `slide_horizontal`, `slide_vertical`, `scale`, `none` |
| `distance` | Pixels travelled. **0 means the toast's own width**, which is what starts it fully outside the screen edge. |
| `in_easing` / `out_easing` | The curves. The default pair is `ease_out_quint` in and `ease_in_cubic` out. |
| `exit_slides` | Off leaves the toast in place and only fades it. |

A category can override the shared motion (`override_motion`), for something that deserves to
arrive differently.

Turning off **Animate** in the Appearance tab pins every toast in place and uses a plain fade.
That is the setting for someone who finds motion distracting, not a debug aid.

## Ordering and overflow

`max_visible` bounds the stack. When more arrive than fit, the **lowest priority** is dropped
first, and among equals the oldest. A dropped-frames warning outliving a scene change is the
right trade; the reverse is not. A toast is never dropped by the very submission that created
it.

`merge_duplicates` folds an identical toast into the one already on screen, restarting its
lifecycle and showing a repeat count, rather than stacking two copies of the same sentence.

`min_interval_ms` is a per-category floor. Two *different* events from one category arriving
inside that window replace each other in place instead of stacking — six scene changes as a
collection loads should cost one toast, and the last one is the true one.

## Per-game profiles

With **Pick a profile automatically per game** on, the add-on matches the game's executable name
against the profile mappings at load and falls back to `default`. So a competitive shooter can
have a quieter configuration than a single-player game without touching anything between
sessions.

## Checking a profile by hand

```bash
obsn-config validate my-profile.json    # every repair the loader would make
obsn-config normalise my-profile.json   # what the loader actually understood
obsn-config defaults                    # the shipped look, to diff against
```

`examples/profiles/` has six starting points, all validated in CI.
