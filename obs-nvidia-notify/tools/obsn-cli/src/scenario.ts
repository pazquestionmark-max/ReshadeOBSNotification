// SPDX-License-Identifier: MIT
// Scripted capture sessions for the mock producer.
//
// These are the sequences worth looking at with your own eyes before shipping a change to the
// renderer: a plain recording, a stack under pressure, and the awkward cases that only happen
// when something is going wrong.

import type { ObsEvent, EventKind } from "./protocol.ts";

export interface Step {
  /** Milliseconds to wait before sending this one. */
  after: number;
  kind: EventKind;
  fields?: Omit<Partial<ObsEvent>, "kind" | "ts">;
}

export interface Scenario {
  name: string;
  description: string;
  steps: Step[];
  /** Run the whole list again from the top when it finishes. */
  loop: boolean;
}

const CLIP = "C:\\Users\\You\\Videos\\2026-09-20 21-14-03.mkv";
const REPLAY = "C:\\Users\\You\\Videos\\Replay 2026-09-20 21-16-42.mkv";

export const SCENARIOS: readonly Scenario[] = [
  {
    name: "session",
    description: "An ordinary recording session, start to finish.",
    loop: true,
    steps: [
      { after: 1500, kind: "replay.started", fields: { replay_seconds: 30 } },
      { after: 2500, kind: "recording.started" },
      { after: 4000, kind: "recording.paused" },
      { after: 2500, kind: "recording.resumed" },
      { after: 3500, kind: "replay.saved",
        fields: { path: REPLAY, replay_seconds: 30, duration_ms: 30_000 } },
      { after: 4000, kind: "recording.stopped", fields: { duration_ms: 727_000 } },
      { after: 400, kind: "recording.saved",
        fields: { path: CLIP, size_bytes: 1_503_238_553, duration_ms: 727_000 } },
      { after: 3000, kind: "replay.stopped" },
      { after: 4000, kind: "script.connected" },
    ],
  },
  {
    name: "replay",
    description: "The replay buffer being armed and clipped repeatedly -- the common case.",
    loop: true,
    steps: [
      { after: 1000, kind: "replay.started", fields: { replay_seconds: 30 } },
      { after: 2500, kind: "replay.saved", fields: { path: REPLAY, replay_seconds: 30 } },
      { after: 3000, kind: "replay.saved",
        fields: { path: "C:\\Users\\You\\Videos\\Replay 2026-09-20 21-17-58.mkv",
                  replay_seconds: 30 } },
      { after: 3000, kind: "replay.stopped" },
    ],
  },
  {
    name: "stream",
    description: "A stream that starts, degrades, recovers and ends.",
    loop: true,
    steps: [
      { after: 1000, kind: "stream.started", fields: { service: "Twitch" } },
      { after: 3000, kind: "warning",
        fields: { detail: "Dropping frames: 7.4% lost to the network" } },
      { after: 2500, kind: "stream.reconnecting", fields: { attempt: 1 } },
      { after: 2000, kind: "stream.reconnecting", fields: { attempt: 2 } },
      { after: 2000, kind: "stream.reconnected" },
      { after: 4000, kind: "stream.stopped", fields: { duration_ms: 2_820_000 } },
    ],
  },
  {
    name: "burst",
    description:
      "Everything at once, faster than the stack can hold. Shows the overflow rule, the " +
      "priority ordering and the restack animation under pressure.",
    loop: true,
    steps: [
      { after: 400, kind: "recording.started" },
      { after: 200, kind: "replay.started", fields: { replay_seconds: 30 } },
      { after: 200, kind: "stream.started", fields: { service: "YouTube" } },
      { after: 200, kind: "virtualcam.started" },
      { after: 200, kind: "replay.saved", fields: { path: REPLAY } },
      { after: 200, kind: "warning", fields: { detail: "Encoder overloaded" } },
      { after: 200, kind: "recording.saved", fields: { path: CLIP, size_bytes: 24_117_248 } },
      { after: 3000, kind: "recording.stopped", fields: { duration_ms: 4_000 } },
      { after: 200, kind: "stream.stopped", fields: { duration_ms: 3_800 } },
      { after: 200, kind: "replay.stopped" },
      { after: 200, kind: "virtualcam.stopped" },
    ],
  },
  {
    name: "awkward",
    description:
      "The cases that break layouts: very long file names, wrapped warnings, and the same " +
      "event twice in a row.",
    loop: true,
    steps: [
      { after: 1000, kind: "recording.saved", fields: {
        path: "D:\\Captures\\2026\\September\\A Very Long Session Name That Goes On " +
              "And On And Does Not Stop Until It Has Run Out Of Room.mkv",
        size_bytes: 8_589_934_592 } },
      { after: 3000, kind: "warning", fields: {
        detail: "Output stopped: the disk the recording folder is on has no space left, and " +
                "OBS could not finish writing the file" } },
      { after: 3000, kind: "replay.saved", fields: { path: REPLAY } },
      { after: 500, kind: "replay.saved", fields: { path: REPLAY } },
      { after: 500, kind: "replay.saved", fields: { path: REPLAY } },
      { after: 3000, kind: "scene.changed",
        fields: { scene: "Gameplay", previous_scene: "Starting soon" } },
    ],
  },
];

export function findScenario(name: string): Scenario | undefined {
  return SCENARIOS.find((scenario) => scenario.name === name);
}
