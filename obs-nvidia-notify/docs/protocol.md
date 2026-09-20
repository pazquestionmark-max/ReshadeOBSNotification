# The wire protocol

One line of JSON per message, newline-terminated, over a local named pipe (Windows) or an
AF_UNIX socket (Linux and macOS). Nothing here touches the network.

Three implementations speak it: the OBS script (`obs-script/obs_nvidia_notify.py`), the overlay
(`shared/src/protocol.cpp`) and the tooling (`tools/obsn-cli/src/protocol.ts`). The third exists
so CI can check the first two against a description written independently of both — a format two
implementations agree on is a format that is actually specified.

## Direction

The **producer** is the OBS script. It listens, and every attached game's overlay is a
**consumer** that connects to it. OBS runs for a whole session and games come and go, so the
long-lived side listens and the transient side carries the reconnect logic.

Several games can attach at once. Each gets its own pipe instance and its own bounded queue.

## Envelope

```json
{"v":1,"seq":42,"ts":1789946781250,"type":"event","data":{ }}
```

| Field | Meaning |
|---|---|
| `v` | Protocol version. Currently 1. A message with any other value is refused **before** its type is read, because a peer speaking a protocol we do not know may use type names that mean something else. |
| `seq` | The sender's monotonic counter. Diagnostic; nothing depends on it. |
| `ts` | Wall-clock milliseconds at the sender. |
| `type` | One of the message types below. |
| `data` | An object, or absent. |

A message may not exceed **65536 bytes**, including the newline. Both ends enforce this while
accumulating, not after: a peer that sends a megabyte without a newline must not be able to grow
the other end's buffer to a megabyte before anyone notices.

## Message types

### Producer to consumer

| Type | Carries |
|---|---|
| `hello` | `protocol_min`, `protocol_max`, `script_version`, `obs_version`, `platform`, `capabilities[]` |
| `state_snapshot` | The whole `ObsState` — what is true right now |
| `event` | One `ObsEvent` — something that happened at an instant |
| `heartbeat` | Nothing. Its arrival is the message. |
| `error` | `code`, `message` |
| `pong` | Nothing |

### Consumer to producer

| Type | Carries |
|---|---|
| `client_hello` | `protocol`, `client`, `client_version`, `process`, `pid` |
| `request_snapshot` | Nothing |
| `ping` | Nothing |

A consumer that receives a consumer-only type is talking to something that is not an OBS script,
and says so rather than trying to make sense of it.

## Handshake

```
consumer                             producer
   |  --- client_hello ------------->  |
   |  <------------------- hello ----  |
   |  --- request_snapshot --------->  |
   |  <------------ state_snapshot --  |
   |  <------------------- event ----  |   (from here on, as things happen)
   |  <--------------- heartbeat ----  |   (every 2s regardless)
```

The snapshot is *requested* rather than assumed. A producer that sends one unprompted is fine;
one that waits to be asked is also fine.

## State and events

The two shapes are kept apart on purpose.

**`ObsState` is a level** — what is true now. It is what the status indicator draws, and what a
freshly-attached overlay is handed so it does not have to infer the world from a history it
missed.

**`ObsEvent` is an edge** — something that happened. Only edges become notifications, which is
why "recording is active" never raises a toast but "recording started" does.

### Event fields

```json
{"kind":"recording.saved","ts":1789946781250,"path":"2026-09-20 21-14-03.mkv",
 "duration_ms":727000,"size_bytes":1503238553}
```

| Field | Notes |
|---|---|
| `kind` | See below. Required. |
| `ts` | Milliseconds. |
| `path` | The file the event concerns. **The file name only, unless the user turned on "Send the full path" in the script** — a full path contains their account name. |
| `duration_ms` | **Absent means "not known".** Never 0, which would be a measurement. |
| `size_bytes` | Same rule. |
| `scene`, `previous_scene`, `profile`, `service` | Free text, clamped to 512 characters. |
| `reason` | `user`, `error`, `out_of_space`, `encoder_error`, `obs_exiting`, `unknown` |
| `detail` | Free text for a warning. Displayed, never interpreted. |
| `attempt` | Reconnect attempt number. |
| `replay_seconds` | Length of a saved replay clip. |
| `state` | A whole `ObsState`, when the event carries a resynchronisation. |

### Event kinds

```
recording.starting  recording.started  recording.stopping  recording.stopped
recording.paused    recording.resumed  recording.saved
replay.starting     replay.started     replay.stopped      replay.saved
stream.starting     stream.started     stream.stopping     stream.stopped
stream.reconnecting stream.reconnected
virtualcam.started  virtualcam.stopped
scene.changed       profile.changed    warning
script.connected    script.disconnected
```

The names are OBS's own frontend event names, lower-cased and dotted. That is deliberate: a pipe
trace should be readable by someone who knows OBS and has never read this page.

`recording.stopped` and `recording.saved` are separate events, and OBS stopping a recording
produces both. They are separate because an automatic file split writes a file without the
recording ending, and because a user who only wants to be told where the file went should be
able to ask for just that.

The `.starting` and `.stopping` kinds never raise a notification. They exist so the status
indicator can show a transition; as toasts they would be noise immediately followed by the real
event.

## Flow control

Every consumer has a bounded queue in the producer. When it overflows:

1. the **oldest** message is dropped — the newest is the one describing the world as it is now;
2. the client is flagged, and a fresh `state_snapshot` is sent on the next event.

So what a stalled game loses is repaired rather than left as a gap it cannot know about. A game
that has stopped reading — minimised, hung, compiling shaders — costs the script a few dropped
events and nothing else. OBS's UI thread never waits on it.

## Staleness

If nothing arrives for `integration.stale_after_ms` (8 seconds by default) the overlay marks the
state stale. That is **not** a disconnect: the pipe is open and the producer may simply be busy.
The overlay stops presenting the state as live — a frozen timer presented as a running one is
worse than no timer — and the Diagnostics tab says so.

## Security

* **Windows.** The pipe is created with an explicit DACL granting only the creating user and
  SYSTEM, with inheritance blocked, and `PIPE_REJECT_REMOTE_CLIENTS` set. If the descriptor
  cannot be built the pipe falls back to the default DACL, which Windows still restricts to the
  creating logon session, and the script log says which is in force.
* **POSIX.** The socket file is `chmod 0600`.
* **Neither end opens a network socket.** Not to localhost, not to anything.
* The pipe name includes the user's SID by default, so two accounts on one machine cannot
  collide.
