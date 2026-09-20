# Fonts

No font file is committed here.

The add-on rasterises its own typeface rather than borrowing ReShade's, so the overlay's font is
independent of ReShade's own UI. It looks in two places, in this order:

1. `%APPDATA%\OBSNotifyOverlay\fonts\` — yours, and wins on a name clash
2. this folder, beside the add-on — whatever shipped with the release

Drop a `.ttf`, `.otf` or `.ttc` in either and pick it in **Appearance → Font**.

With no font available the overlay falls back to ReShade's own, which is perfectly readable and
simply will not match the reference as closely.

## Matching the reference

The default configuration asks for `Inter-Regular.ttf`, at weight 600 for the title and 400 for
the detail line. Inter is a good stand-in for the reference's typeface and is licensed under the
SIL Open Font License, so you may redistribute it with a build of this add-on. It is not
committed here because this repository does not vendor third-party assets.

Any humanist sans will look close. What matters more than the face is the weight contrast
between the two lines.
