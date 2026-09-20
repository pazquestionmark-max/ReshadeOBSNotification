// SPDX-License-Identifier: MIT
// A second description of the configuration, used to check the first.
//
// The C++ loader is the authority: it is what the add-on runs. This mirror exists so CI can
// assert that every setting the loader writes is one this schema knows about, and vice versa --
// which is what catches a setting added to the struct and forgotten in the documentation, or a
// key renamed on one side only.

export type SettingType = "bool" | "number" | "string" | "color" | "enum";

export interface Setting {
  /** Dotted path, e.g. "notifications.box.corner_radius". */
  path: string;
  type: SettingType;
  /** For "enum", the accepted values, in the order the UI lists them. */
  values?: readonly string[];
  min?: number;
  max?: number;
}

export const ANCHORS = [
  "top_left", "top_center", "top_right",
  "center_left", "center", "center_right",
  "bottom_left", "bottom_center", "bottom_right",
] as const;

export const ALIGNS = ["left", "center", "right"] as const;

export const EASINGS = [
  "linear", "ease_in", "ease_out", "ease_in_out",
  "ease_in_cubic", "ease_out_cubic", "ease_in_out_cubic",
  "ease_out_quint", "ease_out_expo", "ease_out_back", "ease_out_elastic",
] as const;

export const ICONS = [
  "none", "dot", "circle", "ring", "square", "diamond", "triangle", "star", "chevron",
  "record", "record_ring", "stop", "pause", "play", "save", "replay", "clock", "camera",
  "broadcast", "film", "scissors", "warning", "check", "cross", "folder", "layers", "disk",
] as const;

export const MOTION_KINDS = [
  "none", "slide_from_edge", "slide_horizontal", "slide_vertical", "scale",
] as const;

export const ACCENT_STYLES = ["none", "bar", "tile", "bar_and_tile"] as const;
export const BORDERS = ["none", "accent", "custom"] as const;
export const STACKS = ["down", "up"] as const;

/** The notification categories, in the order the settings window lists them. */
export const CATEGORIES = [
  "recording_started", "recording_stopped", "recording_paused", "recording_resumed",
  "recording_saved",
  "replay_started", "replay_stopped", "replay_saved",
  "stream_started", "stream_stopped", "stream_reconnecting", "stream_reconnected",
  "virtual_cam_started", "virtual_cam_stopped",
  "scene_changed", "warning", "obs_connected", "obs_disconnected",
] as const;

/** Every placeholder a title or detail template may use. */
export const PLACEHOLDERS = [
  "file", "path", "folder", "duration", "size", "scene", "previous_scene", "profile",
  "service", "reason", "detail", "time", "attempt", "replay_seconds", "elapsed",
] as const;

function placement(prefix: string): Setting[] {
  return [
    { path: `${prefix}.visible`, type: "bool" },
    { path: `${prefix}.anchor`, type: "enum", values: ANCHORS },
    { path: `${prefix}.x`, type: "number" },
    { path: `${prefix}.y`, type: "number" },
    { path: `${prefix}.percent`, type: "bool" },
    { path: `${prefix}.align`, type: "enum", values: ALIGNS },
  ];
}

function fade(prefix: string): Setting[] {
  return [
    { path: `${prefix}.in_ms`, type: "number", min: 0, max: 10_000 },
    { path: `${prefix}.out_ms`, type: "number", min: 0, max: 10_000 },
    { path: `${prefix}.hold_ms`, type: "number", min: 0, max: 600_000 },
    { path: `${prefix}.start_opacity`, type: "number", min: 0, max: 1 },
    { path: `${prefix}.end_opacity`, type: "number", min: 0, max: 1 },
    { path: `${prefix}.easing`, type: "enum", values: EASINGS },
  ];
}

function motion(prefix: string): Setting[] {
  return [
    { path: `${prefix}.kind`, type: "enum", values: MOTION_KINDS },
    { path: `${prefix}.distance`, type: "number", min: 0, max: 4096 },
    { path: `${prefix}.scale_from`, type: "number", min: 0.1, max: 2 },
    { path: `${prefix}.in_easing`, type: "enum", values: EASINGS },
    { path: `${prefix}.out_easing`, type: "enum", values: EASINGS },
    { path: `${prefix}.exit_slides`, type: "bool" },
  ];
}

function category(key: string): Setting[] {
  const prefix = `notifications.${key}`;
  return [
    { path: `${prefix}.enabled`, type: "bool" },
    { path: `${prefix}.title_format`, type: "string" },
    { path: `${prefix}.detail_format`, type: "string" },
    { path: `${prefix}.icon`, type: "enum", values: ICONS },
    { path: `${prefix}.accent`, type: "color" },
    { path: `${prefix}.override_title_color`, type: "bool" },
    { path: `${prefix}.title_color`, type: "color" },
    { path: `${prefix}.override_detail_color`, type: "bool" },
    { path: `${prefix}.detail_color`, type: "color" },
    { path: `${prefix}.color_placeholders`, type: "bool" },
    { path: `${prefix}.placeholder_color`, type: "color" },
    ...fade(`${prefix}.fade`),
    { path: `${prefix}.override_motion`, type: "bool" },
    ...motion(`${prefix}.motion`),
    { path: `${prefix}.sound`, type: "bool" },
    { path: `${prefix}.sound_file`, type: "string" },
    { path: `${prefix}.wrap`, type: "bool" },
    { path: `${prefix}.max_lines`, type: "number", min: 1, max: 8 },
    { path: `${prefix}.priority`, type: "number", min: -100, max: 100 },
  ];
}

export const SCHEMA: readonly Setting[] = [
  { path: "config_version", type: "number" },

  { path: "general.enabled", type: "bool" },
  { path: "general.show_when_obs_closed", type: "bool" },
  { path: "general.master_opacity", type: "number", min: 0, max: 1 },
  { path: "general.scale", type: "number", min: 0.25, max: 4 },
  { path: "general.profile_name", type: "string" },
  { path: "general.auto_profile_by_executable", type: "bool" },
  { path: "general.advanced_settings", type: "bool" },

  { path: "appearance.font_file", type: "string" },
  { path: "appearance.font_face_index", type: "number", min: 0, max: 64 },
  { path: "appearance.title_weight", type: "number", min: 100, max: 900 },
  { path: "appearance.detail_weight", type: "number", min: 100, max: 900 },
  { path: "appearance.title_size", type: "number", min: 6, max: 96 },
  { path: "appearance.detail_size", type: "number", min: 6, max: 96 },
  { path: "appearance.line_gap", type: "number", min: -8, max: 32 },
  { path: "appearance.icon_size", type: "number", min: 0, max: 128 },
  { path: "appearance.title_color", type: "color" },
  { path: "appearance.detail_color", type: "color" },
  { path: "appearance.accent", type: "color" },
  { path: "appearance.text_shadow", type: "bool" },
  { path: "appearance.text_shadow_color", type: "color" },
  { path: "appearance.text_shadow_offset", type: "number", min: 0, max: 8 },
  { path: "appearance.text_outline", type: "bool" },
  { path: "appearance.text_outline_color", type: "color" },
  { path: "appearance.text_outline_thickness", type: "number", min: 0, max: 6 },

  ...placement("notifications.placement"),
  { path: "notifications.max_visible", type: "number", min: 1, max: 32 },
  { path: "notifications.stack", type: "enum", values: STACKS },
  { path: "notifications.spacing", type: "number", min: 0, max: 128 },
  { path: "notifications.width", type: "number", min: 64, max: 4096 },
  { path: "notifications.min_height", type: "number", min: 8, max: 512 },
  { path: "notifications.box.background", type: "color" },
  { path: "notifications.box.background_gradient", type: "color" },
  { path: "notifications.box.corner_radius", type: "number", min: 0, max: 64 },
  { path: "notifications.box.border", type: "enum", values: BORDERS },
  { path: "notifications.box.border_color", type: "color" },
  { path: "notifications.box.border_thickness", type: "number", min: 0, max: 8 },
  { path: "notifications.box.border_accent_opacity", type: "number", min: 0, max: 1 },
  { path: "notifications.box.accent_style", type: "enum", values: ACCENT_STYLES },
  { path: "notifications.box.accent_bar_width", type: "number", min: 0, max: 32 },
  { path: "notifications.box.accent_tile_width", type: "number", min: 0, max: 256 },
  { path: "notifications.box.accent_tile_opacity", type: "number", min: 0, max: 1 },
  { path: "notifications.box.tile_icon_color", type: "color" },
  { path: "notifications.box.shadow", type: "bool" },
  { path: "notifications.box.shadow_color", type: "color" },
  { path: "notifications.box.shadow_size", type: "number", min: 0, max: 64 },
  { path: "notifications.box.shadow_offset_y", type: "number", min: -32, max: 32 },
  { path: "notifications.box.auto_width", type: "bool" },
  { path: "notifications.box.max_width", type: "number", min: 80, max: 4096 },
  { path: "notifications.padding_x", type: "number", min: 0, max: 128 },
  { path: "notifications.padding_y", type: "number", min: 0, max: 128 },
  { path: "notifications.icon_gap", type: "number", min: 0, max: 128 },
  { path: "notifications.font_scale", type: "number", min: 0.25, max: 4 },
  ...motion("notifications.motion"),
  { path: "notifications.merge_duplicates", type: "bool" },
  { path: "notifications.suppress_after_connect_ms", type: "number", min: 0, max: 60_000 },
  { path: "notifications.min_interval_ms", type: "number", min: 0, max: 60_000 },
  ...CATEGORIES.flatMap(category),

  ...placement("status.placement"),
  { path: "status.show_while_recording", type: "bool" },
  { path: "status.show_while_streaming", type: "bool" },
  { path: "status.show_while_replay_armed", type: "bool" },
  { path: "status.show_timer", type: "bool" },
  { path: "status.pulse", type: "bool" },
  { path: "status.pulse_hz", type: "number", min: 0, max: 10 },
  { path: "status.pulse_depth", type: "number", min: 0, max: 1 },
  { path: "status.dot_size", type: "number", min: 0, max: 64 },
  { path: "status.font_size", type: "number", min: 6, max: 96 },
  { path: "status.padding_x", type: "number", min: 0, max: 64 },
  { path: "status.padding_y", type: "number", min: 0, max: 64 },
  { path: "status.corner_radius", type: "number", min: 0, max: 64 },
  { path: "status.show_background", type: "bool" },
  { path: "status.background", type: "color" },
  { path: "status.text", type: "color" },
  { path: "status.recording_color", type: "color" },
  { path: "status.streaming_color", type: "color" },
  { path: "status.paused_color", type: "color" },
  ...fade("status.fade"),

  { path: "animation.enabled", type: "bool" },
  { path: "animation.animate_restack", type: "bool" },
  { path: "animation.restack_ms", type: "number", min: 0, max: 2000 },
  { path: "animation.restack_easing", type: "enum", values: EASINGS },

  { path: "integration.pipe_name", type: "string" },
  { path: "integration.reconnect_initial_ms", type: "number", min: 50, max: 60_000 },
  { path: "integration.reconnect_max_ms", type: "number", min: 50, max: 300_000 },
  { path: "integration.stale_after_ms", type: "number", min: 500, max: 600_000 },
  { path: "integration.ping_interval_ms", type: "number", min: 500, max: 600_000 },
  { path: "integration.auto_connect", type: "bool" },

  { path: "logging.level", type: "string" },
  { path: "logging.to_file", type: "bool" },
  { path: "logging.file_name", type: "string" },
  { path: "logging.max_file_kb", type: "number", min: 16, max: 65_536 },
  { path: "logging.include_paths", type: "bool" },
  { path: "logging.show_diagnostics_overlay", type: "bool" },
];

/** Flattens a configuration document to the dotted paths of its leaves. */
export function flatten(value: unknown, prefix = ""): Map<string, unknown> {
  const out = new Map<string, unknown>();
  if (typeof value !== "object" || value === null || Array.isArray(value)) {
    if (prefix) out.set(prefix, value);
    return out;
  }
  for (const [key, child] of Object.entries(value as Record<string, unknown>)) {
    const path = prefix ? `${prefix}.${key}` : key;
    if (typeof child === "object" && child !== null && !Array.isArray(child)) {
      for (const [k, v] of flatten(child, path)) out.set(k, v);
    } else {
      out.set(path, child);
    }
  }
  return out;
}

export interface SchemaProblem {
  path: string;
  message: string;
}

const COLOR = /^#[0-9A-Fa-f]{8}$/;

/** Checks a configuration document against this schema. */
export function validate(document: unknown): SchemaProblem[] {
  const problems: SchemaProblem[] = [];
  const flat = flatten(document);
  const known = new Map(SCHEMA.map((setting) => [setting.path, setting]));

  for (const setting of SCHEMA) {
    if (!flat.has(setting.path)) {
      problems.push({ path: setting.path, message: "missing" });
      continue;
    }
    const value = flat.get(setting.path);
    switch (setting.type) {
      case "bool":
        if (typeof value !== "boolean") {
          problems.push({ path: setting.path, message: `expected a boolean, got ${typeof value}` });
        }
        break;
      case "number":
        if (typeof value !== "number" || !Number.isFinite(value)) {
          problems.push({ path: setting.path, message: `expected a number, got ${typeof value}` });
        } else if (setting.min !== undefined && value < setting.min) {
          problems.push({ path: setting.path, message: `below the minimum ${setting.min}` });
        } else if (setting.max !== undefined && value > setting.max) {
          problems.push({ path: setting.path, message: `above the maximum ${setting.max}` });
        }
        break;
      case "string":
        if (typeof value !== "string") {
          problems.push({ path: setting.path, message: `expected a string, got ${typeof value}` });
        }
        break;
      case "color":
        if (typeof value !== "string" || !COLOR.test(value)) {
          problems.push({ path: setting.path, message: "expected a #RRGGBBAA colour" });
        }
        break;
      case "enum":
        if (typeof value !== "string" || !setting.values?.includes(value)) {
          problems.push({
            path: setting.path,
            message: `expected one of: ${setting.values?.join(", ")}`,
          });
        }
        break;
    }
  }

  for (const path of flat.keys()) {
    if (!known.has(path)) {
      problems.push({ path, message: "not a setting this schema knows about" });
    }
  }
  return problems;
}
