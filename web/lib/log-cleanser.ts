/**
 * Realtime Log Workbench — Data Cleansing & Type Anchoring Module
 *
 * Enforces strict sanitization rules on the parsed JSONL stream before
 * schema inference, correlation, and visual rendering:
 *
 * 1. Schema Drift Mitigation      – anchor field types; rescue embedded nums
 * 2. Metric Boundary Boxing       – clamp latency / cpu / mem to phys. limits
 * 3. Scale Explosion Prevention   – clamp scientific giants to MAX_SAFE_INT
 * 4. Special Value Exclusion      – strip NaN / Infinity; increment null ratio
 * 5. Dynamic Column Bloat Guard   – hard cap at 32 dynamic fields
 */

import type { ParsedLogRow, FieldSchema } from "./log-parser";

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */
const MAX_DYNAMIC_FIELDS = 32;
const SCALE_EXPLOSION_THRESHOLD = 1e15;
const MAX_SAFE_INTEGER = Number.MAX_SAFE_INTEGER; /* 9007199254740991 */
const COUNTER_MAX = 1e9;
const MEMORY_MB_MAX = 1048576;                    /* 1 TiB in MiB */
const CPU_MAX = 100.0;

const CORE_FIELDS = new Set([
  "raw",
  "line_index",
  "timestamp",
  "timestampLabel",
  "severity",
  "entity",
  "message",
  "shapeKey",
]);

/* -------------------------------------------------------------------------- */
/* Type helpers                                                               */
/* -------------------------------------------------------------------------- */
function inferType(value: unknown): string {
  if (value === null) return "null";
  if (Array.isArray(value)) return "array";
  if (typeof value === "object") return "object";
  if (typeof value === "boolean") return "boolean";
  if (typeof value === "number") return "number";
  return "string";
}

function isSpecialNumericString(value: string): boolean {
  const normalized = value.trim().toLowerCase();
  return normalized === "nan" || normalized === "infinity" || normalized === "-infinity" || normalized === "inf" || normalized === "-inf";
}

function rescueLeadingNumber(value: string): number | null {
  const trimmed = value.trim();
  /* Match leading numeric prefix, including decimals and scientific notation */
  const match = trimmed.match(/^-?(?:\d+(?:\.\d+)?|\.\d+)(?:[eE][+-]?\d+)?/);
  if (match) {
    const num = Number(match[0]);
    if (Number.isFinite(num)) return num;
  }
  return null;
}

function classifyMetricKey(key: string): "latency" | "cpu" | "memory" | "counter" | "generic" {
  const lower = key.toLowerCase();
  if (lower.includes("latency") || lower.includes("duration")) return "latency";
  if (lower.includes("cpu") || lower.includes("cpu_util")) return "cpu";
  if (lower.includes("mem") || lower.includes("rss") || lower.includes("memory")) return "memory";
  if (lower.includes("active_users") || lower.includes("user_count") || lower.includes("request_count") || lower.includes("event_count")) return "counter";
  return "generic";
}

/* -------------------------------------------------------------------------- */
/* 1. Schema Drift Mitigation — type anchoring                                */
/* -------------------------------------------------------------------------- */

interface TypeAnchor {
  key: string;
  dominantType: string;
  numericRescue: boolean; /* true if field should attempt string→number rescue */
}

function buildTypeAnchors(rows: ParsedLogRow[]): Map<string, TypeAnchor> {
  const typeHistograms = new Map<string, Map<string, number>>();

  rows.forEach((row) => {
    Object.entries(row.fields).forEach(([key, value]) => {
      if (CORE_FIELDS.has(key)) return;
      const type = inferType(value);
      let hist = typeHistograms.get(key);
      if (!hist) {
        hist = new Map();
        typeHistograms.set(key, hist);
      }
      hist.set(type, (hist.get(type) || 0) + 1);
    });
  });

  const anchors = new Map<string, TypeAnchor>();
  typeHistograms.forEach((hist, key) => {
    let dominantType = "";
    let maxCount = 0;
    hist.forEach((count, type) => {
      if (count > maxCount) {
        maxCount = count;
        dominantType = type;
      }
    });

    /* If a field is >50 % numeric, anchor it as numeric and attempt rescue */
    const numericCount = hist.get("number") || 0;
    const total = Array.from(hist.values()).reduce((a, b) => a + b, 0);
    const numericRescue = numericCount > 0 && numericCount / total > 0.3;

    anchors.set(key, {
      key,
      dominantType,
      numericRescue: numericRescue && dominantType !== "boolean" && dominantType !== "object",
    });
  });

  return anchors;
}

/* -------------------------------------------------------------------------- */
/* 5. Dynamic Column Bloat Guard                                              */
/* -------------------------------------------------------------------------- */

function selectAllowedDynamicFields(rows: ParsedLogRow[]): Set<string> {
  const fieldCounts = new Map<string, number>();
  rows.forEach((row) => {
    Object.keys(row.fields).forEach((key) => {
      if (CORE_FIELDS.has(key)) return;
      fieldCounts.set(key, (fieldCounts.get(key) || 0) + 1);
    });
  });

  const sorted = Array.from(fieldCounts.entries())
    .sort((a, b) => b[1] - a[1])
    .slice(0, MAX_DYNAMIC_FIELDS)
    .map(([key]) => key);

  return new Set(sorted);
}

/* -------------------------------------------------------------------------- */
/* Cell-level cleansers                                                       */
/* -------------------------------------------------------------------------- */

function cleanseValue(key: string, value: unknown, anchor: TypeAnchor | undefined): unknown {
  const type = inferType(value);

  /* Rule 4: Explicit NaN / Infinity string literals → null */
  if (typeof value === "string" && isSpecialNumericString(value)) {
    return null;
  }

  /* Rule 4: Numeric NaN / Infinity → null */
  if (typeof value === "number" && !Number.isFinite(value)) {
    return null;
  }

  /* Rule 1: Type anchoring — if dominant type is numeric, rescue strings */
  if (anchor?.numericRescue && type === "string" && typeof value === "string") {
    const rescued = rescueLeadingNumber(value);
    if (rescued !== null) {
      return rescued;
    }
    /* Rescue failed → promote to null so it doesn't break numeric arrays */
    return null;
  }

  /* Rule 1: Boolean or empty object in a numeric-anchor field → null */
  if (anchor?.numericRescue && (type === "boolean" || (type === "object" && value !== null))) {
    return null;
  }

  /* If value is not a number by this point, skip metric boxing */
  if (typeof value !== "number" || !Number.isFinite(value)) {
    return value;
  }

  let v = value;

  /* Rule 3: Scale explosion prevention */
  if (Math.abs(v) > SCALE_EXPLOSION_THRESHOLD) {
    v = v > 0 ? MAX_SAFE_INTEGER : -MAX_SAFE_INTEGER;
  }

  /* Also clamp anything beyond MAX_SAFE_INTEGER (but below 1e37) */
  if (Math.abs(v) > MAX_SAFE_INTEGER) {
    v = v > 0 ? MAX_SAFE_INTEGER : -MAX_SAFE_INTEGER;
  }

  /* Rule 2: Metric boundary boxing */
  const mclass = classifyMetricKey(key);
  switch (mclass) {
    case "latency":
      if (v < 0) v = Math.abs(v);
      break;
    case "cpu":
      if (v < 0) v = 0;
      else if (v > CPU_MAX) v = CPU_MAX;
      break;
    case "memory":
      if (v < 0) v = 0;
      else if (v > MEMORY_MB_MAX) v = MEMORY_MB_MAX;
      break;
    case "counter":
      if (v < 0) v = 0;
      else if (v > COUNTER_MAX) v = COUNTER_MAX;
      break;
    default:
      break;
  }

  return v;
}

/* -------------------------------------------------------------------------- */
/* Row cleanser                                                               */
/* -------------------------------------------------------------------------- */

function cleanseRow(row: ParsedLogRow, anchors: Map<string, TypeAnchor>, allowedFields: Set<string>): ParsedLogRow {
  const cleansedFields: Record<string, unknown> = { ...row.fields };
  const cleansedNumeric: Record<string, number> = {};

  Object.entries(cleansedFields).forEach(([key, value]) => {
    /* Rule 5: Drop dynamic fields beyond the top-32 threshold */
    if (!CORE_FIELDS.has(key) && !allowedFields.has(key)) {
      delete cleansedFields[key];
      return;
    }

    const anchor = anchors.get(key);
    const cleaned = cleanseValue(key, value, anchor);
    cleansedFields[key] = cleaned;

    /* Rebuild numeric map */
    if (typeof cleaned === "number" && Number.isFinite(cleaned)) {
      cleansedNumeric[key] = cleaned;
    }
  });

  return {
    ...row,
    fields: cleansedFields,
    numeric: cleansedNumeric,
  };
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

export interface CleansingReport {
  inputRows: number;
  outputRows: number;
  droppedFields: string[];
  nullifiedCount: number;
  rescuedNumericCount: number;
  clampedScaleCount: number;
}

export function cleanseLogRows(rows: ParsedLogRow[]): { rows: ParsedLogRow[]; report: CleansingReport } {
  if (rows.length === 0) {
    return { rows: [], report: { inputRows: 0, outputRows: 0, droppedFields: [], nullifiedCount: 0, rescuedNumericCount: 0, clampedScaleCount: 0 } };
  }

  const anchors = buildTypeAnchors(rows);
  const allowedFields = selectAllowedDynamicFields(rows);

  /* Determine which fields were dropped by the bloat guard */
  const allNonCoreFields = new Set<string>();
  rows.forEach((row) => {
    Object.keys(row.fields).forEach((key) => {
      if (!CORE_FIELDS.has(key)) allNonCoreFields.add(key);
    });
  });
  const droppedFields = Array.from(allNonCoreFields).filter((k) => !allowedFields.has(k));

  let nullifiedCount = 0;
  let rescuedNumericCount = 0;
  let clampedScaleCount = 0;

  const cleansedRows = rows.map((row) => {
    const beforeKeys = Object.keys(row.fields);
    const beforeNumericKeys = Object.keys(row.numeric);

    const cleaned = cleanseRow(row, anchors, allowedFields);

    /* Tally statistics */
    Object.entries(cleaned.fields).forEach(([key, value]) => {
      if (value === null && row.fields[key] !== null) {
        nullifiedCount++;
      }
      if (typeof value === "number" && typeof row.fields[key] !== "number") {
        rescuedNumericCount++;
      }
      if (typeof value === "number" && typeof row.fields[key] === "number") {
        const old = row.fields[key] as number;
        if (Math.abs(old) > SCALE_EXPLOSION_THRESHOLD && Math.abs(value) === MAX_SAFE_INTEGER) {
          clampedScaleCount++;
        }
      }
    });

    /* If a row loses all non-core fields, keep it but note that it happened */
    return cleaned;
  });

  const report: CleansingReport = {
    inputRows: rows.length,
    outputRows: cleansedRows.length,
    droppedFields,
    nullifiedCount,
    rescuedNumericCount,
    clampedScaleCount,
  };

  return { rows: cleansedRows, report };
}
