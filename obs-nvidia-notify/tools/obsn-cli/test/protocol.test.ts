// SPDX-License-Identifier: MIT
// The wire format, checked here and -- where the C++ tool is built -- against the C++ one.

import assert from "node:assert/strict";
import { test } from "node:test";

import {
  EVENT_KINDS, LineFramer, MAX_MESSAGE_BYTES, PRODUCER_TO_CONSUMER,
  decode, encode, isFatal, isMessageType,
} from "../src/protocol.ts";

test("an envelope round-trips", () => {
  const line = encode("event", 7, 1_700_000_000_000, { kind: "recording.started", ts: 1 });
  const result = decode(line.trim());
  assert.equal(result.status, "ok");
  assert.equal(result.envelope?.type, "event");
  assert.equal(result.envelope?.seq, 7);
  assert.deepEqual(result.envelope?.data, { kind: "recording.started", ts: 1 });
});

test("every message ends with exactly one newline", () => {
  const line = encode("heartbeat", 1, 0);
  assert.ok(line.endsWith("\n"));
  assert.equal(line.indexOf("\n"), line.length - 1);
});

test("rubbish is rejected without throwing", () => {
  assert.equal(decode("").status, "not_json");
  assert.equal(decode("not json").status, "not_json");
  assert.equal(decode("[1,2,3]").status, "not_object");
  assert.equal(decode("null").status, "not_object");
  assert.equal(decode("{}").status, "missing_field");
});

test("the version is checked before the type", () => {
  // A peer speaking a protocol we do not know may use type names that mean something else.
  assert.equal(decode(JSON.stringify({ v: 99, type: "event" })).status, "bad_version");
});

test("an unknown type is droppable, malformed input is fatal", () => {
  assert.equal(decode(JSON.stringify({ v: 1, type: "something_new" })).status, "unknown_type");
  assert.ok(!isFatal("unknown_type"));
  assert.ok(isFatal("not_json"));
  assert.ok(isFatal("too_large"));
  assert.ok(isFatal("not_object"));
});

test("an over-long message is refused before it is parsed", () => {
  assert.equal(decode("x".repeat(MAX_MESSAGE_BYTES + 1)).status, "too_large");
});

test("direction is enforced", () => {
  for (const type of ["hello", "state_snapshot", "event", "heartbeat", "error", "pong"] as const) {
    assert.ok(PRODUCER_TO_CONSUMER.has(type), type);
  }
  for (const type of ["client_hello", "ping", "request_snapshot"] as const) {
    assert.ok(!PRODUCER_TO_CONSUMER.has(type), type);
  }
});

test("the message type set is closed", () => {
  assert.ok(isMessageType("event"));
  assert.ok(!isMessageType("events"));
  assert.ok(!isMessageType(""));
});

test("the framer yields complete lines and holds partial ones", () => {
  const framer = new LineFramer();
  assert.deepEqual(framer.feed("one\ntwo\n"), ["one", "two"]);
  assert.deepEqual(framer.feed("par"), []);
  assert.deepEqual(framer.feed("tial\n"), ["partial"]);
});

test("the framer refuses an over-long line during accumulation", () => {
  // The point of the cap is that a peer sending a megabyte with no newline cannot grow the
  // buffer to a megabyte before anyone notices.
  const framer = new LineFramer(64);
  assert.equal(framer.feed("x".repeat(200)), null);
  assert.ok(framer.overflowed);
  assert.equal(framer.feed("short\n"), null);
  framer.reset();
  assert.deepEqual(framer.feed("short\n"), ["short"]);
});

test("lines already completed survive a later overflow", () => {
  const framer = new LineFramer(32);
  assert.deepEqual(framer.feed(`good\n${"x".repeat(100)}`), ["good"]);
  assert.ok(framer.overflowed);
});

test("event kinds are unique, lower-case, and named after what they are", () => {
  // Most are "<output>.<what happened>". A few -- "warning" -- belong to no output and are
  // deliberately bare rather than filed under an invented one.
  const seen = new Set<string>();
  for (const kind of EVENT_KINDS) {
    assert.match(kind, /^[a-z_]+(\.[a-z_]+)?$/, kind);
    assert.ok(!seen.has(kind), `duplicate ${kind}`);
    seen.add(kind);
  }
  assert.ok(EVENT_KINDS.includes("warning"));
  assert.ok(EVENT_KINDS.includes("recording.saved"));
});
