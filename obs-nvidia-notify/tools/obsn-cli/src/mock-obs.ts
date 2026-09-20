// SPDX-License-Identifier: MIT
// A stand-in for the OBS script, so the overlay can be looked at without OBS running.
//
// Node's net module listens on a Windows named pipe and an AF_UNIX socket through the same
// API, so this is the same code on both platforms and reaches the real add-on on Windows.

import net from "node:net";
import os from "node:os";
import path from "node:path";
import fs from "node:fs";

import {
  LineFramer, PROTOCOL_VERSION, decode, encode,
  type MessageType, type ObsEvent, type ObsState,
} from "./protocol.ts";
import type { Scenario } from "./scenario.ts";

export function defaultEndpoint(name = ""): string {
  if (process.platform === "win32") {
    if (name.startsWith("\\\\")) return name;
    return `\\\\.\\pipe\\${name || "obsn.v1"}`;
  }
  if (name.startsWith("/")) return name;
  const dir = process.env["XDG_RUNTIME_DIR"] ?? process.env["TMPDIR"] ?? os.tmpdir();
  return path.join(dir, `${name || "obsn.v1"}.sock`);
}

interface Client {
  socket: net.Socket;
  framer: LineFramer;
  label: string;
}

export interface MockOptions {
  endpoint: string;
  scenario: Scenario;
  /** Called for each line of activity, so the CLI decides how to present it. */
  onLog?: (message: string) => void;
}

export class MockObs {
  #server: net.Server | undefined;
  #clients = new Set<Client>();
  #seq = 0;
  #timer: NodeJS.Timeout | undefined;
  #heartbeat: NodeJS.Timeout | undefined;
  #step = 0;
  #state: ObsState = emptyState();

  constructor(private readonly options: MockOptions) {}

  #log(message: string): void {
    this.options.onLog?.(message);
  }

  async start(): Promise<void> {
    // A stale socket file from a crashed run would otherwise make listen() fail with EADDRINUSE
    // for a server that is not running.
    if (process.platform !== "win32") {
      try {
        fs.unlinkSync(this.options.endpoint);
      } catch {
        // Nothing there, which is the normal case.
      }
    }

    this.#server = net.createServer((socket) => this.#accept(socket));
    await new Promise<void>((resolve, reject) => {
      this.#server!.once("error", reject);
      this.#server!.listen(this.options.endpoint, () => resolve());
    });
    this.#log(`listening on ${this.options.endpoint}`);

    this.#heartbeat = setInterval(() => this.#broadcast("heartbeat"), 2000);
    this.#schedule();
  }

  async stop(): Promise<void> {
    if (this.#timer) clearTimeout(this.#timer);
    if (this.#heartbeat) clearInterval(this.#heartbeat);
    for (const client of this.#clients) client.socket.destroy();
    this.#clients.clear();
    await new Promise<void>((resolve) => {
      if (!this.#server) return resolve();
      this.#server.close(() => resolve());
    });
    this.#server = undefined;
  }

  get clientCount(): number {
    return this.#clients.size;
  }

  #accept(socket: net.Socket): void {
    const client: Client = { socket, framer: new LineFramer(), label: "overlay" };
    this.#clients.add(client);
    socket.setNoDelay(true);

    socket.on("data", (chunk) => {
      const lines = client.framer.feed(chunk.toString("utf8"));
      if (lines === null) {
        this.#log(`${client.label} sent an over-long line; dropping it`);
        socket.destroy();
        return;
      }
      for (const line of lines) {
        if (line.trim().length === 0) continue;
        this.#handle(client, line);
      }
    });
    socket.on("error", () => socket.destroy());
    socket.on("close", () => {
      this.#clients.delete(client);
      this.#log(`${client.label} disconnected`);
    });
  }

  #handle(client: Client, line: string): void {
    const result = decode(line);
    if (result.status !== "ok" || !result.envelope) {
      this.#log(`undecodable message from ${client.label}: ${result.error ?? result.status}`);
      return;
    }
    switch (result.envelope.type) {
      case "client_hello": {
        const data = (result.envelope.data ?? {}) as Record<string, unknown>;
        const process_ = typeof data["process"] === "string" ? data["process"] : "overlay";
        client.label = process_;
        this.#log(`${client.label} attached`);
        this.#send(client, "hello", {
          protocol_min: PROTOCOL_VERSION,
          protocol_max: PROTOCOL_VERSION,
          script_version: "mock",
          obs_version: "30.2.3 (mock)",
          platform: process.platform,
          capabilities: ["recording", "replay_buffer", "streaming", "virtualcam", "warnings"],
        });
        this.#send(client, "state_snapshot", this.#state);
        break;
      }
      case "request_snapshot":
        this.#send(client, "state_snapshot", this.#state);
        break;
      case "ping":
        this.#send(client, "pong");
        break;
      default:
        this.#log(`${client.label} sent an unexpected ${result.envelope.type}`);
        break;
    }
  }

  #send(client: Client, type: MessageType, data?: unknown): void {
    if (client.socket.destroyed) return;
    client.socket.write(encode(type, ++this.#seq, Date.now(), data));
  }

  #broadcast(type: MessageType, data?: unknown): void {
    for (const client of this.#clients) this.#send(client, type, data);
  }

  #schedule(): void {
    const steps = this.options.scenario.steps;
    if (steps.length === 0) return;
    const step = steps[this.#step % steps.length];
    if (!step) return;

    this.#timer = setTimeout(() => {
      const event: ObsEvent = { kind: step.kind, ts: Date.now(), ...(step.fields ?? {}) };
      this.#apply(event);
      this.#broadcast("event", event);
      this.#log(`-> ${event.kind}`);

      this.#step += 1;
      if (this.#step >= steps.length && !this.options.scenario.loop) return;
      this.#schedule();
    }, step.after);
  }

  /** Keeps the snapshot consistent with the events sent, so a late attacher sees the truth. */
  #apply(event: ObsEvent): void {
    const state = this.#state;
    switch (event.kind) {
      case "recording.started":
        state.recording = { state: "active", started_ms: Date.now() };
        break;
      case "recording.paused":
        if (state.recording) {
          state.recording.state = "paused";
          state.recording.paused_since_ms = Date.now();
        }
        break;
      case "recording.resumed":
        if (state.recording) {
          state.recording.state = "active";
          state.recording.paused_ms =
            (state.recording.paused_ms ?? 0) +
            Math.max(0, Date.now() - (state.recording.paused_since_ms ?? Date.now()));
          state.recording.paused_since_ms = 0;
        }
        break;
      case "recording.stopped":
        state.recording = { state: "idle" };
        break;
      case "replay.started":
        state.replay = { state: "active", started_ms: Date.now(),
                         duration_s: event.replay_seconds ?? 30 };
        break;
      case "replay.stopped":
        state.replay = { state: "idle" };
        break;
      case "replay.saved":
        if (state.replay) {
          state.replay.last_saved_path = event.path ?? "";
          state.replay.last_saved_ms = Date.now();
        }
        break;
      case "stream.started":
        state.stream = { state: "active", started_ms: Date.now(),
                         service: event.service ?? "" };
        break;
      case "stream.stopped":
        state.stream = { state: "idle" };
        break;
      case "virtualcam.started":
        state.virtual_cam = { state: "active", started_ms: Date.now() };
        break;
      case "virtualcam.stopped":
        state.virtual_cam = { state: "idle" };
        break;
      case "scene.changed":
        state.current_scene = event.scene ?? "";
        break;
      default:
        break;
    }
  }
}

function emptyState(): ObsState {
  return {
    obs_version: "30.2.3 (mock)",
    script_version: "mock",
    profile: "Untitled",
    scene_collection: "Untitled",
    current_scene: "Gameplay",
    recording: { state: "idle" },
    replay: { state: "idle" },
    stream: { state: "idle" },
    virtual_cam: { state: "idle" },
    stats: {},
  };
}
