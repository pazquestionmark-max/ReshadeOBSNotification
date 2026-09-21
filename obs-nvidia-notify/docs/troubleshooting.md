# Troubleshooting

## Start here: prove the pipe works

```
python obs_nvidia_notify.py --selftest
```

Run that with the same Python OBS uses. It stands up the server, connects to it, completes the
handshake and pushes an event, with no OBS and no game involved. It takes about a second and it
separates the two halves of almost every problem on this page:

* **PASS** -- the pipe is fine, so the problem is on the overlay side. Skip to
  [Toasts appear but ...](#toasts-appear-but-the-text-is-clipped-or-misaligned) or check
  ReShade's menu under OBS Notifications, Diagnostics.
* **could not connect** -- the pipe exists but cannot be opened. On Windows that is the security
  descriptor; the line above it in the output says which one is in force.
* **the server could not push an event to an idle client** -- the symptom of a synchronous pipe
  handle. It is what this check exists for, and it is fixed in 1.0.1; if you see it, you are on
  an older script.

---

Symptom first. The two places that will tell you what is wrong are OBS's **script log**
(Tools → Scripts, select the script) and the add-on's **Diagnostics** tab in ReShade's menu.

## Nothing appears in game at all

**Is the add-on loaded?** ReShade's menu → **Add-ons**. If **OBS Notifications** is not listed:

* You may have the **addon-free** ReShade build. Add-ons cannot load there. Reinstall ReShade
  with full add-on support.
* The file may be in the wrong place. It goes next to the game's ReShade DLL (`dxgi.dll`,
  `d3d11.dll`, …), not in a subfolder.
* 32-bit game, 64-bit add-on, or the reverse. Use `.addon` for 32-bit and `.addon64` for 64-bit.

**Is the link up?** Diagnostics → **State**.

| State | Means |
|---|---|
| `connected` | The overlay is talking to the script. The problem is elsewhere on this page. |
| `waiting to retry` | The script is not listening. See the next section. |
| `handshaking` | Connected but the script has not said hello. Usually a version mismatch; the log will say. |
| `failed` | Something specific went wrong; **Detail** says what. |

**Is anything enabled?** Both ends have switches. The script's **Send these events** group, and
each category's enable switch in the Notifications tab.

## "could not listen" in the OBS script log

* **Another copy is already running.** The script refuses to become a second server on the same
  name, because a second server is one nothing would ever reach. Remove the duplicate script
  entry, or give one of them a different pipe name.
* **A different pipe name at each end.** Both default to empty, which means
  `obsn.v1.<your SID>`. If you set one, set the other to match: the script's **Pipe name** field
  and the add-on's Link tab must agree.
* **A permissions error building the security descriptor.** The log says so and falls back to
  the default DACL, which still restricts the pipe to your logon session. This is not usually
  what is stopping you.

## The overlay says `connected` but no toasts appear

* **The Preview tab is open.** While it is, the overlay draws sample state instead of the live
  one. Switch to any other tab.
* **The category is off** at one end or the other.
* **Events are arriving during the suppression window.** Just after the link comes up, events
  are absorbed into the initial synchronisation, because state that was already true is not
  news. If you attach mid-recording, you will not be told the recording started ten minutes ago.
  That is the intended behaviour; `suppress_after_connect_ms` controls the window.
* **They are off screen.** A percentage placement with a large offset can put them outside the
  viewport. Notifications → Placement, or load `examples/profiles/default.json`.

## Toasts appear but the text is clipped or misaligned

Diagnostics → **Text width** says how the overlay is measuring text.

| Value | Means |
|---|---|
| the overlay's own font | Correct. Widths come from the table the glyphs are drawn from. |
| `ImGui::CalcTextSize` / `ImFont::CalcTextSizeA` | Fine. ReShade's font is answering. |
| **estimated** | ReShade's ImGui would not answer, so widths are a codepoint estimate. Text stays on screen and readable, but edges will be a few pixels out. Load a font (Appearance → Font) and this goes away. |

## The recording timer is frozen

Diagnostics will show a stale warning. Nothing has arrived from the script for
`stale_after_ms` (8 seconds by default). The pipe is still open — the script may simply be busy,
or OBS may be hung. The overlay deliberately stops presenting the state as live rather than
showing a timer that has silently stopped advancing.

## A file name shows but the full path does not

That is the default. A full path contains your account name, so only the file name crosses the
pipe. Turn on **Send the full path, not just the file name** in the script if you want `{path}`
and `{folder}` to work.

## The file size is wrong, or missing

OBS reports the path the moment it closes the file, and a muxer can still be flushing. The
script waits for two identical size readings before sending one. If it is still missing, the
file is somewhere the script cannot stat — a network drive that has gone away, usually. The
`{size}` placeholder then disappears rather than showing `0 B`.

## Too many notifications at once

* **Most on screen at once** (`max_visible`) bounds the stack. When more arrive, the lowest
  priority is dropped.
* **Minimum gap per category** (`min_interval_ms`) makes two events from one category inside
  that window replace each other rather than stack.
* Turn off the categories you do not want, at whichever end suits you. Turning them off in the
  **script** means they never cross the pipe at all.

## Settings do not survive a restart

Press **Save** in the Profiles tab. The add-on does not save on every keystroke, so a change you
liked is not lost to one you did not.

If saving fails, the Profiles tab shows why. The usual cause is that
`%APPDATA%\OBSNotifyOverlay\` could not be created.

## Nothing appeared, and the script log says `write failed ... error 232`

Error 232 is ERROR_NO_DATA, "the pipe is being closed". Before 1.0.1 this was the *end* of a
much earlier problem rather than the problem itself: the server used a synchronous pipe handle,
which serialises its operations, so while its read was outstanding -- essentially always, since
the overlay only speaks every ten seconds -- every message it tried to send sat in a blocked
`WriteFile`. The write finally returned, with error 232, when the game exited and the pipe
started closing. The connection looked healthy the whole time and nothing was ever delivered.

Update the script to 1.0.1 or later and run `--selftest`. If the self-test passes and you still
see this, it is now the ordinary case it is named for: the game closed.

## Checking without a game

```bash
python obs_nvidia_notify.py --selftest                # prove the pipe, end to end
obsn-config probe 10                                  # attach as the overlay does
cd tools/obsn-cli && npx tsx src/cli.ts mock          # pretend to be OBS
```

The first tells you whether the script is sending anything. The second tells you whether the
add-on is drawing anything. Between them they separate the two halves of almost every problem
on this page.

## Reporting a problem

Diagnostics shows **Build** — please include it. The log is at
`%APPDATA%\OBSNotifyOverlay\obsn-overlay.log` and contains no file paths unless you turned on
**Include file paths in the log**.
