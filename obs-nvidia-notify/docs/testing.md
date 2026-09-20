# What is tested, and what is not

## Automated

Everything below runs on every push.

| Suite | Covers |
|---|---|
| `ctest` (C++) | JSON, framing, the protocol, the configuration loader, layout and text fitting, the motion curves, the notification lifecycle, the triple buffer, profile storage, and end-to-end IPC over a real socket |
| `python -m unittest` | The OBS script's protocol, its state machine, the privacy rules, the bounded client queue, and the server over a real socket |
| `node --test` (TypeScript) | A second implementation of the wire format, and **parity against the C++ one**: the schema, the category list, every enum value and every example profile |
| `scripts/cross-language-check.sh` | The real OBS script driving the real overlay client, asserting the exact toast text each event produces |
| `scripts/check-windows-sources.sh` | The Windows-only sources cross-compiled with MinGW |
| Windows CI | The authoritative MSVC build of the add-on |

### What the parity tests are for

The C++ loader is the authority — it is what the add-on runs. The TypeScript schema is written
independently and CI asserts that they describe the same thing, **in both directions**: a
setting the loader writes that the schema does not know about, or a setting the schema describes
that the loader never writes, both fail. That is what catches a field added to the struct and
forgotten everywhere else.

The enum parity test goes further: every value the schema lists is written into a profile, passed
through the real loader, and checked to come back unchanged. A value the loader does not
recognise is silently replaced with the default, so a round-trip that does not preserve it is how
a stale list is found.

### What the cross-language check is for

Two implementations agreeing about a format on paper is not the same as two programs agreeing
over a socket. That script stands up the Python server exactly as OBS would, plays a scripted
capture session through it, and asserts that the C++ client receives every event **and produces
the exact toast the shipped configuration describes** — `Replay saved / Replay 2026-09-20.mkv`,
not merely "an event arrived".

## Not automated

Honesty about the gaps matters more than a longer table.

**The add-on has not been run on Windows hardware by the author.** The Windows build is defined
in CI and every Windows-only source compiles under both MinGW and MSVC, but nothing here has
been rendered inside a real game against a real OBS. The logic underneath it is thoroughly
tested; the last mile is not.

**The visual match to the reference has not been verified frame-by-frame against a capture.**
[nvidia-reference.md](nvidia-reference.md) says which figures were measured and which were
judged. The judged ones are judged.

**These need a person:**

* The overlay at 1080p, 1440p, 4K and ultrawide, in a real game.
* Every anchor corner, with the slide arriving from the right edge each time.
* A long recording, to confirm the timer stays correct across a pause and an hour boundary.
* Behaviour when OBS is closed mid-recording, and when the game alt-tabs during a slide.
* Multiple games attached at once.
* A font dropped into the config folder, and the fallback when none is present.

## Running them

```bash
cmake -S . -B build && cmake --build build -j && ctest --test-dir build --output-on-failure
python3 -m unittest discover -s obs-script/tests -v
cd tools/obsn-cli && npm install && npm run typecheck && npm test
scripts/cross-language-check.sh
scripts/check-windows-sources.sh          # needs mingw-w64 and scripts/fetch-deps.sh
```

Looking at it without OBS or a game:

```bash
cd tools/obsn-cli && npx tsx src/cli.ts mock --scenario session
```

`--scenario burst` shows the stack under pressure and the restack animation; `--scenario awkward`
shows long file names, wrapped warnings and repeated events.
