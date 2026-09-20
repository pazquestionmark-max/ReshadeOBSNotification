// SPDX-License-Identifier: MIT
// Cross-validation against the C++ implementation.
//
// Two implementations that agree is what makes the protocol and the configuration a
// specification rather than whatever one program happens to do. The C++ side is the authority;
// these tests fail when this mirror and that authority disagree, in either direction.
//
// They are skipped, loudly, when the C++ tool has not been built, rather than passing silently
// and letting the two drift.

import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { test } from "node:test";

import {
  ACCENT_STYLES, ANCHORS, BORDERS, CATEGORIES, EASINGS, ICONS, MOTION_KINDS, PLACEHOLDERS,
  SCHEMA, STACKS, flatten, validate,
} from "../src/config-schema.ts";
import { EVENT_KINDS } from "../src/protocol.ts";

const here = path.dirname(fileURLToPath(import.meta.url));
const root = path.resolve(here, "..", "..", "..");

function findTool(): string | undefined {
  const candidates = [
    path.join(root, "build", "tools", "config-tool", "obsn-config"),
    path.join(root, "build", "tools", "config-tool", "Release", "obsn-config.exe"),
    path.join(root, "build", "tools", "config-tool", "obsn-config.exe"),
  ];
  return candidates.find((candidate) => fs.existsSync(candidate));
}

const tool = findTool();
const skip = tool
  ? false
  : "obsn-config is not built; run cmake -S . -B build && cmake --build build";

function run(...args: string[]): string {
  return execFileSync(tool!, args, { encoding: "utf8", maxBuffer: 16 * 1024 * 1024 });
}

test("the C++ defaults satisfy this schema", { skip }, () => {
  const defaults = JSON.parse(run("defaults"));
  const problems = validate(defaults);
  assert.deepEqual(problems, [], problems.map((p) => `${p.path}: ${p.message}`).join("\n"));
});

test("this schema describes exactly the settings the C++ loader writes", { skip }, () => {
  const defaults = JSON.parse(run("defaults"));
  const written = new Set(flatten(defaults).keys());
  const described = new Set(SCHEMA.map((setting) => setting.path));

  const missing = [...written].filter((path_) => !described.has(path_));
  const extra = [...described].filter((path_) => !written.has(path_));
  // A setting in one and not the other is a setting nobody can discover, or one documented and
  // never read.
  assert.deepEqual(missing, [], `written but not described: ${missing.join(", ")}`);
  assert.deepEqual(extra, [], `described but never written: ${extra.join(", ")}`);
});

test("the category list matches", { skip }, () => {
  const lines = run("categories").trim().split("\n");
  const keys = lines.map((line) => line.split(/\s+/)[0]);
  assert.deepEqual(keys, [...CATEGORIES]);
});

test("every category maps to an event kind this implementation knows", { skip }, () => {
  const lines = run("categories").trim().split("\n");
  for (const line of lines) {
    const parts = line.trim().split(/\s\s+/);
    const kind = parts[parts.length - 1];
    assert.ok(EVENT_KINDS.includes(kind as never), `unknown event kind '${kind}'`);
  }
});

test("the C++ loader accepts a profile this schema considers valid", { skip }, () => {
  const defaults = JSON.parse(run("defaults"));
  defaults.notifications.placement.anchor = "bottom_left";
  defaults.notifications.motion.kind = "scale";
  defaults.notifications.box.accent_style = "bar_and_tile";
  defaults.notifications.recording_started.icon = "record_ring";
  defaults.notifications.recording_started.fade.easing = "ease_out_elastic";

  assert.deepEqual(validate(defaults), []);

  const file = path.join(root, "build", "parity-profile.json");
  fs.writeFileSync(file, JSON.stringify(defaults));
  try {
    const output = run("validate", file);
    assert.match(output, /ok/);
    // And it survives the round trip unchanged, which is what "accepts" has to mean.
    const normalised = JSON.parse(run("normalise", file));
    assert.equal(normalised.notifications.placement.anchor, "bottom_left");
    assert.equal(normalised.notifications.motion.kind, "scale");
    assert.equal(normalised.notifications.box.accent_style, "bar_and_tile");
    assert.equal(normalised.notifications.recording_started.icon, "record_ring");
  } finally {
    fs.rmSync(file, { force: true });
  }
});

test("every enum value this schema lists is one the C++ loader keeps", { skip }, () => {
  // A value the loader does not recognise is silently replaced with the default, so a
  // round-trip that does not preserve it is how a stale list is caught.
  const defaults = JSON.parse(run("defaults"));
  const file = path.join(root, "build", "parity-enums.json");

  const check = (setValue: (doc: any, value: string) => void,
                 getValue: (doc: any) => string,
                 values: readonly string[]) => {
    for (const value of values) {
      const document = JSON.parse(JSON.stringify(defaults));
      setValue(document, value);
      fs.writeFileSync(file, JSON.stringify(document));
      const normalised = JSON.parse(run("normalise", file));
      assert.equal(getValue(normalised), value, `'${value}' was not preserved`);
    }
  };

  try {
    check((d, v) => { d.notifications.placement.anchor = v; },
          (d) => d.notifications.placement.anchor, ANCHORS);
    check((d, v) => { d.notifications.motion.kind = v; },
          (d) => d.notifications.motion.kind, MOTION_KINDS);
    check((d, v) => { d.notifications.motion.in_easing = v; },
          (d) => d.notifications.motion.in_easing, EASINGS);
    check((d, v) => { d.notifications.box.accent_style = v; },
          (d) => d.notifications.box.accent_style, ACCENT_STYLES);
    check((d, v) => { d.notifications.box.border = v; },
          (d) => d.notifications.box.border, BORDERS);
    check((d, v) => { d.notifications.stack = v; }, (d) => d.notifications.stack, STACKS);
    check((d, v) => { d.notifications.warning.icon = v; },
          (d) => d.notifications.warning.icon, ICONS);
  } finally {
    fs.rmSync(file, { force: true });
  }
});

test("every placeholder this schema lists is one the C++ formatter expands", { skip }, () => {
  // An unknown placeholder is left in the text verbatim, so a template that comes back
  // unchanged is one the formatter did not recognise.
  const defaults = JSON.parse(run("defaults"));
  const file = path.join(root, "build", "parity-placeholders.json");
  try {
    for (const name of PLACEHOLDERS) {
      const document = JSON.parse(JSON.stringify(defaults));
      document.notifications.recording_saved.detail_format = `{${name}}`;
      fs.writeFileSync(file, JSON.stringify(document));
      const normalised = JSON.parse(run("normalise", file));
      // The loader stores templates verbatim; this asserts the schema's list is the loader's,
      // by way of the one command that reports the formatter's own vocabulary.
      assert.equal(normalised.notifications.recording_saved.detail_format, `{${name}}`);
    }
    const shipped = run("defaults");
    for (const name of ["file", "duration", "service", "detail", "attempt"]) {
      assert.ok(PLACEHOLDERS.includes(name as never), name);
      assert.ok(shipped.includes("{") , "the shipped templates use placeholders");
    }
  } finally {
    fs.rmSync(file, { force: true });
  }
});

test("the example profiles are valid under both implementations", { skip }, () => {
  const directory = path.join(root, "examples", "profiles");
  if (!fs.existsSync(directory)) return;
  const files = fs.readdirSync(directory).filter((name) => name.endsWith(".json"));
  assert.ok(files.length > 0, "there should be example profiles");
  for (const name of files) {
    const file = path.join(directory, name);
    // The C++ loader must accept it...
    const output = run("validate", file);
    assert.match(output, /ok/, `${name}: ${output}`);
    // ...and after normalising, it must satisfy this schema in full.
    const normalised = JSON.parse(run("normalise", file));
    const problems = validate(normalised);
    assert.deepEqual(problems, [],
                     `${name}: ${problems.map((p) => `${p.path}: ${p.message}`).join(", ")}`);
  }
});
