const TIMESTAMP_KEYS = new Set([
  "timestamp",
  "@timestamp",
  "ts",
  "time",
  "time_local",
  "event_time",
  "created_at",
  "datetime",
]);

const SEVERITY_KEYS = new Set(["level", "lvl", "severity", "priority"]);
const ENTITY_KEYS = new Set(["service", "svc", "source", "app", "module", "component", "host", "node"]);
const MESSAGE_KEYS = new Set(["msg", "message", "text", "event", "detail", "description", "error", "reason"]);

export type HistogramBar = {
  label: string;
  value: number;
};

export type SeriesChart = {
  key: string;
  color: string;
  points: { label: string; value: number }[];
  min: number;
  max: number;
};

export type ParsedLogRow = {
  raw: string;
  format: "json" | "kv" | "text";
  fields: Record<string, unknown>;
  numeric: Record<string, number>;
  timestamp: number;
  timestampLabel: string;
  severity: string;
  entity: string;
  message: string;
  shapeKey: string;
};

export type ParsedLogDataset = {
  rows: ParsedLogRow[];
  histogram: HistogramBar[];
  fieldCoverage: HistogramBar[];
  shapeCoverage: HistogramBar[];
  formatCoverage: HistogramBar[];
  series: SeriesChart[];
  dominantKeys: string[];
  invalidRows: number;
  categoryKey: string;
  primaryKey: string;
  timestampKey: string;
};

function isNumericLike(value: string) {
  return /^-?(?:\d+(?:\.\d+)?|\.\d+)(?:e[+-]?\d+)?$/i.test(value.trim()) || /^0x[0-9a-f]+$/i.test(value.trim());
}

function coerceValue(key: string, value: string) {
  const trimmed = value.trim();
  const unwrapped =
    (trimmed.startsWith('"') && trimmed.endsWith('"')) || (trimmed.startsWith("'") && trimmed.endsWith("'"))
      ? trimmed.slice(1, -1)
      : trimmed;

  if (/^(true|false)$/i.test(unwrapped)) {
    return unwrapped.toLowerCase() === "true";
  }
  if (/^null$/i.test(unwrapped)) {
    return null;
  }
  if (TIMESTAMP_KEYS.has(key.toLowerCase())) {
    const parsed = Date.parse(unwrapped);
    if (Number.isFinite(parsed)) return parsed;
  }
  if (isNumericLike(unwrapped)) {
    return unwrapped.toLowerCase().startsWith("0x") ? Number.parseInt(unwrapped, 16) : Number(unwrapped);
  }
  return unwrapped;
}

function parseJsonLine(line: string) {
  try {
    const parsed = JSON.parse(line);
    if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) {
      return parsed as Record<string, unknown>;
    }
  } catch {
    // fall through
  }
  return null;
}

function parseKeyValues(line: string) {
  const fields: Record<string, unknown> = {};
  const pattern = /(?:^|[\s,;|])([A-Za-z0-9_.@/-]+)\s*(=|:|=>)\s*/g;
  const matches = [...line.matchAll(pattern)];

  for (let index = 0; index < matches.length; index += 1) {
    const match = matches[index];
    const key = match[1];
    const start = (match.index ?? 0) + match[0].length;
    const end = index + 1 < matches.length ? (matches[index + 1].index ?? line.length) : line.length;
    const rawValue = line.slice(start, end).trim().replace(/[,\s;|]+$/, "");

    if (!rawValue) continue;
    fields[key] = coerceValue(key, rawValue);
  }

  return fields;
}

function extractMessage(raw: string, fields: Record<string, unknown>) {
  for (const key of MESSAGE_KEYS) {
    const value = fields[key];
    if (typeof value === "string" && value.trim()) return value.trim();
  }
  if (typeof fields.message === "string" && fields.message.trim()) return fields.message.trim();
  if (typeof fields.msg === "string" && fields.msg.trim()) return fields.msg.trim();
  return raw.trim();
}

function extractSeverity(fields: Record<string, unknown>, fallback: string) {
  for (const key of SEVERITY_KEYS) {
    const value = fields[key];
    if (typeof value === "string" && value.trim()) return value.trim().toUpperCase();
  }
  return fallback;
}

function extractEntity(fields: Record<string, unknown>) {
  for (const key of ENTITY_KEYS) {
    const value = fields[key];
    if (typeof value === "string" && value.trim()) return value.trim();
  }
  return "";
}

function extractTimestamp(fields: Record<string, unknown>, fallbackIndex: number) {
  for (const key of TIMESTAMP_KEYS) {
    const value = fields[key];
    if (typeof value === "number" && Number.isFinite(value)) {
      return { key, timestamp: value, label: new Date(value).toISOString() };
    }
    if (typeof value === "string" && value.trim()) {
      const parsed = Date.parse(value);
      if (Number.isFinite(parsed)) {
        return { key, timestamp: parsed, label: value.trim() };
      }
    }
  }
  return { key: "row index", timestamp: fallbackIndex, label: `row ${fallbackIndex + 1}` };
}

function rowShape(fields: Record<string, unknown>) {
  return Object.keys(fields)
    .filter((key) => !key.startsWith("__"))
    .sort((a, b) => a.localeCompare(b))
    .join(" | ");
}

export function parseLogStream(payload: string): ParsedLogDataset {
  const lines = payload
    .split("\n")
    .map((line) => line.trim())
    .filter(Boolean);

  const rows = lines.map((line, index) => {
    const jsonFields = parseJsonLine(line);
    const fields = jsonFields ?? parseKeyValues(line);
    const format: ParsedLogRow["format"] = jsonFields ? "json" : Object.keys(fields).length > 0 ? "kv" : "text";
    const normalizedFields: Record<string, unknown> = { ...fields };
    normalizedFields.raw = line;
    normalizedFields.line_index = index;
    if (!Object.keys(fields).length) {
      normalizedFields.message = line;
    }

    const timestamp = extractTimestamp(normalizedFields, index);
    const message = extractMessage(line, normalizedFields);
    const severity = extractSeverity(normalizedFields, format === "text" ? "TEXT" : "INFO");
    const entity = extractEntity(normalizedFields) || (format === "text" ? "unstructured" : "unknown");
    const numeric = Object.entries(normalizedFields).reduce<Record<string, number>>((acc, [key, value]) => {
      if (typeof value === "number" && Number.isFinite(value)) {
        acc[key] = value;
      }
      return acc;
    }, {});

    return {
      raw: line,
      format,
      fields: normalizedFields,
      numeric,
      timestamp: timestamp.timestamp,
      timestampLabel: timestamp.label,
      severity,
      entity,
      message,
      shapeKey: rowShape(normalizedFields) || format,
    };
  });

  const fieldCounts = new Map<string, number>();
  const shapeCounts = new Map<string, number>();
  const formatCounts = new Map<string, number>();

  rows.forEach((row) => {
    formatCounts.set(row.format, (formatCounts.get(row.format) ?? 0) + 1);
    shapeCounts.set(row.shapeKey, (shapeCounts.get(row.shapeKey) ?? 0) + 1);
    Object.keys(row.fields).forEach((key) => {
      if (key === "raw" || key === "line_index") return;
      fieldCounts.set(key, (fieldCounts.get(key) ?? 0) + 1);
    });
  });

  const numericKeys = Array.from(
    rows.reduce((set, row) => {
      Object.entries(row.numeric).forEach(([key]) => set.add(key));
      return set;
    }, new Set<string>()),
  );

  const series = numericKeys
    .map((key, index) => {
      const points = rows
        .map((row) => ({
          label: row.timestampLabel,
          value: row.numeric[key],
        }))
        .filter((point): point is { label: string; value: number } => typeof point.value === "number");

      if (points.length < 2) return null;

      const values = points.map((point) => point.value);
      return {
        key,
        color: ["#ffda7b", "#ff8c6a", "#f45d96", "#7ad7ff", "#8ef0b5", "#c8a4ff"][index % 6],
        points,
        min: Math.min(...values),
        max: Math.max(...values),
      } satisfies SeriesChart;
    })
    .filter((item): item is SeriesChart => Boolean(item));

  const totalRows = Math.max(rows.length, 1);
  const fieldCoverage = Array.from(fieldCounts.entries())
    .map(([label, value]) => ({ label, value }))
    .sort((a, b) => b.value - a.value)
    .slice(0, 8);
  const shapeCoverage = Array.from(shapeCounts.entries())
    .map(([label, value]) => ({ label, value }))
    .sort((a, b) => b.value - a.value)
    .slice(0, 8);
  const formatCoverage = Array.from(formatCounts.entries())
    .map(([label, value]) => ({ label, value }))
    .sort((a, b) => b.value - a.value);
  const dominantKeys = fieldCoverage
    .filter((bar) => bar.value / totalRows >= 0.5)
    .map((bar) => bar.label)
    .slice(0, 6);
  const primaryKey = dominantKeys[0] ?? (fieldCoverage[0]?.label ?? "record");

  return {
    rows,
    histogram: shapeCoverage,
    fieldCoverage,
    shapeCoverage,
    formatCoverage,
    series,
    dominantKeys,
    invalidRows: rows.filter((row) => row.format === "text").length,
    categoryKey: primaryKey,
    primaryKey,
    timestampKey: rows.some((row) => row.timestampLabel.startsWith("row ")) ? "row index" : "timestamp",
  };
}
