# What the defaults are reproducing, and how closely

The brief was that the notifications be "to a T" the same as NVIDIA's. This page says exactly
what that means here, which figures are measured and which are judged, so the claim is
checkable rather than asserted.

## What was and was not copied

**Nothing of NVIDIA's is in this repository.** No image, font, sound or line of code was taken
from NVIDIA software. Every glyph in `reshade-integration/src/icons.cpp` is drawn from
primitives, and they are the ordinary universal marks — a filled circle for record, a square for
stop, two bars for pause, an arrow into a tray for save, a circular arrow for replay. The NVIDIA
logo and the GeForce eye are **not** reproduced, and no setting will produce them; they are
trademarks and drawing them is not something a third-party add-on should do.

What was reproduced is the *design*: the shape of the panel, the relationship between the
elements, the wording style and the motion. That is what you see, and it is what the defaults
match.

## The reference form

A capture notification in NVIDIA's overlay is:

* a near-black, near-opaque panel, anchored near a screen corner;
* a square region on the leading edge, filled with an accent colour, with a dark glyph in it;
* a short title in a medium weight, white;
* an optional second line in a smaller, grey type;
* corners that are very nearly square;
* an entry that travels in from the screen edge it is anchored to, decelerating into place
  rather than bouncing, then a hold, then a reverse.

Every one of those is a setting here. `notifications.box.accent_style` is `tile`,
`corner_radius` is 2, `motion.kind` is `slide_from_edge` with `ease_out_quint`, and so on.

## The figures

| Setting | Default | How it was arrived at |
|---|---|---|
| `box.background` | `#111111F2` | Judged. A near-black panel at ~95% opacity. Sampling a screenshot gives a value that depends on what was behind it, so this is chosen to read the same way rather than to match a pixel. |
| `box.corner_radius` | 2 | Measured, from screenshots at 1080p and 1440p. The reference is very nearly square but not perfectly. |
| `box.accent_style` | `tile` | Observed. The icon sits in a filled square on the leading edge. |
| `box.accent_tile_width` | 52 | Measured at 1080p, as roughly the panel height. |
| `appearance.accent` | `#76B900` | NVIDIA's published brand green. This one is exact. |
| `appearance.title_size` | 15 | Measured at 1080p, ±1px. |
| `appearance.detail_size` | 13 | Measured at 1080p, ±1px. |
| `appearance.detail_color` | `#A8ACB0` | Judged. Clearly subordinate to the title without being unreadable. |
| `placement` | top-right, 32px | Observed default corner. The reference lets you pick a corner; so does this. |
| `fade.in_ms` | 260 | Estimated by frame-stepping capture footage at 60fps: about 15–16 frames. |
| `fade.hold_ms` | 3200 | Estimated the same way. The reference is somewhere around three seconds; this is not a precise figure and is the one most worth adjusting to taste. |
| `fade.out_ms` | 220 | Estimated by frame-stepping: shorter than the entry, which is what makes it feel like a dismissal rather than a second animation. |
| `motion.in_easing` | `ease_out_quint` | Judged. The reference decelerates hard and settles without overshooting; a quintic ease-out is the standard curve with that character. |
| `motion.out_easing` | `ease_in_cubic` | Judged, on the same basis. |

### What "judged" means

It means someone looked at both and adjusted until they read the same, rather than extracting a
number from a file. Colours sampled from a screenshot are contaminated by whatever was behind
the panel; durations read off footage are quantised to the frame rate. Where a figure could be
measured it was, and the table says which.

**None of it is guesswork you are stuck with.** Every figure above is a setting in ReShade's
menu, and `examples/profiles/default.json` is the shipped look written out so you can diff a
profile you have changed against it.

## Wording

The reference uses short verb phrases in sentence case with no trailing full stop — "Recording
started", not "Recording Started." or "Your recording has started". The defaults follow that,
including the one place the reference does something different: the replay buffer is phrased as
a capability being switched on ("Replay buffer is on") rather than an output that started,
because nothing is being written yet.

Every string is a template you can rewrite, with placeholders listed in
[configuration.md](configuration.md).

## What is deliberately not the same

* **No overlay hotkeys, no clip editor, no share flow.** This shows notifications. OBS does the
  capturing and already has hotkeys for it.
* **A warning category the reference has no equivalent for.** Dropped frames and a filling disk
  are things OBS knows and NVIDIA's overlay does not, and they are worth interrupting for.
* **A status indicator that is off by default.** The reference keeps a small mark on screen
  while recording. This can do that (`status.placement.visible`), but it is opt-in: it is
  permanently on screen and that is the user's call.

## Verifying it yourself

```bash
cd tools/obsn-cli && npx tsx src/cli.ts mock --scenario session
```

That plays a capture session into the overlay with no OBS and no game involved, so you can sit
and watch the animation against a reference recording. `--scenario burst` shows the stack under
pressure; `--scenario awkward` shows the cases that break layouts.
