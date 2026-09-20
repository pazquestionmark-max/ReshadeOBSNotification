// SPDX-License-Identifier: MIT
// obsn: a mock OBS producer, and a validator for overlay profiles.

import fs from "node:fs";
import process from "node:process";

import { MockObs, defaultEndpoint } from "./mock-obs.ts";
import { SCENARIOS, findScenario } from "./scenario.ts";
import { CATEGORIES, PLACEHOLDERS, SCHEMA, validate } from "./config-schema.ts";

function usage(): never {
  process.stderr.write(
    `usage: obsn <command> [options]

  mock [--scenario <name>] [--endpoint <path>] [--once]
        Pretend to be OBS, so the in-game overlay can be looked at without OBS running.
        The add-on connects to it exactly as it connects to the real script.

  scenarios
        List the scripted sessions the mock can play.

  validate <file>...
        Check profiles against the configuration schema.

  schema
        Print every setting, its type and its range.

  placeholders
        Print the placeholders a title or detail template may use.
`);
  process.exit(2);
}

function parseOptions(argv: readonly string[]): Map<string, string | boolean> {
  const options = new Map<string, string | boolean>();
  for (let i = 0; i < argv.length; i += 1) {
    const argument = argv[i];
    if (argument === undefined || !argument.startsWith("--")) continue;
    const key = argument.slice(2);
    const next = argv[i + 1];
    if (next !== undefined && !next.startsWith("--")) {
      options.set(key, next);
      i += 1;
    } else {
      options.set(key, true);
    }
  }
  return options;
}

async function mock(argv: readonly string[]): Promise<number> {
  const options = parseOptions(argv);
  const name = typeof options.get("scenario") === "string"
    ? (options.get("scenario") as string)
    : "session";
  const scenario = findScenario(name);
  if (!scenario) {
    process.stderr.write(`unknown scenario '${name}'. Try: obsn scenarios\n`);
    return 2;
  }
  const endpoint = typeof options.get("endpoint") === "string"
    ? (options.get("endpoint") as string)
    : defaultEndpoint();

  const server = new MockObs({
    endpoint,
    scenario: options.get("once") === true ? { ...scenario, loop: false } : scenario,
    onLog: (message) => process.stdout.write(`${message}\n`),
  });

  process.stdout.write(`${scenario.name}: ${scenario.description}\n`);
  try {
    await server.start();
  } catch (error) {
    process.stderr.write(`could not listen on ${endpoint}: ${String(error)}\n`);
    return 1;
  }
  process.stdout.write("Press Ctrl-C to stop.\n");

  await new Promise<void>((resolve) => {
    const shutdown = () => {
      void server.stop().then(resolve);
    };
    process.on("SIGINT", shutdown);
    process.on("SIGTERM", shutdown);
  });
  return 0;
}

function scenarios(): number {
  for (const scenario of SCENARIOS) {
    process.stdout.write(`${scenario.name.padEnd(10)} ${scenario.description}\n`);
  }
  return 0;
}

function validateFiles(paths: readonly string[]): number {
  if (paths.length === 0) usage();
  let failures = 0;
  for (const path of paths) {
    let document: unknown;
    try {
      document = JSON.parse(fs.readFileSync(path, "utf8"));
    } catch (error) {
      process.stderr.write(`${path}: ${String(error)}\n`);
      failures += 1;
      continue;
    }
    const problems = validate(document);
    if (problems.length === 0) {
      process.stdout.write(`${path}: ok\n`);
      continue;
    }
    failures += 1;
    for (const problem of problems) {
      process.stdout.write(`${path}: ${problem.path}: ${problem.message}\n`);
    }
  }
  return failures === 0 ? 0 : 1;
}

function schema(): number {
  for (const setting of SCHEMA) {
    let detail = setting.type as string;
    if (setting.values) detail = setting.values.join(" | ");
    else if (setting.min !== undefined || setting.max !== undefined) {
      detail = `${setting.type} [${setting.min ?? "-"}, ${setting.max ?? "-"}]`;
    }
    process.stdout.write(`${setting.path.padEnd(52)} ${detail}\n`);
  }
  process.stdout.write(`\n${SCHEMA.length} settings, ${CATEGORIES.length} categories\n`);
  return 0;
}

function placeholders(): number {
  for (const name of PLACEHOLDERS) process.stdout.write(`{${name}}\n`);
  return 0;
}

const [command, ...rest] = process.argv.slice(2);
let code = 0;
switch (command) {
  case "mock":
    code = await mock(rest);
    break;
  case "scenarios":
    code = scenarios();
    break;
  case "validate":
    code = validateFiles(rest);
    break;
  case "schema":
    code = schema();
    break;
  case "placeholders":
    code = placeholders();
    break;
  default:
    usage();
}
process.exit(code);
