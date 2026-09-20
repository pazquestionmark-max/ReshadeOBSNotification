// SPDX-License-Identifier: MIT
// A second, independent implementation of the wire format.
//
// This exists to be checked against the C++ one rather than to share code with it: a format
// that two implementations agree on is a format that is actually specified, and the parity
// tests are what turn docs/protocol.md from a description into a contract.

export const PROTOCOL_VERSION = 1;
export const MAX_MESSAGE_BYTES = 65536;

export type MessageType =
  | "hello"
  | "state_snapshot"
  | "event"
  | "heartbeat"
  | "error"
  | "client_hello"
  | "request_snapshot"
  | "ping"
  | "pong";

/** Types that may legitimately travel producer to consumer. */
export const PRODUCER_TO_CONSUMER: ReadonlySet<MessageType> = new Set<MessageType>([
  "hello",
  "state_snapshot",
  "event",
  "heartbeat",
  "error",
  "pong",
]);

export type EventKind =
  | "recording.starting"
  | "recording.started"
  | "recording.stopping"
  | "recording.stopped"
  | "recording.paused"
  | "recording.resumed"
  | "recording.saved"
  | "replay.starting"
  | "replay.started"
  | "replay.stopped"
  | "replay.saved"
  | "stream.starting"
  | "stream.started"
  | "stream.stopping"
  | "stream.stopped"
  | "stream.reconnecting"
  | "stream.reconnected"
  | "virtualcam.started"
  | "virtualcam.stopped"
  | "scene.changed"
  | "profile.changed"
  | "warning"
  | "script.connected"
  | "script.disconnected";

export const EVENT_KINDS: readonly EventKind[] = [
  "recording.starting", "recording.started", "recording.stopping", "recording.stopped",
  "recording.paused", "recording.resumed", "recording.saved",
  "replay.starting", "replay.started", "replay.stopped", "replay.saved",
  "stream.starting", "stream.started", "stream.stopping", "stream.stopped",
  "stream.reconnecting", "stream.reconnected",
  "virtualcam.started", "virtualcam.stopped",
  "scene.changed", "profile.changed", "warning",
  "script.connected", "script.disconnected",
];

export type OutputState =
  | "idle" | "starting" | "active" | "paused" | "stopping" | "reconnecting";

export interface ObsEvent {
  kind: EventKind;
  ts: number;
  path?: string;
  /** Milliseconds. Absent means "not known" -- never 0, which would be a measurement. */
  duration_ms?: number;
  size_bytes?: number;
  scene?: string;
  previous_scene?: string;
  profile?: string;
  service?: string;
  reason?: string;
  detail?: string;
  attempt?: number;
  replay_seconds?: number;
}

export interface ObsState {
  obs_version?: string;
  script_version?: string;
  profile?: string;
  scene_collection?: string;
  current_scene?: string;
  recording?: { state: OutputState; started_ms?: number; paused_ms?: number;
                paused_since_ms?: number; path?: string };
  replay?: { state: OutputState; started_ms?: number; last_saved_path?: string;
             last_saved_ms?: number; duration_s?: number };
  stream?: { state: OutputState; started_ms?: number; service?: string;
             reconnect_attempt?: number };
  virtual_cam?: { state: OutputState; started_ms?: number };
  stats?: Record<string, number>;
}

export interface Envelope {
  v: number;
  seq: number;
  ts: number;
  type: MessageType;
  data?: unknown;
}

export type DecodeStatus =
  | "ok" | "not_json" | "not_object" | "missing_field" | "bad_version"
  | "unknown_type" | "too_large";

export interface DecodeResult {
  status: DecodeStatus;
  envelope?: Envelope;
  error?: string;
}

/** Whether a decode failure warrants dropping the connection, as opposed to one message. */
export function isFatal(status: DecodeStatus): boolean {
  return status === "not_json" || status === "too_large" || status === "not_object";
}

export function encode(type: MessageType, seq: number, ts: number, data?: unknown): string {
  const envelope: Envelope = { v: PROTOCOL_VERSION, seq, ts, type };
  if (data !== undefined) envelope.data = data;
  return `${JSON.stringify(envelope)}\n`;
}

export function decode(line: string): DecodeResult {
  if (Buffer.byteLength(line, "utf8") > MAX_MESSAGE_BYTES) {
    return { status: "too_large", error: "message exceeds the size limit" };
  }
  let parsed: unknown;
  try {
    parsed = JSON.parse(line);
  } catch (error) {
    return { status: "not_json", error: String(error) };
  }
  if (typeof parsed !== "object" || parsed === null || Array.isArray(parsed)) {
    return { status: "not_object", error: "top-level value is not an object" };
  }
  const doc = parsed as Record<string, unknown>;
  if (typeof doc["type"] !== "string") {
    return { status: "missing_field", error: "missing 'type'" };
  }
  // The version is checked before the type, because a peer speaking a protocol we do not know
  // may well use type names that mean something else entirely.
  const version = typeof doc["v"] === "number" ? doc["v"] : PROTOCOL_VERSION;
  if (version !== PROTOCOL_VERSION) {
    return { status: "bad_version", error: `unsupported protocol version ${version}` };
  }
  const type = doc["type"] as MessageType;
  if (!isMessageType(type)) {
    return { status: "unknown_type", error: "unrecognised message type" };
  }
  return {
    status: "ok",
    envelope: {
      v: version,
      seq: typeof doc["seq"] === "number" ? doc["seq"] : 0,
      ts: typeof doc["ts"] === "number" ? doc["ts"] : 0,
      type,
      data: doc["data"],
    },
  };
}

const MESSAGE_TYPES: ReadonlySet<string> = new Set([
  "hello", "state_snapshot", "event", "heartbeat", "error",
  "client_hello", "request_snapshot", "ping", "pong",
]);

export function isMessageType(value: string): value is MessageType {
  return MESSAGE_TYPES.has(value);
}

/**
 * Splits a byte stream into lines with a hard cap enforced *during* accumulation.
 *
 * The cap matters: a peer that sends a megabyte without a newline must not be able to grow the
 * buffer to a megabyte before anyone notices.
 */
export class LineFramer {
  #pending = "";
  #overflowed = false;

  constructor(private readonly maxBytes = MAX_MESSAGE_BYTES) {}

  get overflowed(): boolean {
    return this.#overflowed;
  }

  get pendingBytes(): number {
    return Buffer.byteLength(this.#pending, "utf8");
  }

  reset(): void {
    this.#pending = "";
    this.#overflowed = false;
  }

  /** Returns the complete lines, or null once the framer has overflowed. */
  feed(chunk: string): string[] | null {
    if (this.#overflowed) return null;
    this.#pending += chunk;
    const lines: string[] = [];
    let index = this.#pending.indexOf("\n");
    while (index >= 0) {
      lines.push(this.#pending.slice(0, index));
      this.#pending = this.#pending.slice(index + 1);
      index = this.#pending.indexOf("\n");
    }
    if (this.pendingBytes > this.maxBytes) {
      this.#overflowed = true;
      this.#pending = "";
      // Lines already completed are still returned: they were whole and are not the problem.
      return lines.length > 0 ? lines : null;
    }
    return lines;
  }
}
