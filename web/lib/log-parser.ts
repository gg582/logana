const TIMESTAMP_KEYS = new Set([
  "timestamp", "@timestamp", "ts", "time", "time_local", "event_time", "created_at", "datetime", "date",
]);

const INTERNAL_NUMERIC_KEYS = new Set(["line_index"]);

const SEVERITY_KEYS = new Set(["level", "lvl", "severity", "priority", "log_level"]);
const ENTITY_KEYS = new Set(["service", "svc", "source", "app", "module", "component", "host", "node", "pod", "container"]);
const MESSAGE_KEYS = new Set(["msg", "message", "text", "event", "detail", "description", "error", "reason", "log"]);

export type HistogramBar = { label: string; value: number };

export type SeriesChart = {
  key: string;
  color: string;
  points: { label: string; value: number }[];
  min: number;
  max: number;
};

export type ScatterSeries = {
  key: string;
  xKey: string;
  yKey: string;
  color: string;
  points: { x: number; y: number; label: string }[];
};

export type CorrelationCell = {
  x: string;
  y: string;
  value: number; // -1 ~ 1
};

export type FieldSchema = {
  key: string;
  types: Map<string, number>; // type -> count
  nullCount: number;
  numericCount: number;
  stringCount: number;
  boolCount: number;
  arrayCount: number;
  objectCount: number;
  min?: number;
  max?: number;
  mean?: number;
  stddev?: number;
  uniqueValues: Set<string>;
  sampleValues: unknown[];
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
  scatterSeries: ScatterSeries[];
  correlations: CorrelationCell[];
  fieldSchemas: FieldSchema[];
  booleanHistograms: { key: string; trueCount: number; falseCount: number }[];
  textLengthSeries: SeriesChart[];
  nullRatioBars: HistogramBar[];
  dominantKeys: string[];
  invalidRows: number;
  categoryKey: string;
  primaryKey: string;
  timestampKey: string;
};

const PALETTE = ["#ffda7b", "#ff8c6a", "#f45d96", "#7ad7ff", "#8ef0b5", "#c8a4ff", "#ffb3c1", "#a0c4ff"];

function isNumericLike(value: string) {
  return /^-?(?:\d+(?:\.\d+)?|\.\d+)(?:e[+-]?\d+)?$/i.test(value.trim()) || /^0x[0-9a-f]+$/i.test(value.trim());
}

function coerceValue(key: string, value: string): unknown {
  const trimmed = value.trim();
  const unwrapped =
    (trimmed.startsWith('"') && trimmed.endsWith('"')) || (trimmed.startsWith("'") && trimmed.endsWith("'"))
      ? trimmed.slice(1, -1)
      : trimmed;

  if (/^(true|false)$/i.test(unwrapped)) return unwrapped.toLowerCase() === "true";
  if (/^null$/i.test(unwrapped)) return null;
  if (TIMESTAMP_KEYS.has(key.toLowerCase())) {
    const parsed = Date.parse(unwrapped);
    if (Number.isFinite(parsed)) return parsed;
  }
  if (isNumericLike(unwrapped)) {
    return unwrapped.toLowerCase().startsWith("0x") ? Number.parseInt(unwrapped, 16) : Number(unwrapped);
  }
  return unwrapped;
}

function parseJsonLine(line: string): Record<string, unknown> | null {
  try {
    const parsed = JSON.parse(line);
    if (parsed && typeof parsed === "object" && !Array.isArray(parsed)) return parsed as Record<string, unknown>;
  } catch {
    // fall through
  }
  return null;
}

function flattenObject(obj: unknown, prefix = "", depth = 0): Record<string, unknown> {
  if (depth > 4) return prefix ? { [prefix]: obj } : {};
  if (obj === null || typeof obj !== "object") return prefix ? { [prefix]: obj } : {};
  if (Array.isArray(obj)) {
    // Store array length as a metric, and also first few items flattened
    const result: Record<string, unknown> = prefix ? { [`${prefix}._length`]: obj.length } : {};
    obj.slice(0, 3).forEach((item, i) => {
      const nested = flattenObject(item, prefix ? `${prefix}[${i}]` : `[${i}]`, depth + 1);
      Object.assign(result, nested);
    });
    return result;
  }
  const result: Record<string, unknown> = {};
  for (const [key, value] of Object.entries(obj as Record<string, unknown>)) {
    const flatKey = prefix ? `${prefix}.${key}` : key;
    if (value !== null && typeof value === "object" && !Array.isArray(value)) {
      Object.assign(result, flattenObject(value, flatKey, depth + 1));
    } else {
      result[flatKey] = value;
    }
  }
  return result;
}

function parseKeyValues(line: string): Record<string, unknown> {
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
      if (Number.isFinite(parsed)) return { key, timestamp: parsed, label: value.trim() };
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

function inferType(value: unknown): string {
  if (value === null) return "null";
  if (Array.isArray(value)) return "array";
  if (typeof value === "object") return "object";
  if (typeof value === "boolean") return "boolean";
  if (typeof value === "number") return "number";
  return "string";
}

function buildFieldSchemas(rows: ParsedLogRow[]): FieldSchema[] {
  const schemas = new Map<string, FieldSchema>();

  rows.forEach((row) => {
    Object.entries(row.fields).forEach(([key, value]) => {
      if (key === "raw" || key === "line_index") return;
      let s = schemas.get(key);
      if (!s) {
        s = {
          key,
          types: new Map(),
          nullCount: 0,
          numericCount: 0,
          stringCount: 0,
          boolCount: 0,
          arrayCount: 0,
          objectCount: 0,
          uniqueValues: new Set(),
          sampleValues: [],
        };
        schemas.set(key, s);
      }

      const t = inferType(value);
      s.types.set(t, (s.types.get(t) ?? 0) + 1);

      if (value === null) {
        s.nullCount++;
      } else if (typeof value === "number" && Number.isFinite(value)) {
        s.numericCount++;
        if (s.min === undefined || value < s.min) s.min = value;
        if (s.max === undefined || value > s.max) s.max = value;
        s.mean = (s.mean ?? 0) + value;
      } else if (typeof value === "boolean") {
        s.boolCount++;
      } else if (typeof value === "string") {
        s.stringCount++;
        s.uniqueValues.add(value);
      } else if (Array.isArray(value)) {
        s.arrayCount++;
      } else if (typeof value === "object") {
        s.objectCount++;
      }

      if (s.sampleValues.length < 5 && !s.sampleValues.some((v) => JSON.stringify(v) === JSON.stringify(value))) {
        s.sampleValues.push(value);
      }
    });
  });

  // finalize means & stddev
  schemas.forEach((s) => {
    if (s.numericCount > 0 && s.mean !== undefined) {
      s.mean /= s.numericCount;
    }
  });

  // second pass for stddev
  rows.forEach((row) => {
    Object.entries(row.fields).forEach(([key, value]) => {
      const s = schemas.get(key);
      if (s && typeof value === "number" && Number.isFinite(value) && s.mean !== undefined) {
        s.stddev = (s.stddev ?? 0) + (value - s.mean) ** 2;
      }
    });
  });

  schemas.forEach((s) => {
    if (s.numericCount > 0 && s.stddev !== undefined) {
      s.stddev = Math.sqrt(s.stddev / s.numericCount);
    }
  });

  return Array.from(schemas.values()).sort((a, b) => {
    const aTotal = a.numericCount + a.stringCount + a.boolCount;
    const bTotal = b.numericCount + b.stringCount + b.boolCount;
    return bTotal - aTotal;
  });
}

function computeCorrelations(rows: ParsedLogRow[], numericKeys: string[]): CorrelationCell[] {
  const cells: CorrelationCell[] = [];
  if (numericKeys.length < 2) return cells;

  const means = new Map<string, number>();
  numericKeys.forEach((key) => {
    const values = rows.map((r) => r.numeric[key]).filter((v): v is number => typeof v === "number");
    if (values.length) means.set(key, values.reduce((a, b) => a + b, 0) / values.length);
  });

  for (let i = 0; i < numericKeys.length; i++) {
    for (let j = i + 1; j < numericKeys.length; j++) {
      const xKey = numericKeys[i];
      const yKey = numericKeys[j];
      let num = 0;
      let denX = 0;
      let denY = 0;
      let count = 0;
      rows.forEach((row) => {
        const x = row.numeric[xKey];
        const y = row.numeric[yKey];
        if (typeof x !== "number" || typeof y !== "number") return;
        const mx = means.get(xKey) ?? 0;
        const my = means.get(yKey) ?? 0;
        const dx = x - mx;
        const dy = y - my;
        num += dx * dy;
        denX += dx * dx;
        denY += dy * dy;
        count++;
      });
      if (count > 1 && denX > 0 && denY > 0) {
        cells.push({ x: xKey, y: yKey, value: num / Math.sqrt(denX * denY) });
      }
    }
  }
  return cells;
}

export function parseLogStream(payload: string): ParsedLogDataset {
  const lines = payload
    .split("\n")
    .map((line) => line.trim())
    .filter(Boolean);

  const rows = lines.map((line, index) => {
    const jsonFields = parseJsonLine(line);
    const flattened = jsonFields ? flattenObject(jsonFields) : null;
    const fields = flattened ?? parseKeyValues(line);
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
      if (typeof value === "number" && Number.isFinite(value)) acc[key] = value;
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

  const fieldSchemas = buildFieldSchemas(rows);

  const numericKeys = fieldSchemas
    .filter((s) => s.numericCount >= Math.max(1, rows.length * 0.3))
    .map((s) => s.key)
    .filter((k) => !INTERNAL_NUMERIC_KEYS.has(k));

  const stringKeys = fieldSchemas
    .filter((s) => s.stringCount > 0)
    .map((s) => s.key);

  const boolKeys = fieldSchemas
    .filter((s) => s.boolCount > 0)
    .map((s) => s.key);

  const correlations = computeCorrelations(rows, numericKeys);

  // Series: all numeric keys over time (or row index)
  const series = numericKeys
    .map((key, index) => {
      const points = rows
        .map((row) => ({ label: row.timestampLabel, value: row.numeric[key] }))
        .filter((point): point is { label: string; value: number } => typeof point.value === "number");
      if (points.length < 2) return null;
      const values = points.map((p) => p.value);
      return {
        key,
        color: PALETTE[index % PALETTE.length],
        points,
        min: Math.min(...values),
        max: Math.max(...values),
      } satisfies SeriesChart;
    })
    .filter((item): item is SeriesChart => Boolean(item));

  // Scatter: pick best pairs by correlation or just first 2 numeric keys
  const scatterSeries: ScatterSeries[] = [];
  if (numericKeys.length >= 2) {
    // If we have correlations, pick the strongest pairs
    const sortedCorr = [...correlations].sort((a, b) => Math.abs(b.value) - Math.abs(a.value));
    const usedPairs = new Set<string>();
    const addScatter = (xKey: string, yKey: string, colorIdx: number) => {
      const pairKey = [xKey, yKey].sort().join("::");
      if (usedPairs.has(pairKey)) return;
      usedPairs.add(pairKey);
      const points = rows
        .map((row) => {
          const x = row.numeric[xKey];
          const y = row.numeric[yKey];
          if (typeof x !== "number" || typeof y !== "number") return null;
          return { x, y, label: row.timestampLabel };
        })
        .filter((p): p is { x: number; y: number; label: string } => p !== null);
      if (points.length >= 2) {
        scatterSeries.push({
          key: `${xKey} vs ${yKey}`,
          xKey,
          yKey,
          color: PALETTE[colorIdx % PALETTE.length],
          points,
        });
      }
    };

    let colorIdx = 0;
    for (const corr of sortedCorr.slice(0, 3)) {
      addScatter(corr.x, corr.y, colorIdx++);
    }
    // Fallback: first two numeric keys if no correlation computed
    if (scatterSeries.length === 0) {
      addScatter(numericKeys[0], numericKeys[1], 0);
    }
    // Also add any other numeric pairs if we have more than 2 numeric keys (max 3 total)
    if (numericKeys.length >= 3 && scatterSeries.length < 3) {
      addScatter(numericKeys[0], numericKeys[2], colorIdx++);
    }
  }

  // Boolean histograms
  const booleanHistograms = boolKeys.map((key) => {
    let trueCount = 0;
    let falseCount = 0;
    rows.forEach((row) => {
      const v = row.fields[key];
      if (v === true) trueCount++;
      else if (v === false) falseCount++;
    });
    return { key, trueCount, falseCount };
  });

  // Text length series for message-like fields
  const textLengthSeries: SeriesChart[] = [];
  const textKeys = [...MESSAGE_KEYS, ...stringKeys.filter((k) => k.includes("message") || k.includes("msg") || k.includes("text"))];
  const uniqueTextKeys = Array.from(new Set(textKeys));
  uniqueTextKeys.forEach((key, index) => {
    const points = rows
      .map((row) => {
        const v = row.fields[key];
        if (typeof v !== "string") return null;
        return { label: row.timestampLabel, value: v.length };
      })
      .filter((p): p is { label: string; value: number } => p !== null);
    if (points.length >= 2 && points.some((p) => p.value > 0)) {
      const values = points.map((p) => p.value);
      textLengthSeries.push({
        key: `${key} length`,
        color: PALETTE[(index + 3) % PALETTE.length],
        points,
        min: Math.min(...values),
        max: Math.max(...values),
      });
    }
  });

  // Null ratio bars
  const nullRatioBars = fieldSchemas
    .map((s) => ({ label: s.key, value: Math.round((s.nullCount / Math.max(rows.length, 1)) * 1000) / 10 }))
    .filter((b) => b.value > 0)
    .sort((a, b) => b.value - a.value)
    .slice(0, 10);

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

  // Smart category histogram
  const severityCounts = new Map<string, number>();
  const entityCounts = new Map<string, number>();
  const lowCardinalityStrings: { key: string; values: Map<string, number> }[] = [];

  rows.forEach((row) => {
    if (row.severity) severityCounts.set(row.severity, (severityCounts.get(row.severity) ?? 0) + 1);
    if (row.entity && row.entity !== "unknown" && row.entity !== "unstructured") {
      entityCounts.set(row.entity, (entityCounts.get(row.entity) ?? 0) + 1);
    }
  });

  // Find low-cardinality string fields for donut charts
  for (const s of fieldSchemas) {
    if (s.stringCount > 0 && s.uniqueValues.size > 1 && s.uniqueValues.size <= 12 && s.uniqueValues.size < rows.length * 0.5) {
      const values = new Map<string, number>();
      rows.forEach((row) => {
        const v = row.fields[s.key];
        if (typeof v === "string") values.set(v, (values.get(v) ?? 0) + 1);
      });
      lowCardinalityStrings.push({ key: s.key, values });
    }
  }

  let histogram: HistogramBar[];
  let categoryKey: string;

  if (severityCounts.size > 0) {
    histogram = Array.from(severityCounts.entries())
      .map(([label, value]) => ({ label, value }))
      .sort((a, b) => b.value - a.value);
    categoryKey = [...SEVERITY_KEYS].find((key) => rows.some((r) => typeof r.fields[key] === "string")) ?? "severity";
  } else if (entityCounts.size > 0) {
    histogram = Array.from(entityCounts.entries())
      .map(([label, value]) => ({ label, value }))
      .sort((a, b) => b.value - a.value);
    categoryKey = [...ENTITY_KEYS].find((key) => rows.some((r) => typeof r.fields[key] === "string")) ?? "service";
  } else if (lowCardinalityStrings.length > 0) {
    const best = lowCardinalityStrings.sort((a, b) => b.values.size - a.values.size)[0];
    histogram = Array.from(best.values.entries())
      .map(([label, value]) => ({ label, value }))
      .sort((a, b) => b.value - a.value);
    categoryKey = best.key;
  } else {
    histogram = formatCoverage;
    categoryKey = "format";
  }

  return {
    rows,
    histogram,
    fieldCoverage,
    shapeCoverage,
    formatCoverage,
    series,
    scatterSeries,
    correlations,
    fieldSchemas,
    booleanHistograms,
    textLengthSeries,
    nullRatioBars,
    dominantKeys,
    invalidRows: rows.filter((row) => row.format === "text").length,
    categoryKey,
    primaryKey,
    timestampKey: rows.some((row) => row.timestampLabel.startsWith("row ")) ? "row index" : "timestamp",
  };
}
