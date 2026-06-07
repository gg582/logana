"use client";

import { Children, isValidElement, useEffect, useId, useLayoutEffect, useMemo, useRef, useState } from "react";
import {
  parseLogStream,
  type HistogramBar,
  type SeriesChart,
  type ScatterSeries,
  type CorrelationCell,
  type FieldSchema,
  type ParsedLogDataset,
} from "../lib/log-parser";

const SAMPLE = `{"timestamp":"2026-05-31T18:00:01.000Z","level":"INFO","service":"auth-api","latency_ms":20.0,"http_status":200,"active_users":1400,"cpu_util":40.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:02.000Z","level":"INFO","service":"payment-v2","latency_ms":110.3,"http_status":200,"active_users":451,"cpu_util":30.1,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:00:03.000Z","level":"INFO","service":"mesh-router","latency_ms":4.1,"http_status":200,"active_users":8820,"cpu_util":15.1,"mem_rss_mb":128.0}
{"timestamp":"2026-05-31T18:00:04.000Z","level":"INFO","service":"auth-api","latency_ms":21.5,"http_status":200,"active_users":1406,"cpu_util":40.6,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:05.000Z","level":"INFO","service":"payment-v2","latency_ms":111.2,"http_status":200,"active_users":450,"cpu_util":30.4,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:00:06.000Z","level":"INFO","service":"mesh-router","latency_ms":4.25,"http_status":200,"active_users":8850,"cpu_util":15.25,"mem_rss_mb":128.0}
{"timestamp":"2026-05-31T18:00:07.000Z","level":"INFO","service":"auth-api","latency_ms":23.0,"http_status":200,"active_users":1412,"cpu_util":41.2,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:08.000Z","level":"INFO","service":"payment-v2","latency_ms":112.1,"http_status":200,"active_users":452,"cpu_util":30.7,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:00:09.000Z","level":"INFO","service":"mesh-router","latency_ms":4.4,"http_status":200,"active_users":8880,"cpu_util":15.4,"mem_rss_mb":128.0}
{"timestamp":"2026-05-31T18:00:10.000Z","level":"INFO","service":"auth-api","latency_ms":24.5,"http_status":200,"active_users":1418,"cpu_util":41.8,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:11.000Z","level":"INFO","service":"payment-v2","latency_ms":113.0,"http_status":200,"active_users":451,"cpu_util":31.0,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:00:12.000Z","level":"INFO","service":"mesh-router","latency_ms":4.55,"http_status":200,"active_users":8910,"cpu_util":15.55,"mem_rss_mb":128.0}
{"timestamp":"2026-05-31T18:00:13.000Z","level":"INFO","service":"auth-api","latency_ms":26.0,"http_status":200,"active_users":1424,"cpu_util":42.4,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:14.000Z","level":"INFO","service":"payment-v2","latency_ms":113.9,"http_status":200,"active_users":453,"cpu_util":31.3,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:00:15.000Z","level":"INFO","service":"mesh-router","latency_ms":4.7,"http_status":200,"active_users":8940,"cpu_util":15.7,"mem_rss_mb":128.0}
{"timestamp":"2026-05-31T18:00:16.000Z","level":"INFO","service":"auth-api","latency_ms":27.5,"http_status":200,"active_users":1430,"cpu_util":43.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:17.000Z","level":"INFO","service":"payment-v2","latency_ms":114.8,"http_status":200,"active_users":452,"cpu_util":31.6,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:00:18.000Z","level":"INFO","service":"mesh-router","latency_ms":4.85,"http_status":200,"active_users":8970,"cpu_util":15.85,"mem_rss_mb":128.0}
{"timestamp":"2026-05-31T18:00:19.000Z","level":"INFO","service":"auth-api","latency_ms":29.0,"http_status":200,"active_users":1436,"cpu_util":43.6,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:20.000Z","level":"INFO","service":"payment-v2","latency_ms":115.7,"http_status":200,"active_users":454,"cpu_util":31.9,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:00:21.000Z","level":"INFO","service":"auth-api","latency_ms":30.0,"http_status":200,"active_users":1500,"cpu_util":45.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:22.000Z","level":"INFO","service":"auth-api","latency_ms":125.0,"http_status":200,"active_users":1650,"cpu_util":49.5,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:23.000Z","level":"INFO","service":"auth-api","latency_ms":220.0,"http_status":200,"active_users":1800,"cpu_util":54.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:24.000Z","level":"INFO","service":"auth-api","latency_ms":315.0,"http_status":200,"active_users":1950,"cpu_util":58.5,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:25.000Z","level":"INFO","service":"auth-api","latency_ms":410.0,"http_status":200,"active_users":2100,"cpu_util":63.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:26.000Z","level":"INFO","service":"auth-api","latency_ms":505.0,"http_status":200,"active_users":2250,"cpu_util":67.5,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:27.000Z","level":"INFO","service":"auth-api","latency_ms":600.0,"http_status":200,"active_users":2400,"cpu_util":72.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:28.000Z","level":"INFO","service":"auth-api","latency_ms":695.0,"http_status":200,"active_users":2550,"cpu_util":76.5,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:29.000Z","level":"WARN","service":"auth-api","latency_ms":790.0,"http_status":200,"active_users":2700,"cpu_util":81.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:30.000Z","level":"WARN","service":"auth-api","latency_ms":885.0,"http_status":200,"active_users":2850,"cpu_util":85.5,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:31.000Z","level":"WARN","service":"auth-api","latency_ms":980.0,"http_status":200,"active_users":3000,"cpu_util":90.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:32.000Z","level":"WARN","service":"auth-api","latency_ms":1075.0,"http_status":200,"active_users":3150,"cpu_util":94.5,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:33.000Z","level":"WARN","service":"auth-api","latency_ms":1170.0,"http_status":200,"active_users":3300,"cpu_util":99.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:34.000Z","level":"WARN","service":"auth-api","latency_ms":1265.0,"http_status":200,"active_users":3450,"cpu_util":99.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:35.000Z","level":"ERROR","service":"auth-api","latency_ms":1360.0,"http_status":500,"active_users":3600,"cpu_util":99.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:36.000Z","level":"ERROR","service":"auth-api","latency_ms":1455.0,"http_status":500,"active_users":3750,"cpu_util":99.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:37.000Z","level":"ERROR","service":"auth-api","latency_ms":1550.0,"http_status":500,"active_users":3900,"cpu_util":99.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:38.000Z","level":"ERROR","service":"auth-api","latency_ms":1645.0,"http_status":500,"active_users":4050,"cpu_util":99.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:39.000Z","level":"ERROR","service":"auth-api","latency_ms":1740.0,"http_status":500,"active_users":4200,"cpu_util":99.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:40.000Z","level":"FATAL","service":"auth-api","latency_ms":1835.0,"http_status":500,"active_users":4350,"cpu_util":99.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:00:41.000Z","level":"INFO","service":"payment-v2","latency_ms":120.0,"http_status":200,"active_users":460,"cpu_util":32.0,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:00:42.000Z","level":"INFO","service":"payment-v2","latency_ms":122.0,"http_status":200,"active_users":461,"cpu_util":32.5,"mem_rss_mb":1069.0}
{"timestamp":"2026-05-31T18:00:43.000Z","level":"INFO","service":"payment-v2","latency_ms":124.0,"http_status":200,"active_users":462,"cpu_util":33.0,"mem_rss_mb":1114.0}
{"timestamp":"2026-05-31T18:00:44.000Z","level":"INFO","service":"payment-v2","latency_ms":126.0,"http_status":200,"active_users":460,"cpu_util":33.5,"mem_rss_mb":1159.0}
{"timestamp":"2026-05-31T18:00:45.000Z","level":"INFO","service":"payment-v2","latency_ms":128.0,"http_status":200,"active_users":461,"cpu_util":34.0,"mem_rss_mb":1204.0}
{"timestamp":"2026-05-31T18:00:46.000Z","level":"INFO","service":"payment-v2","latency_ms":130.0,"http_status":200,"active_users":462,"cpu_util":34.5,"mem_rss_mb":1249.0}
{"timestamp":"2026-05-31T18:00:47.000Z","level":"INFO","service":"payment-v2","latency_ms":132.0,"http_status":200,"active_users":460,"cpu_util":35.0,"mem_rss_mb":1294.0}
{"timestamp":"2026-05-31T18:00:48.000Z","level":"INFO","service":"payment-v2","latency_ms":134.0,"http_status":200,"active_users":461,"cpu_util":35.5,"mem_rss_mb":1339.0}
{"timestamp":"2026-05-31T18:00:49.000Z","level":"INFO","service":"payment-v2","latency_ms":136.0,"http_status":200,"active_users":462,"cpu_util":36.0,"mem_rss_mb":1384.0}
{"timestamp":"2026-05-31T18:00:50.000Z","level":"INFO","service":"payment-v2","latency_ms":138.0,"http_status":200,"active_users":460,"cpu_util":36.5,"mem_rss_mb":1429.0}
{"timestamp":"2026-05-31T18:00:51.000Z","level":"INFO","service":"payment-v2","latency_ms":140.0,"http_status":200,"active_users":461,"cpu_util":37.0,"mem_rss_mb":1474.0}
{"timestamp":"2026-05-31T18:00:52.000Z","level":"INFO","service":"payment-v2","latency_ms":142.0,"http_status":200,"active_users":462,"cpu_util":37.5,"mem_rss_mb":1519.0}
{"timestamp":"2026-05-31T18:00:53.000Z","level":"INFO","service":"payment-v2","latency_ms":144.0,"http_status":200,"active_users":460,"cpu_util":38.0,"mem_rss_mb":1564.0}
{"timestamp":"2026-05-31T18:00:54.000Z","level":"INFO","service":"payment-v2","latency_ms":146.0,"http_status":200,"active_users":461,"cpu_util":38.5,"mem_rss_mb":1609.0}
{"timestamp":"2026-05-31T18:00:55.000Z","level":"INFO","service":"payment-v2","latency_ms":148.0,"http_status":200,"active_users":462,"cpu_util":39.0,"mem_rss_mb":1654.0}
{"timestamp":"2026-05-31T18:00:56.000Z","level":"INFO","service":"payment-v2","latency_ms":150.0,"http_status":200,"active_users":460,"cpu_util":39.5,"mem_rss_mb":1699.0}
{"timestamp":"2026-05-31T18:00:57.000Z","level":"INFO","service":"payment-v2","latency_ms":152.0,"http_status":200,"active_users":461,"cpu_util":40.0,"mem_rss_mb":1744.0}
{"timestamp":"2026-05-31T18:00:58.000Z","level":"INFO","service":"payment-v2","latency_ms":154.0,"http_status":200,"active_users":462,"cpu_util":40.5,"mem_rss_mb":1789.0}
{"timestamp":"2026-05-31T18:00:59.000Z","level":"WARN","service":"payment-v2","latency_ms":156.0,"http_status":200,"active_users":460,"cpu_util":41.0,"mem_rss_mb":1834.0}
{"timestamp":"2026-05-31T18:01:00.000Z","level":"WARN","service":"payment-v2","latency_ms":158.0,"http_status":200,"active_users":461,"cpu_util":41.5,"mem_rss_mb":1879.0}
{"timestamp":"2026-05-31T18:01:01.000Z","level":"WARN","service":"payment-v2","latency_ms":160.0,"http_status":200,"active_users":462,"cpu_util":42.0,"mem_rss_mb":1924.0}
{"timestamp":"2026-05-31T18:01:02.000Z","level":"WARN","service":"payment-v2","latency_ms":162.0,"http_status":200,"active_users":460,"cpu_util":42.5,"mem_rss_mb":1969.0}
{"timestamp":"2026-05-31T18:01:03.000Z","level":"WARN","service":"payment-v2","latency_ms":164.0,"http_status":200,"active_users":461,"cpu_util":43.0,"mem_rss_mb":2014.0}
{"timestamp":"2026-05-31T18:01:04.000Z","level":"WARN","service":"payment-v2","latency_ms":166.0,"http_status":200,"active_users":462,"cpu_util":43.5,"mem_rss_mb":2059.0}
{"timestamp":"2026-05-31T18:01:05.000Z","level":"WARN","service":"payment-v2","latency_ms":168.0,"http_status":200,"active_users":460,"cpu_util":44.0,"mem_rss_mb":2104.0}
{"timestamp":"2026-05-31T18:01:06.000Z","level":"ERROR","service":"payment-v2","latency_ms":0.0,"http_status":503,"active_users":0,"cpu_util":2.0,"mem_rss_mb":2149.0}
{"timestamp":"2026-05-31T18:01:07.000Z","level":"ERROR","service":"payment-v2","latency_ms":0.0,"http_status":503,"active_users":0,"cpu_util":2.0,"mem_rss_mb":2194.0}
{"timestamp":"2026-05-31T18:01:08.000Z","level":"ERROR","service":"payment-v2","latency_ms":0.0,"http_status":503,"active_users":0,"cpu_util":2.0,"mem_rss_mb":2239.0}
{"timestamp":"2026-05-31T18:01:09.000Z","level":"ERROR","service":"payment-v2","latency_ms":0.0,"http_status":503,"active_users":0,"cpu_util":2.0,"mem_rss_mb":2284.0}
{"timestamp":"2026-05-31T18:01:10.000Z","level":"ERROR","service":"payment-v2","latency_ms":0.0,"http_status":503,"active_users":0,"cpu_util":2.0,"mem_rss_mb":2329.0}
{"timestamp":"2026-05-31T18:01:11.000Z","level":"INFO","service":"mesh-router","latency_ms":5.0,"http_status":200,"active_users":9000,"cpu_util":20.0,"mem_rss_mb":128.0}
{"timestamp":"2026-05-31T18:01:12.000Z","level":"INFO","service":"mesh-router","latency_ms":10.0,"http_status":200,"active_users":9400,"cpu_util":24.5,"mem_rss_mb":129.0}
{"timestamp":"2026-05-31T18:01:13.000Z","level":"INFO","service":"mesh-router","latency_ms":15.0,"http_status":200,"active_users":9800,"cpu_util":29.0,"mem_rss_mb":130.0}
{"timestamp":"2026-05-31T18:01:14.000Z","level":"INFO","service":"mesh-router","latency_ms":20.0,"http_status":200,"active_users":10200,"cpu_util":33.5,"mem_rss_mb":131.0}
{"timestamp":"2026-05-31T18:01:15.000Z","level":"INFO","service":"mesh-router","latency_ms":25.0,"http_status":200,"active_users":10600,"cpu_util":38.0,"mem_rss_mb":132.0}
{"timestamp":"2026-05-31T18:01:16.000Z","level":"INFO","service":"mesh-router","latency_ms":30.0,"http_status":200,"active_users":11000,"cpu_util":42.5,"mem_rss_mb":133.0}
{"timestamp":"2026-05-31T18:01:17.000Z","level":"INFO","service":"mesh-router","latency_ms":35.0,"http_status":200,"active_users":11400,"cpu_util":47.0,"mem_rss_mb":134.0}
{"timestamp":"2026-05-31T18:01:18.000Z","level":"WARN","service":"mesh-router","latency_ms":40.0,"http_status":200,"active_users":11800,"cpu_util":51.5,"mem_rss_mb":135.0}
{"timestamp":"2026-05-31T18:01:19.000Z","level":"WARN","service":"mesh-router","latency_ms":45.0,"http_status":200,"active_users":12200,"cpu_util":56.0,"mem_rss_mb":136.0}
{"timestamp":"2026-05-31T18:01:20.000Z","level":"WARN","service":"mesh-router","latency_ms":50.0,"http_status":200,"active_users":12600,"cpu_util":60.5,"mem_rss_mb":137.0}
{"timestamp":"2026-05-31T18:01:21.000Z","level":"WARN","service":"mesh-router","latency_ms":55.0,"http_status":200,"active_users":13000,"cpu_util":65.0,"mem_rss_mb":138.0}
{"timestamp":"2026-05-31T18:01:22.000Z","level":"WARN","service":"mesh-router","latency_ms":60.0,"http_status":200,"active_users":13400,"cpu_util":69.5,"mem_rss_mb":139.0}
{"timestamp":"2026-05-31T18:01:23.000Z","level":"WARN","service":"mesh-router","latency_ms":65.0,"http_status":200,"active_users":13800,"cpu_util":74.0,"mem_rss_mb":140.0}
{"timestamp":"2026-05-31T18:01:24.000Z","level":"WARN","service":"mesh-router","latency_ms":70.0,"http_status":200,"active_users":14200,"cpu_util":78.5,"mem_rss_mb":141.0}
{"timestamp":"2026-05-31T18:01:25.000Z","level":"FATAL","service":"mesh-router","latency_ms":0.0,"http_status":504,"active_users":0,"cpu_util":83.0,"mem_rss_mb":142.0}
{"timestamp":"2026-05-31T18:01:26.000Z","level":"FATAL","service":"mesh-router","latency_ms":0.0,"http_status":504,"active_users":0,"cpu_util":87.5,"mem_rss_mb":143.0}
{"timestamp":"2026-05-31T18:01:27.000Z","level":"FATAL","service":"mesh-router","latency_ms":0.0,"http_status":504,"active_users":0,"cpu_util":92.0,"mem_rss_mb":144.0}
{"timestamp":"2026-05-31T18:01:28.000Z","level":"FATAL","service":"mesh-router","latency_ms":0.0,"http_status":504,"active_users":0,"cpu_util":96.5,"mem_rss_mb":145.0}
{"timestamp":"2026-05-31T18:01:29.000Z","level":"FATAL","service":"mesh-router","latency_ms":0.0,"http_status":504,"active_users":0,"cpu_util":99.0,"mem_rss_mb":146.0}
{"timestamp":"2026-05-31T18:01:30.000Z","level":"FATAL","service":"mesh-router","latency_ms":0.0,"http_status":504,"active_users":0,"cpu_util":99.0,"mem_rss_mb":147.0}
{"timestamp":"2026-05-31T18:01:31.000Z","level":"INFO","service":"auth-api","latency_ms":25.0,"http_status":200,"active_users":1200,"cpu_util":35.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:01:32.000Z","level":"INFO","service":"payment-v2","latency_ms":115.0,"http_status":200,"active_users":400,"cpu_util":25.0,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:01:33.000Z","level":"INFO","service":"auth-api","latency_ms":25.0,"http_status":200,"active_users":1200,"cpu_util":35.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:01:34.000Z","level":"INFO","service":"payment-v2","latency_ms":115.0,"http_status":200,"active_users":400,"cpu_util":25.0,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:01:35.000Z","level":"INFO","service":"auth-api","latency_ms":25.0,"http_status":200,"active_users":1200,"cpu_util":35.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:01:36.000Z","level":"INFO","service":"payment-v2","latency_ms":115.0,"http_status":200,"active_users":400,"cpu_util":25.0,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:01:37.000Z","level":"INFO","service":"auth-api","latency_ms":25.0,"http_status":200,"active_users":1200,"cpu_util":35.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:01:38.000Z","level":"INFO","service":"payment-v2","latency_ms":115.0,"http_status":200,"active_users":400,"cpu_util":25.0,"mem_rss_mb":1024.0}
{"timestamp":"2026-05-31T18:01:39.000Z","level":"INFO","service":"auth-api","latency_ms":25.0,"http_status":200,"active_users":1200,"cpu_util":35.0,"mem_rss_mb":512.0}
{"timestamp":"2026-05-31T18:01:40.000Z","level":"INFO","service":"payment-v2","latency_ms":115.0,"http_status":200,"active_users":400,"cpu_util":25.0,"mem_rss_mb":1024.0}`;

type ClusterPoint = {
  x: number;
  y: number;
  label: number;
  isNoise: boolean;
  outlier: boolean;
};

type Result = {
  jobId: string;
  status: string;
  rows?: number;
  clusters?: number;
  entropy?: number;
  trendSlope?: number;
  outlierRatio?: number;
  schemaDrift?: number;
  svg?: string;
  html?: string;
  error?: string;
  algorithm?: string;
  points?: ClusterPoint[];
};

const ALL_ALGORITHMS = [
  "dbscan",
  "kmeans++",
  "birch",
  "mean_shift",
  "optics",
  "gmm",
  "agglomerative",
] as const;

function scoreResult(r: Result): number {
  const clusters = r.clusters ?? 0;
  const entropy = r.entropy ?? Infinity;
  const rows = r.rows ?? 0;
  if (clusters <= 1) return -1000;
  if (clusters > rows / 2 && rows > 0) return -500;
  let score = 0;
  if (clusters >= 2 && clusters <= 6) score += 30;
  else if (clusters <= 10) score += 20;
  else score += 10;
  score -= entropy * 3;
  return score;
}

function formatCompactNumber(value: number) {
  return new Intl.NumberFormat("en-US", { notation: "compact", maximumFractionDigits: 1 }).format(value);
}

function escapeXml(value: string) {
  return value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&apos;");
}

/* ─────────── SVG exporters ─────────── */

const DEFAULT_EXPORT_SIZE = { width: 960, height: 560 };

function parseSvgLength(value: string | null) {
  if (!value) return null;
  const trimmed = value.trim();
  if (!trimmed || trimmed.endsWith("%")) return null;

  const match = trimmed.match(/^(\d+(?:\.\d+)?|\.\d+)/);
  if (!match) return null;

  const parsed = Number(match[1]);
  return Number.isFinite(parsed) && parsed > 0 ? parsed : null;
}

function parseSvgViewBox(value: string | null) {
  if (!value) return null;

  const parts = value
    .trim()
    .split(/[\s,]+/)
    .map((part) => Number(part));

  if (parts.length !== 4 || parts.some((part) => !Number.isFinite(part))) return null;

  const [, , width, height] = parts;
  return width > 0 && height > 0 ? { width, height } : null;
}

function readSvgExportSize(markup: string) {
  try {
    const doc = new DOMParser().parseFromString(markup, "image/svg+xml");
    const svg = doc.documentElement;
    if (svg.tagName.toLowerCase() !== "svg") return DEFAULT_EXPORT_SIZE;

    const width = parseSvgLength(svg.getAttribute("width"));
    const height = parseSvgLength(svg.getAttribute("height"));
    const viewBox = parseSvgViewBox(svg.getAttribute("viewBox"));

    return {
      width: Math.round(width ?? viewBox?.width ?? DEFAULT_EXPORT_SIZE.width),
      height: Math.round(height ?? viewBox?.height ?? DEFAULT_EXPORT_SIZE.height),
    };
  } catch {
    return DEFAULT_EXPORT_SIZE;
  }
}

function normalizeSvgForExport(markup: string, size: { width: number; height: number }) {
  try {
    const doc = new DOMParser().parseFromString(markup, "image/svg+xml");
    const svg = doc.documentElement;
    if (svg.tagName.toLowerCase() !== "svg") return markup;

    svg.setAttribute("width", String(size.width));
    svg.setAttribute("height", String(size.height));
    if (!svg.getAttribute("viewBox")) {
      svg.setAttribute("viewBox", `0 0 ${size.width} ${size.height}`);
    }

    return new XMLSerializer().serializeToString(svg);
  } catch {
    return markup;
  }
}

function histogramSvgMarkup(bars: HistogramBar[], title: string, subtitle: string) {
  const width = 960;
  const height = 560;
  const left = 88;
  const right = 42;
  const top = 84;
  const bottom = 84;
  const max = Math.max(...bars.map((bar) => bar.value), 1);
  const innerWidth = width - left - right;
  const innerHeight = height - top - bottom;
  const band = innerWidth / Math.max(bars.length, 1);
  const barWidth = Math.max(28, band * 0.62);

  const rects = bars
    .map((bar, index) => {
      const barHeight = (bar.value / max) * innerHeight;
      const x = left + index * band + (band - barWidth) / 2;
      const y = top + innerHeight - barHeight;
      return `
        <g>
          <rect x="${x}" y="${y}" width="${barWidth}" height="${barHeight}" rx="16" fill="url(#barGradient)" />
          <text x="${x + barWidth / 2}" y="${y - 12}" text-anchor="middle" fill="#c4b5fd" font-size="20" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(String(bar.value))}</text>
          <text x="${x + barWidth / 2}" y="${height - 34}" text-anchor="middle" fill="#6b6398" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(bar.label)}</text>
        </g>`;
    })
    .join("");

  return `
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}">
      <defs>
        <linearGradient id="bgGradient" x1="0" x2="1" y1="0" y2="1">
          <stop offset="0%" stop-color="#0f0c1a" />
          <stop offset="50%" stop-color="#1a1430" />
          <stop offset="100%" stop-color="#0d0a18" />
        </linearGradient>
        <linearGradient id="barGradient" x1="0" x2="0" y1="0" y2="1">
          <stop offset="0%" stop-color="#a78bfa" />
          <stop offset="100%" stop-color="#f472b6" />
        </linearGradient>
      </defs>
      <rect width="${width}" height="${height}" rx="34" fill="url(#bgGradient)" />
      <text x="${left}" y="48" fill="#f0eaff" font-size="28" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(title)}</text>
      <text x="${left}" y="74" fill="#9b92c4" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(subtitle)}</text>
      <line x1="${left}" y1="${top + innerHeight}" x2="${width - right}" y2="${top + innerHeight}" stroke="#3a3050" stroke-width="2" />
      ${rects}
    </svg>`;
}

function lineSvgMarkup(series: SeriesChart) {
  const width = 960;
  const height = 560;
  const left = 86;
  const right = 46;
  const top = 82;
  const bottom = 72;
  const innerWidth = width - left - right;
  const innerHeight = height - top - bottom;
  const values = series.points.map((point) => point.value);
  const min = Math.min(...values);
  const max = Math.max(...values);
  const span = max - min || 1;

  const points = series.points.map((point, index) => {
    const x = left + (innerWidth * index) / Math.max(series.points.length - 1, 1);
    const y = top + innerHeight - ((point.value - min) / span) * innerHeight;
    return { ...point, x, y };
  });

  const path = points.map((point, index) => `${index === 0 ? "M" : "L"} ${point.x.toFixed(2)} ${point.y.toFixed(2)}`).join(" ");

  const guides = [0, 0.5, 1]
    .map((ratio) => {
      const y = top + innerHeight - ratio * innerHeight;
      const value = min + ratio * span;
      return `
        <g>
          <line x1="${left}" y1="${y}" x2="${width - right}" y2="${y}" stroke="#3b322c" stroke-width="1.5" stroke-dasharray="6 10" />
          <text x="24" y="${y + 6}" fill="#7c75a8" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(value.toFixed(0))}</text>
        </g>`;
    })
    .join("");

  const dots = points
    .map(
      (point) => `
        <g>
          <circle cx="${point.x}" cy="${point.y}" r="6" fill="${series.color}" />
        </g>`,
    )
    .join("");

  return `
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}">
      <defs>
        <linearGradient id="bgGradient" x1="0" x2="1" y1="0" y2="1">
          <stop offset="0%" stop-color="#0f0c1a" />
          <stop offset="50%" stop-color="#1a1430" />
          <stop offset="100%" stop-color="#0d0a18" />
        </linearGradient>
      </defs>
      <rect width="${width}" height="${height}" rx="34" fill="url(#bgGradient)" />
      <text x="${left}" y="48" fill="#f0eaff" font-size="28" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(series.key)}</text>
      <text x="${left}" y="74" fill="#9b92c4" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">timestamp trend</text>
      ${guides}
      <line x1="${left}" y1="${top + innerHeight}" x2="${width - right}" y2="${top + innerHeight}" stroke="#3a3050" stroke-width="2" />
      <path d="${path}" fill="none" stroke="${series.color}" stroke-width="5" stroke-linecap="round" stroke-linejoin="round" />
      ${dots}
    </svg>`;
}

function scatterSvgMarkup(scatter: ScatterSeries) {
  const width = 960;
  const height = 560;
  const left = 86;
  const right = 46;
  const top = 82;
  const bottom = 72;
  const innerWidth = width - left - right;
  const innerHeight = height - top - bottom;
  const xs = scatter.points.map((p) => p.x);
  const ys = scatter.points.map((p) => p.y);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);
  const spanX = maxX - minX || 1;
  const spanY = maxY - minY || 1;

  const dots = scatter.points
    .map((p) => {
      const x = left + ((p.x - minX) / spanX) * innerWidth;
      const y = top + innerHeight - ((p.y - minY) / spanY) * innerHeight;
      return `<circle cx="${x.toFixed(2)}" cy="${y.toFixed(2)}" r="5" fill="${scatter.color}" opacity="0.85" />`;
    })
    .join("");

  const xGuides = [0, 0.5, 1]
    .map((ratio) => {
      const x = left + ratio * innerWidth;
      const value = minX + ratio * spanX;
      return `<text x="${x}" y="${height - 28}" text-anchor="middle" fill="#6b6398" font-size="16" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(value.toFixed(1))}</text>`;
    })
    .join("");

  const yGuides = [0, 0.5, 1]
    .map((ratio) => {
      const y = top + innerHeight - ratio * innerHeight;
      const value = minY + ratio * spanY;
      return `<text x="18" y="${y + 5}" fill="#6b6398" font-size="16" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(value.toFixed(1))}</text>`;
    })
    .join("");

  return `
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}">
      <defs>
        <linearGradient id="bgGradient" x1="0" x2="1" y1="0" y2="1">
          <stop offset="0%" stop-color="#0f0c1a" />
          <stop offset="50%" stop-color="#1a1430" />
          <stop offset="100%" stop-color="#0d0a18" />
        </linearGradient>
      </defs>
      <rect width="${width}" height="${height}" rx="34" fill="url(#bgGradient)" />
      <text x="${left}" y="48" fill="#f0eaff" font-size="28" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(scatter.key)}</text>
      <text x="${left}" y="74" fill="#9b92c4" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">Scatter plot</text>
      <line x1="${left}" y1="${top + innerHeight}" x2="${width - right}" y2="${top + innerHeight}" stroke="#3a3050" stroke-width="2" />
      <line x1="${left}" y1="${top}" x2="${left}" y2="${top + innerHeight}" stroke="#3a3050" stroke-width="2" />
      ${xGuides}
      ${yGuides}
      ${dots}
    </svg>`;
}

function wrapText(text: string, maxChars: number): string[] {
  if (text.length <= maxChars) return [text];
  const lines: string[] = [];
  let remaining = text;
  while (remaining.length > 0) {
    let breakIdx = maxChars;
    if (remaining.length > maxChars) {
      const window = remaining.slice(0, maxChars + 1);
      const underscore = window.lastIndexOf("_");
      const hyphen = window.lastIndexOf("-");
      const dot = window.lastIndexOf(".");
      const slash = window.lastIndexOf("/");
      const candidates = [underscore, hyphen, dot, slash].filter((i) => i > 1 && i < maxChars);
      if (candidates.length > 0) {
        breakIdx = Math.max(...candidates) + 1;
      } else {
        // Try camelCase break
        for (let k = maxChars; k > 1; k--) {
          if (/[a-z][A-Z]/.test(window.slice(k - 1, k + 1))) {
            breakIdx = k;
            break;
          }
        }
      }
    }
    lines.push(remaining.slice(0, breakIdx));
    remaining = remaining.slice(breakIdx);
  }
  return lines;
}

function heatmapSvgMarkup(cells: CorrelationCell[], title: string) {
  const width = 960;
  const height = 560;
  const right = 60;
  const bottom = 60;
  const padding = 14; // padding between labels and grid

  const keys = Array.from(new Set(cells.flatMap((c) => [c.x, c.y]))).sort();
  if (keys.length === 0) return "";

  const maxLabelLen = Math.max(...keys.map((k) => k.length), 1);

  // ── Pass 1: estimate margins so we can compute cellSize ──
  let left = Math.max(150, maxLabelLen * 7 + padding + 16);
  let top = Math.max(95, keys.length > 8 ? 125 : 95);

  const cellSize = Math.min(
    (width - left - right) / keys.length,
    (height - top - bottom) / keys.length,
    80,
  );

  const labelFontSize = Math.max(7, Math.min(10, cellSize * 0.15));
  const charWidth = labelFontSize * 0.58; // approx monospace char width
  const lineHeight = labelFontSize * 1.25;

  // ── Column labels: wrap to fit inside cell width ──
  const colMaxChars = Math.max(3, Math.floor((cellSize - padding * 2) / charWidth));
  const colWrapped = keys.map((k) => wrapText(k, colMaxChars));
  const maxColLines = Math.max(1, ...colWrapped.map((l) => l.length));

  // ── Row labels: wrap to fit inside left margin width ──
  const rowMaxChars = Math.max(3, Math.floor((left - padding - 10) / charWidth));
  const rowWrapped = keys.map((k) => wrapText(k, rowMaxChars));
  const maxRowLineLen = Math.max(
    1,
    ...rowWrapped.flatMap((l) => l.map((s) => s.length)),
  );
  const maxRowLines = Math.max(1, ...rowWrapped.map((l) => l.length));

  // ── Adjust margins based on actual wrapped text dimensions ──
  top = Math.max(85, maxColLines * lineHeight + padding + 30);
  left = Math.max(
    140,
    maxRowLineLen * charWidth + padding + 16,
    maxRowLines * lineHeight + 10,
  );

  // ── Recompute cellSize with final margins ──
  const finalCellSize = Math.min(
    (width - left - right) / keys.length,
    (height - top - bottom) / keys.length,
    80,
  );

  // ── Value font size: auto-scale so "-0.99" fits inside the cell ──
  const rawValueFontSize = Math.max(7, Math.min(10, finalCellSize * 0.15));
  const valueCharWidth = rawValueFontSize * 0.58;
  const typicalValue = "-0.99";
  const valueFontSize =
    typicalValue.length * valueCharWidth > finalCellSize - 10
      ? Math.max(7, (finalCellSize - 10) / typicalValue.length / 0.58)
      : rawValueFontSize;

  const colorFor = (v: number) => {
    const t = Math.max(0, Math.min(1, Math.abs(v)));
    if (v >= 0) {
      const r = Math.round(34 + t * (59 - 34));
      const g = Math.round(211 + t * (130 - 211));
      const b = Math.round(238 + t * (246 - 238));
      return `rgb(${r},${g},${b})`;
    }
    const r = Math.round(244 + t * (167 - 244));
    const g = Math.round(114 + t * (139 - 114));
    const b = Math.round(182 + t * (250 - 182));
    return `rgb(${r},${g},${b})`;
  };

  const rects: string[] = [];
  for (let i = 0; i < keys.length; i++) {
    for (let j = 0; j < keys.length; j++) {
      const x = left + j * finalCellSize;
      const y = top + i * finalCellSize;
      if (i === j) {
        rects.push(
          `<rect x="${x}" y="${y}" width="${finalCellSize}" height="${finalCellSize}" fill="#2a1f1b" rx="6" />`,
        );
        rects.push(
          `<text x="${x + finalCellSize / 2}" y="${y + finalCellSize / 2 + 5}" text-anchor="middle" fill="#8d7561" font-size="${valueFontSize}" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">1</text>`,
        );
        continue;
      }
      const cell = cells.find(
        (c) =>
          (c.x === keys[i] && c.y === keys[j]) ||
          (c.x === keys[j] && c.y === keys[i]),
      );
      const value = cell?.value ?? 0;
      rects.push(
        `<rect x="${x + 1}" y="${y + 1}" width="${finalCellSize - 2}" height="${finalCellSize - 2}" fill="${colorFor(value)}" rx="6" opacity="0.9" />`,
      );
      rects.push(
        `<text x="${x + finalCellSize / 2}" y="${y + finalCellSize / 2 + 5}" text-anchor="middle" fill="#fff" font-size="${valueFontSize}" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${value.toFixed(2)}</text>`,
      );
    }
  }

  // ── Column labels: horizontal, multi-line, centered with padding ──
  const colLabels = keys
    .map((key, i) => {
      const x = left + i * finalCellSize + finalCellSize / 2;
      const lines = colWrapped[i];
      const startY = top - padding - (lines.length - 1) * lineHeight;
      const tspans = lines
        .map(
          (line, idx) =>
            `<tspan x="${x}" dy="${idx === 0 ? startY : lineHeight}">${escapeXml(line)}</tspan>`,
        )
        .join("");
      return `<text text-anchor="middle" fill="#b69774" font-size="${labelFontSize}" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${tspans}</text>`;
    })
    .join("");

  // ── Row labels: multi-line, right-aligned with padding ──
  const rowLabels = keys
    .map((key, i) => {
      const y = top + i * finalCellSize + finalCellSize / 2;
      const lines = rowWrapped[i];
      const startY = y - ((lines.length - 1) * lineHeight) / 2 + 4;
      const tspans = lines
        .map(
          (line, idx) =>
            `<tspan x="${left - padding}" dy="${idx === 0 ? startY : lineHeight}">${escapeXml(line)}</tspan>`,
        )
        .join("");
      return `<text text-anchor="end" fill="#b69774" font-size="${labelFontSize}" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${tspans}</text>`;
    })
    .join("");

  return `
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}">
      <defs>
        <linearGradient id="bgGradient" x1="0" x2="1" y1="0" y2="1">
          <stop offset="0%" stop-color="#0f0c1a" />
          <stop offset="50%" stop-color="#1a1430" />
          <stop offset="100%" stop-color="#0d0a18" />
        </linearGradient>
      </defs>
      <rect width="${width}" height="${height}" rx="34" fill="url(#bgGradient)" />
      <text x="${left}" y="48" fill="#f0eaff" font-size="28" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(title)}</text>
      <text x="${left}" y="74" fill="#9b92c4" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">Numeric correlation matrix</text>
      ${rects.join("")}
      ${colLabels}
      ${rowLabels}
    </svg>`;
}

function donutSvgMarkup(bars: HistogramBar[], title: string, subtitle: string) {
  const width = 960;
  const height = 560;
  const cx = 480;
  const cy = 280;
  const radius = 160;
  const innerRadius = 100;
  const total = Math.max(bars.reduce((s, b) => s + b.value, 0), 1);
  const colors = ["#22d3ee", "#a78bfa", "#f472b6", "#3b82f6", "#34d399", "#fbbf24", "#fb7185", "#60a5fa"];

  let startAngle = 0;
  const slices = bars.map((bar, i) => {
    const angle = (bar.value / total) * Math.PI * 2;
    const endAngle = startAngle + angle;
    const x1 = cx + radius * Math.cos(startAngle);
    const y1 = cy + radius * Math.sin(startAngle);
    const x2 = cx + radius * Math.cos(endAngle);
    const y2 = cy + radius * Math.sin(endAngle);
    const x1i = cx + innerRadius * Math.cos(startAngle);
    const y1i = cy + innerRadius * Math.sin(startAngle);
    const x2i = cx + innerRadius * Math.cos(endAngle);
    const y2i = cy + innerRadius * Math.sin(endAngle);
    const largeArc = angle > Math.PI ? 1 : 0;
    const d = `M ${x1i} ${y1i} L ${x1} ${y1} A ${radius} ${radius} 0 ${largeArc} 1 ${x2} ${y2} L ${x2i} ${y2i} A ${innerRadius} ${innerRadius} 0 ${largeArc} 0 ${x1i} ${y1i}`;
    const midAngle = startAngle + angle / 2;
    const lx = cx + (radius + 30) * Math.cos(midAngle);
    const ly = cy + (radius + 30) * Math.sin(midAngle);
    startAngle = endAngle;
    return { d, color: colors[i % colors.length], label: bar.label, value: bar.value, lx, ly, midAngle };
  });

  const paths = slices
    .map(
      (s) => `
        <path d="${s.d}" fill="${s.color}" opacity="0.92" />
        <line x1="${cx + radius * Math.cos(s.midAngle)}" y1="${cy + radius * Math.sin(s.midAngle)}" x2="${s.lx}" y2="${s.ly}" stroke="#4b3a30" stroke-width="1.5" />
        <text x="${s.lx + (Math.cos(s.midAngle) > 0 ? 8 : -8)}" y="${s.ly + 5}" text-anchor="${Math.cos(s.midAngle) > 0 ? "start" : "end"}" fill="#b69774" font-size="16" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(s.label)} ${escapeXml(String(s.value))}</text>
      `,
    )
    .join("");

  return `
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}">
      <defs>
        <linearGradient id="bgGradient" x1="0" x2="1" y1="0" y2="1">
          <stop offset="0%" stop-color="#0f0c1a" />
          <stop offset="50%" stop-color="#1a1430" />
          <stop offset="100%" stop-color="#0d0a18" />
        </linearGradient>
      </defs>
      <rect width="${width}" height="${height}" rx="34" fill="url(#bgGradient)" />
      <text x="80" y="48" fill="#f0eaff" font-size="28" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(title)}</text>
      <text x="80" y="74" fill="#9b92c4" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(subtitle)}</text>
      ${paths}
    </svg>`;
}

async function downloadSvgAsPng(markup: string, name: string) {
  const exportSize = readSvgExportSize(markup);
  const exportMarkup = normalizeSvgForExport(markup, exportSize);
  const blob = new Blob([exportMarkup], { type: "image/svg+xml;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  try {
    const image = await new Promise<HTMLImageElement>((resolve, reject) => {
      const img = new Image();
      img.onload = () => resolve(img);
      img.onerror = () => reject(new Error("image load failed"));
      img.src = url;
    });
    const canvas = document.createElement("canvas");
    canvas.width = exportSize.width;
    canvas.height = exportSize.height;
    const context = canvas.getContext("2d");
    if (!context) throw new Error("canvas context unavailable");
    context.drawImage(image, 0, 0, exportSize.width, exportSize.height);
    const pngUrl = canvas.toDataURL("image/png");
    const link = document.createElement("a");
    link.href = pngUrl;
    link.download = `${name}.png`;
    link.click();
  } finally {
    URL.revokeObjectURL(url);
  }
}

/* ─────────── Animation hooks ─────────── */

function useCountUp(target: number, duration = 800) {
  const [value, setValue] = useState(0);
  const startRef = useRef<number | null>(null);
  const fromRef = useRef(0);

  useEffect(() => {
    fromRef.current = value;
    startRef.current = null;
    let raf: number;

    const tick = (now: number) => {
      if (startRef.current === null) startRef.current = now;
      const elapsed = now - startRef.current;
      const progress = Math.min(elapsed / duration, 1);
      const eased = progress === 1 ? 1 : 1 - Math.pow(2, -10 * progress);
      setValue(fromRef.current + (target - fromRef.current) * eased);
      if (progress < 1) raf = requestAnimationFrame(tick);
    };

    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [target, duration]);

  return value;
}

function useInView<T extends HTMLElement>(ref: React.RefObject<T | null>, threshold = 0.1) {
  const [inView, setInView] = useState(false);
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const observer = new IntersectionObserver(
      ([entry]) => {
        if (entry.isIntersecting) {
          setInView(true);
          observer.disconnect();
        }
      },
      { threshold }
    );
    observer.observe(el);
    return () => observer.disconnect();
  }, [ref, threshold]);
  return inView;
}

function AnimatedValue({ value, decimals = 0 }: { value: number; decimals?: number }) {
  const animated = useCountUp(value, 900);
  return <>{animated.toFixed(decimals)}</>;
}

function FitText({
  children,
  className,
  style,
  minFontSize = 11,
}: {
  children: React.ReactNode;
  className?: string;
  style?: React.CSSProperties;
  minFontSize?: number;
}) {
  const ref = useRef<HTMLSpanElement>(null);
  const [ready, setReady] = useState(false);

  useLayoutEffect(() => {
    const el = ref.current;
    if (!el) return;
    const parent = el.parentElement;
    if (!parent) return;

    el.style.fontSize = "";
    el.style.whiteSpace = "nowrap";
    const computedSize = parseFloat(getComputedStyle(el).fontSize);
    let size = computedSize;

    if (el.scrollWidth > parent.clientWidth && parent.clientWidth > 0) {
      while (el.scrollWidth > parent.clientWidth && size > minFontSize) {
        size -= 0.5;
        el.style.fontSize = `${size}px`;
      }
      if (el.scrollWidth > parent.clientWidth) {
        // Even at min size it still overflows — allow wrapping and restore original size
        el.style.whiteSpace = "normal";
        el.style.fontSize = `${computedSize}px`;
      }
    }
    setReady(true);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [children, minFontSize]);

  return (
    <span
      ref={ref}
      className={className}
      style={{
        display: "inline-block",
        visibility: ready ? "visible" : "hidden",
        ...style,
      }}
    >
      {children}
    </span>
  );
}

/* ─────────── React components ─────────── */

type WindowKey = "input" | "runState" | "analysis" | "schema" | "rules";

function AnimatedWindow({
  show,
  className = "",
  children,
  duration = 350,
}: {
  show: boolean;
  className?: string;
  children: React.ReactNode;
  duration?: number;
}) {
  const [mounted, setMounted] = useState(show);
  const [closing, setClosing] = useState(false);

  useEffect(() => {
    if (show) {
      setClosing(false);
      setMounted(true);
    } else {
      setClosing(true);
      const timer = setTimeout(() => setMounted(false), duration);
      return () => clearTimeout(timer);
    }
  }, [show, duration]);

  if (!mounted) return null;

  return (
    <div className={`${className} ${closing ? "is-closing" : ""}`}>
      {children}
    </div>
  );
}

function Chrome({
  onClose,
  onMinimize,
  onFullscreen,
}: {
  onClose?: () => void;
  onMinimize?: () => void;
  onFullscreen?: () => void;
}) {
  return (
    <div className="chrome">
      <span
        className="dot dot-red"
        onClick={onClose}
        style={{ cursor: onClose ? "pointer" : "default" }}
        title={onClose ? "Close" : undefined}
      />
      <span
        className="dot dot-yellow"
        onClick={onMinimize}
        style={{ cursor: onMinimize ? "pointer" : "default" }}
        title={onMinimize ? "Minimize" : undefined}
      />
      <span
        className="dot dot-green"
        onClick={onFullscreen}
        style={{ cursor: onFullscreen ? "pointer" : "default" }}
        title={onFullscreen ? "Fullscreen" : undefined}
      />
    </div>
  );
}

function WindowShell({
  show,
  onClose,
  className = "window",
  children,
}: {
  show: boolean;
  onClose: () => void;
  className?: string;
  children: React.ReactNode;
}) {
  const [minimized, setMinimized] = useState(false);
  const [fullscreen, setFullscreen] = useState(false);
  const [shaking, setShaking] = useState(false);

  const shake = () => {
    setShaking(true);
    setTimeout(() => setShaking(false), 350);
  };

  const handleClose = () => {
    shake();
    onClose();
  };

  const childArray = useMemo(() => Children.toArray(children), [children]);
  const headChild = childArray.find(
    (c) => isValidElement(c) && String((c as any).props.className).includes("window-head"),
  );

  return (
    <AnimatedWindow
      show={show}
      className={`${className} ${shaking ? "is-shaking" : ""} ${minimized ? "is-minimized" : ""} ${fullscreen ? "is-fullscreen" : ""}`}
    >
      <Chrome
        onClose={handleClose}
        onMinimize={() => {
          shake();
          setMinimized((v) => !v);
        }}
        onFullscreen={() => {
          shake();
          setFullscreen((v) => !v);
        }}
      />
      {minimized ? (
        <div className="window-minimized-head">{headChild}</div>
      ) : (
        <div className="window-body-wrap">{children}</div>
      )}
    </AnimatedWindow>
  );
}

function Dock({
  windows,
  onToggle,
}: {
  windows: Record<WindowKey, boolean>;
  onToggle: (key: WindowKey) => void;
}) {
  const items: { key: WindowKey; label: string }[] = [
    { key: "input", label: "Input" },
    { key: "runState", label: "State" },
    { key: "analysis", label: "Output" },
    { key: "schema", label: "Schema" },
    { key: "rules", label: "Rules" },
  ];

  return (
    <nav className="dock">
      {items.map((item) => (
        <button
          key={item.key}
          className={`dock-item ${windows[item.key] ? "dock-active" : ""}`}
          onClick={() => onToggle(item.key)}
          title={item.label}
        >
          <span className="dock-dot" />
          <span className="dock-label">{item.label}</span>
        </button>
      ))}
    </nav>
  );
}

function ChartCard({
  title,
  subtitle,
  svgMarkup,
  children,
  downloadName,
}: {
  title: string;
  subtitle: string;
  svgMarkup: string;
  children: React.ReactNode;
  downloadName: string;
}) {
  const chartId = useId();
  return (
    <article className="viz-card">
      <div className="viz-head">
        <div>
          <h3>{title}</h3>
          <p>{subtitle}</p>
        </div>
        <button className="ghost-button" onClick={() => downloadSvgAsPng(svgMarkup, downloadName)}>
          Download PNG
        </button>
      </div>
      <div className="chart" aria-labelledby={chartId}>
        <div className="chart-svg" id={chartId}>
          {children}
        </div>
      </div>
    </article>
  );
}

function HistogramChart({ bars }: { bars: HistogramBar[] }) {
  const max = Math.max(...bars.map((bar) => bar.value), 1);
  const [mounted, setMounted] = useState(false);
  useEffect(() => {
    const t = setTimeout(() => setMounted(true), 50);
    return () => clearTimeout(t);
  }, [bars]);

  return (
    <div className="histogram">
      {bars.map((bar, i) => (
        <div className="histogram-bar" key={bar.label} style={{ opacity: mounted ? 1 : 0, transition: `opacity 0.4s ease ${i * 40}ms` }}>
          <span>{bar.label}</span>
          <div className="histogram-track">
            <div className="histogram-fill" style={{ width: mounted ? `${(bar.value / max) * 100}%` : "0%" }} />
          </div>
          <strong><FitText minFontSize={12}>{bar.value}</FitText></strong>
        </div>
      ))}
    </div>
  );
}

function LineChart({ series }: { series: SeriesChart }) {
  const pathRef = useRef<SVGPathElement | null>(null);
  const width = 700;
  const height = 240;
  const left = 16;
  const right = 16;
  const top = 16;
  const bottom = 26;
  const innerWidth = width - left - right;
  const innerHeight = height - top - bottom;
  const range = series.max - series.min || 1;

  const points = series.points.map((point, index) => {
    const x = left + (innerWidth * index) / Math.max(series.points.length - 1, 1);
    const y = top + innerHeight - ((point.value - series.min) / range) * innerHeight;
    return { ...point, x, y };
  });

  const path = points.map((point, index) => `${index === 0 ? "M" : "L"} ${point.x} ${point.y}`).join(" ");

  useEffect(() => {
    const el = pathRef.current;
    if (!el) return;
    const len = el.getTotalLength();
    el.style.strokeDasharray = `${len}`;
    el.style.strokeDashoffset = `${len}`;
    el.getBoundingClientRect();
    el.style.transition = "stroke-dashoffset 1.2s cubic-bezier(0.16, 1, 0.3, 1)";
    el.style.strokeDashoffset = "0";
  }, [path]);

  return (
    <svg viewBox={`0 0 ${width} ${height}`} role="img" aria-label={`${series.key} line chart`}>
      <path className="chart-grid" d={`M ${left} ${top + innerHeight} H ${width - right}`} />
      <path ref={pathRef} d={path} fill="none" stroke={series.color} strokeWidth="3.5" strokeLinecap="round" strokeLinejoin="round" />
      {points.map((point, i) => (
        <circle className="chart-point" key={`${series.key}-${point.label}`} cx={point.x} cy={point.y} r="3.5" fill={series.color} style={{ transitionDelay: `${i * 25}ms` }} />
      ))}
    </svg>
  );
}

function ScatterChart({ scatter }: { scatter: ScatterSeries }) {
  const width = 700;
  const height = 280;
  const left = 44;
  const right = 16;
  const top = 16;
  const bottom = 36;
  const innerWidth = width - left - right;
  const innerHeight = height - top - bottom;
  const xs = scatter.points.map((p) => p.x);
  const ys = scatter.points.map((p) => p.y);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);
  const spanX = maxX - minX || 1;
  const spanY = maxY - minY || 1;

  const points = scatter.points.map((p) => ({
    x: left + ((p.x - minX) / spanX) * innerWidth,
    y: top + innerHeight - ((p.y - minY) / spanY) * innerHeight,
  }));

  return (
    <svg viewBox={`0 0 ${width} ${height}`} role="img" aria-label={`${scatter.key} scatter`}>
      <text x={left} y={top - 2} fill="#9b92c4" fontSize="12" fontFamily="ui-monospace, monospace">
        {scatter.yKey}
      </text>
      <text x={width - right} y={height - 8} textAnchor="end" fill="#9b92c4" fontSize="12" fontFamily="ui-monospace, monospace">
        {scatter.xKey}
      </text>
      <line x1={left} y1={top + innerHeight} x2={width - right} y2={top + innerHeight} stroke="#3a3050" strokeWidth="1.5" />
      <line x1={left} y1={top} x2={left} y2={top + innerHeight} stroke="#3a3050" strokeWidth="1.5" />
      {points.map((p, i) => (
        <circle className="scatter-point" key={i} cx={p.x} cy={p.y} r="3.5" fill={scatter.color} opacity="0.85" />
      ))}
    </svg>
  );
}

function InteractiveClusterChart({
  points,
  svgMarkup,
  algorithm,
}: {
  points: ClusterPoint[];
  svgMarkup?: string | null;
  algorithm?: string;
}) {
  const [activeTab, setActiveTab] = useState<"interactive" | "engine">("interactive");
  const [hovered, setHovered] = useState<ClusterPoint | null>(null);
  const [mousePos, setMousePos] = useState({ x: 0, y: 0 });
  const [filter, setFilter] = useState<number | null>(null);
  const containerRef = useRef<HTMLDivElement>(null);

  const colors = ["#22d3ee", "#a78bfa", "#f472b6", "#3b82f6", "#34d399", "#fbbf24", "#fb7185", "#60a5fa"];
  const clusterLabels = Array.from(new Set(points.filter((p) => !p.isNoise).map((p) => p.label))).sort((a, b) => a - b);

  const width = 960;
  const height = 560;
  const left = 80;
  const right = 160;
  const top = 60;
  const bottom = 60;
  const innerWidth = width - left - right;
  const innerHeight = height - top - bottom;

  const xs = points.map((p) => p.x);
  const ys = points.map((p) => p.y);
  const minX = Math.min(...xs);
  const maxX = Math.max(...xs);
  const minY = Math.min(...ys);
  const maxY = Math.max(...ys);
  const spanX = maxX - minX || 1;
  const spanY = maxY - minY || 1;

  const projected = points.map((p) => ({
    ...p,
    px: left + ((p.x - minX) / spanX) * innerWidth,
    py: top + innerHeight - ((p.y - minY) / spanY) * innerHeight,
  }));

  return (
    <article className="viz-card" style={{ padding: 0 }}>
      <div className="viz-head" style={{ padding: "20px 24px 0" }}>
        <div>
          <h3>Clustered Stream Preview</h3>
          <p>{algorithm ? `${algorithm} · ` : ""}{points.length} points · {clusterLabels.length} clusters</p>
        </div>
        <div style={{ display: "flex", gap: 8 }}>
          <button
            className={`ghost-button ${activeTab === "interactive" ? "dock-active" : ""}`}
            onClick={() => setActiveTab("interactive")}
          >
            Interactive
          </button>
          {svgMarkup ? (
            <button
              className={`ghost-button ${activeTab === "engine" ? "dock-active" : ""}`}
              onClick={() => setActiveTab("engine")}
            >
              Backend Render
            </button>
          ) : null}
        </div>
      </div>

      {activeTab === "interactive" ? (
        <>
          <div style={{ display: "flex", gap: 8, padding: "12px 24px", flexWrap: "wrap", alignItems: "center" }}>
            <button
              className={`ghost-button ${filter === null ? "dock-active" : ""}`}
              onClick={() => setFilter(null)}
              style={{ fontSize: 13 }}
            >
              All
            </button>
            {clusterLabels.map((label) => (
              <button
                key={label}
                className={`ghost-button ${filter === label ? "dock-active" : ""}`}
                onClick={() => setFilter(label === filter ? null : label)}
                style={{ fontSize: 13 }}
              >
                <span
                  style={{
                    display: "inline-block",
                    width: 10,
                    height: 10,
                    borderRadius: "50%",
                    background: colors[label % colors.length],
                    marginRight: 6,
                  }}
                />
                Cluster {label}
              </button>
            ))}
          </div>
          <div ref={containerRef} style={{ position: "relative", width: "100%", height: 480, overflow: "hidden" }}>
            <svg viewBox={`0 0 ${width} ${height}`} style={{ width: "100%", height: "100%" }}>
              <defs>
                <linearGradient id="bgGradient" x1="0" x2="1" y1="0" y2="1">
                  <stop offset="0%" stop-color="#0f0c1a" />
                  <stop offset="50%" stop-color="#1a1430" />
                  <stop offset="100%" stop-color="#0d0a18" />
                </linearGradient>
              </defs>
              <rect width={width} height={height} rx={34} fill="url(#bgGradient)" />
              {Array.from({ length: 10 }).map((_, i) => (
                <line
                  key={`gx-${i}`}
                  x1={left + (innerWidth * i) / 9}
                  y1={top}
                  x2={left + (innerWidth * i) / 9}
                  y2={top + innerHeight}
                  stroke="#2d2640"
                  strokeWidth="1"
                  strokeDasharray="4 6"
                  opacity={0.5}
                />
              ))}
              {Array.from({ length: 8 }).map((_, i) => (
                <line
                  key={`gy-${i}`}
                  x1={left}
                  y1={top + (innerHeight * i) / 7}
                  x2={left + innerWidth}
                  y2={top + (innerHeight * i) / 7}
                  stroke="#2d2640"
                  strokeWidth="1"
                  strokeDasharray="4 6"
                  opacity={0.5}
                />
              ))}
              {projected.map((p, i) => {
                if (p.isNoise) return null;
                if (filter !== null && p.label !== filter) return null;
                const isHovered = hovered === points[i];
                const baseR = p.outlier ? 6 : 4.5;
                return (
                  <circle
                    key={i}
                    cx={p.px}
                    cy={p.py}
                    r={isHovered ? baseR * 1.8 : baseR}
                    fill={colors[p.label % colors.length]}
                    opacity={isHovered ? 1 : 0.85}
                    stroke={p.outlier ? "#ff6b6b" : "none"}
                    strokeWidth={p.outlier ? 2 : 0}
                    style={{ transition: "all 0.2s ease", cursor: "pointer" }}
                    onMouseEnter={(e) => {
                      setHovered(points[i]);
                      const rect = containerRef.current?.getBoundingClientRect();
                      setMousePos({
                        x: e.clientX - (rect?.left ?? 0),
                        y: e.clientY - (rect?.top ?? 0),
                      });
                    }}
                    onMouseMove={(e) => {
                      const rect = containerRef.current?.getBoundingClientRect();
                      setMousePos({
                        x: e.clientX - (rect?.left ?? 0),
                        y: e.clientY - (rect?.top ?? 0),
                      });
                    }}
                    onMouseLeave={() => setHovered(null)}
                  />
                );
              })}
            </svg>
            {hovered && (
              <div
                className="donut-tooltip"
                style={{
                  position: "absolute",
                  left: mousePos.x + 12,
                  top: mousePos.y + 12,
                  pointerEvents: "none",
                }}
              >
                <div className="donut-tooltip-head">{hovered.isNoise ? "Noise" : `Cluster ${hovered.label}`}</div>
                <div className="donut-tooltip-line">x: {hovered.x.toFixed(2)}</div>
                <div className="donut-tooltip-line">y: {hovered.y.toFixed(2)}</div>
                {hovered.isNoise && (
                  <div className="donut-tooltip-line" style={{ color: "#94a3b8" }}>
                    Noise point
                  </div>
                )}
                {hovered.outlier && (
                  <div className="donut-tooltip-line" style={{ color: "#ff6b6b" }}>
                    Outlier
                  </div>
                )}
              </div>
            )}
          </div>
        </>
      ) : (
        <div
          className="preview preview-large"
          dangerouslySetInnerHTML={{ __html: svgMarkup ?? "" }}
          style={{ height: 480 }}
        />
      )}
    </article>
  );
}

function HeatmapChart({ cells }: { cells: CorrelationCell[] }) {
  const keys = useMemo(() => Array.from(new Set(cells.flatMap((c) => [c.x, c.y]))).sort(), [cells]);
  if (keys.length === 0) return null;

  /* Ergonomic sizing: never let cells get so small that labels collide */
  const cellSize = Math.max(52, Math.min(76, 780 / Math.max(keys.length, 1)));
  const margin = 120;
  const width = margin + keys.length * cellSize + 24;
  const height = margin + keys.length * cellSize + 24;
  const labelFontSize = Math.max(9, Math.min(11, cellSize * 0.19));
  const valueFontSize = Math.max(8, Math.min(10, cellSize * 0.17));

  const colorFor = (v: number) => {
    const t = Math.max(0, Math.min(1, Math.abs(v)));
    if (v >= 0) {
      const r = Math.round(34 + t * (59 - 34));
      const g = Math.round(211 + t * (130 - 211));
      const b = Math.round(238 + t * (246 - 238));
      return `rgb(${r},${g},${b})`;
    }
    const r = Math.round(244 + t * (167 - 244));
    const g = Math.round(114 + t * (139 - 114));
    const b = Math.round(182 + t * (250 - 182));
    return `rgb(${r},${g},${b})`;
  };

  return (
    <svg viewBox={`0 0 ${width} ${height}`} role="img" aria-label="correlation heatmap">
      {keys.map((ki, i) =>
        keys.map((kj, j) => {
          const cell = cells.find((c) => (c.x === ki && c.y === kj) || (c.x === kj && c.y === ki));
          const value = i === j ? 1 : cell?.value ?? 0;
          const x = margin + j * cellSize;
          const y = margin + i * cellSize;
          return (
            <g key={`${ki}-${kj}`}>
              <rect className="heatmap-cell" x={x + 1} y={y + 1} width={cellSize - 2} height={cellSize - 2} fill={colorFor(value)} rx={6} opacity={0.9} />
              <text x={x + cellSize / 2} y={y + cellSize / 2 + 4} textAnchor="middle" fill="#fff" fontSize={valueFontSize} fontFamily="ui-monospace, monospace">
                {value.toFixed(2)}
              </text>
            </g>
          );
        }),
      )}
      {keys.map((key, i) => (
        <g key={`label-${key}`}>
          {/* Top labels: rotated -45° so they never overlap horizontally */}
          <text
            x={margin + i * cellSize + cellSize / 2}
            y={margin - 10}
            textAnchor="end"
            fill="#d6edf8"
            fontSize={labelFontSize}
            fontFamily="ui-monospace, monospace"
            opacity={1}
            transform={`rotate(-45, ${margin + i * cellSize + cellSize / 2}, ${margin - 10})`}
          >
            {key}
          </text>
          {/* Left labels: right-aligned with generous margin */}
          <text
            x={margin - 12}
            y={margin + i * cellSize + cellSize / 2 + 4}
            textAnchor="end"
            fill="#9b92c4"
            fontSize={labelFontSize}
            fontFamily="ui-monospace, monospace"
          >
            {key}
          </text>
        </g>
      ))}
    </svg>
  );
}

function DonutChart({ bars, samples }: { bars: HistogramBar[]; samples?: Map<string, string[]> }) {
  const total = Math.max(bars.reduce((s, b) => s + b.value, 0), 1);
  const colors = ["#ffda7b", "#ff8c6a", "#f45d96", "#7ad7ff", "#8ef0b5", "#c8a4ff", "#ffb3c1", "#a0c4ff"];
  const cx = 120;
  const cy = 120;
  const radius = 90;
  const innerRadius = 56;
  const [hovered, setHovered] = useState<string | null>(null);
  const [mousePos, setMousePos] = useState({ x: 0, y: 0 });

  let startAngle = 0;
  const slices = bars.map((bar, i) => {
    const angle = (bar.value / total) * Math.PI * 2;
    const endAngle = startAngle + angle;
    const x1 = cx + radius * Math.cos(startAngle);
    const y1 = cy + radius * Math.sin(startAngle);
    const x2 = cx + radius * Math.cos(endAngle);
    const y2 = cy + radius * Math.sin(endAngle);
    const x1i = cx + innerRadius * Math.cos(startAngle);
    const y1i = cy + innerRadius * Math.sin(startAngle);
    const x2i = cx + innerRadius * Math.cos(endAngle);
    const y2i = cy + innerRadius * Math.sin(endAngle);
    const largeArc = angle > Math.PI ? 1 : 0;
    const d = `M ${x1i} ${y1i} L ${x1} ${y1} A ${radius} ${radius} 0 ${largeArc} 1 ${x2} ${y2} L ${x2i} ${y2i} A ${innerRadius} ${innerRadius} 0 ${largeArc} 0 ${x1i} ${y1i}`;
    const midAngle = startAngle + angle / 2;
    const lx = cx + (radius + 18) * Math.cos(midAngle);
    const ly = cy + (radius + 18) * Math.sin(midAngle);
    startAngle = endAngle;
    return { d, color: colors[i % colors.length], label: bar.label, value: bar.value, lx, ly, midAngle };
  });

  const tooltipLines = hovered ? (samples?.get(hovered) ?? []) : [];

  return (
    <div style={{ position: "relative", width: "100%", height: "100%" }}>
      <svg
        viewBox="0 0 340 260"
        role="img"
        aria-label="donut chart"
        onMouseMove={(e) => {
          const rect = e.currentTarget.getBoundingClientRect();
          setMousePos({ x: e.clientX - rect.left, y: e.clientY - rect.top });
        }}
      >
        {slices.map((s, i) => (
          <g key={i}>
            <path
              className="donut-slice"
              d={s.d}
              fill={s.color}
              opacity={0.92}
              onMouseEnter={() => setHovered(s.label)}
              onMouseLeave={() => setHovered(null)}
            />
            <line
              x1={cx + radius * Math.cos(s.midAngle)}
              y1={cy + radius * Math.sin(s.midAngle)}
              x2={s.lx}
              y2={s.ly}
              stroke="#3a3050"
              strokeWidth="1"
            />
            <text
              x={s.lx + (Math.cos(s.midAngle) > 0 ? 6 : -6)}
              y={s.ly + 4}
              textAnchor={Math.cos(s.midAngle) > 0 ? "start" : "end"}
              fill="#9b92c4"
              fontSize="11"
              fontFamily="ui-monospace, monospace"
            >
              {s.label} {s.value}
            </text>
          </g>
        ))}
        <text x={cx} y={cy + 5} textAnchor="middle" fill="#f0eaff" fontSize="18" fontFamily="ui-monospace, monospace">
          {bars.length} classes
        </text>
      </svg>
      {hovered && tooltipLines.length > 0 && (
        <div
          className="donut-tooltip"
          style={{ left: mousePos.x + 12, top: mousePos.y + 12 }}
        >
          <div className="donut-tooltip-head">{hovered}</div>
          {tooltipLines.map((line, i) => (
            <div key={i} className="donut-tooltip-line">{line}</div>
          ))}
        </div>
      )}
    </div>
  );
}

function BooleanChart({ data }: { data: { key: string; trueCount: number; falseCount: number } }) {
  const total = data.trueCount + data.falseCount;
  const max = Math.max(total, 1);
  const [mounted, setMounted] = useState(false);
  useEffect(() => {
    const t = setTimeout(() => setMounted(true), 50);
    return () => clearTimeout(t);
  }, [data]);

  return (
    <div className="boolean-chart">
      <div className="boolean-row" style={{ opacity: mounted ? 1 : 0, transition: "opacity 0.4s ease" }}>
        <span>true</span>
        <div className="boolean-track">
          <div className="boolean-fill boolean-true" style={{ width: mounted ? `${(data.trueCount / max) * 100}%` : "0%" }} />
        </div>
        <strong><FitText minFontSize={12}>{data.trueCount}</FitText></strong>
      </div>
      <div className="boolean-row" style={{ opacity: mounted ? 1 : 0, transition: "opacity 0.4s ease 0.1s" }}>
        <span>false</span>
        <div className="boolean-track">
          <div className="boolean-fill boolean-false" style={{ width: mounted ? `${(data.falseCount / max) * 100}%` : "0%" }} />
        </div>
        <strong><FitText minFontSize={12}>{data.falseCount}</FitText></strong>
      </div>
    </div>
  );
}

function StatsGrid({ schemas }: { schemas: FieldSchema[] }) {
  return (
    <div className="stats-grid">
      {schemas.slice(0, 8).map((s, idx) => {
        const total = s.nullCount + s.numericCount + s.stringCount + s.boolCount + s.arrayCount + s.objectCount;
        const dominantType = Array.from(s.types.entries()).sort((a, b) => b[1] - a[1])[0]?.[0] ?? "unknown";
        const numericSummary = s.hasError ? "error" : `${s.min?.toFixed(2)} / ${s.max?.toFixed(2)} / ${s.mean?.toFixed(2)}`;
        const hasLongSample = s.sampleValues.some((v) => typeof v === "string" && v.length >= 16);
        const isWideStats = (s.numericCount > 0 && numericSummary.length >= 28) || hasLongSample;

        const formatSample = (val: unknown): string => {
          if (val === null) return "null";
          if (typeof val === "object") {
            try {
              return JSON.stringify(val);
            } catch {
              return String(val);
            }
          }
          return String(val);
        };

        return (
          <div className={`stats-card${isWideStats ? " stats-card-wide" : ""} stagger-${Math.min(idx + 1, 8)}`} key={s.key}>
            <div className="stats-header">
              <code>{s.key}</code>
              <span className="stats-badge">{dominantType}</span>
            </div>
            <div className="stats-body">
              {s.numericCount > 0 && (
                <div className="stats-metric">
                  <span>min / max / mean</span>
                  <strong>
                    <FitText minFontSize={12}>
                      {numericSummary}
                    </FitText>
                  </strong>
                </div>
              )}
              {s.stringCount > 0 && (
                <div className="stats-metric">
                  <span>unique</span>
                  <strong><FitText minFontSize={12}>{formatCompactNumber(s.uniqueValues.size)}</FitText></strong>
                </div>
              )}
              <div className="stats-metric">
                <span>null rate</span>
                <strong><FitText minFontSize={12}>{((s.nullCount / Math.max(total, 1)) * 100).toFixed(1)}%</FitText></strong>
              </div>
              {s.sampleValues && s.sampleValues.length > 0 && (
                <div className="stats-metric" style={{ gridColumn: "span 2", display: "flex", flexDirection: "column", gap: "4px", width: "100%", overflow: "hidden" }}>
                  <span>samples</span>
                  {hasLongSample ? (
                    <table style={{ width: "100%", borderCollapse: "collapse", marginTop: "4px", fontSize: "11px", tableLayout: "fixed" }}>
                      <tbody>
                        {s.sampleValues.map((val, vidx) => {
                          const formatted = formatSample(val);
                          return (
                            <tr key={vidx} style={{ borderBottom: vidx === s.sampleValues.length - 1 ? "none" : "1px solid var(--line)", opacity: 0.85 }}>
                              <td
                                style={{
                                  padding: "5px 0",
                                  fontFamily: "var(--font-fira), monospace",
                                  color: "var(--accent-soft)",
                                  textAlign: "left",
                                  whiteSpace: "nowrap",
                                  overflow: "hidden",
                                  textOverflow: "ellipsis"
                                }}
                                title={formatted}
                              >
                                {formatted}
                              </td>
                            </tr>
                          );
                        })}
                      </tbody>
                    </table>
                  ) : (
                    <strong style={{ width: "100%", textAlign: "right" }}>
                      <FitText minFontSize={12}>
                        {s.sampleValues.map(formatSample).join(", ")}
                      </FitText>
                    </strong>
                  )}
                </div>
              )}
            </div>
          </div>
        );
      })}
    </div>
  );
}

/* ─────────── Page ─────────── */

export default function Home() {
  const [payload, setPayload] = useState(SAMPLE);
  const [algorithm, setAlgorithm] = useState("auto");
  const [status, setStatus] = useState("idle");
  const [result, setResult] = useState<Result | null>(null);
  const [isDragging, setIsDragging] = useState(false);
  const [windows, setWindows] = useState<Record<WindowKey, boolean>>({
    input: true,
    runState: true,
    analysis: true,
    schema: true,
    rules: true,
  });

  const toggleWindow = (key: WindowKey) => {
    setWindows((prev) => ({ ...prev, [key]: !prev[key] }));
  };

  const analytics: ParsedLogDataset = useMemo(() => parseLogStream(payload), [payload]);

  const insights = useMemo(() => {
    if (!result) return [];
    const items: { title: string; detail: string; tone: "calm" | "warm" | "hot" }[] = [];

    const rows = result.rows ?? 0;
    const clusters = result.clusters ?? 0;
    items.push({
      title: "Segmentation",
      detail: `${rows} rows were compressed into ${clusters} cluster bands with ${analytics.fieldSchemas.length} numeric dimensions.`,
      tone: clusters > 1 ? "warm" : "calm",
    });

    const entropy = result.entropy ?? 0;
    items.push({
      title: "Distribution",
      detail: `Entropy landed at ${entropy.toFixed(3)}, which suggests a ${entropy >= 1.25 ? "broad" : "narrow"} categorical spread in the payload.`,
      tone: entropy >= 1.25 ? "hot" : "calm",
    });

    const slope = result.trendSlope ?? 0;
    const trendLabel = slope > 0.05 ? "Rising" : slope < -0.05 ? "Falling" : "Stable";
    items.push({
      title: "Trend",
      detail: `The dominant trend is ${trendLabel} with slope ${slope.toFixed(4)} across the primary timeline.`,
      tone: Math.abs(slope) >= 0.05 ? "warm" : "calm",
    });

    const outlierRatio = result.outlierRatio ?? 0;
    items.push({
      title: "Anomaly pressure",
      detail: `Outliers account for ${(outlierRatio * 100).toFixed(2)}% of the observed rows.`,
      tone: outlierRatio >= 0.08 ? "hot" : "calm",
    });

    if (analytics.fieldSchemas.length >= 2) {
      const dims = analytics.fieldSchemas.filter((s) => s.numericCount > 0).slice(0, 2);
      if (dims.length >= 2) {
        items.push({
          title: "Projection",
          detail: `A 2D projection of ${dims[0].key} and ${dims[1].key} is rendered. Range: [${dims[0].min?.toFixed(2) ?? 0}, ${dims[0].max?.toFixed(2) ?? 0}] x [${dims[1].min?.toFixed(2) ?? 0}, ${dims[1].max?.toFixed(2) ?? 0}].`,
          tone: "calm",
        });
      }
    }

    if (outlierRatio >= 0.15) {
      items.push({
        title: "Noise signal",
        detail: `High outlier ratio (${(outlierRatio * 100).toFixed(1)}%) indicates noisy or multi-modal data. Consider tuning epsilon or switching to OPTICS/GMM.`,
        tone: "hot",
      });
    }

    return items;
  }, [result, analytics.fieldSchemas]);

  useEffect(() => {
    const id = setTimeout(() => submit(), 200);
    return () => clearTimeout(id);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  async function fetchIngest(payload: string, algorithm: string, attempt: number = 1): Promise<{ jobId?: string; error?: string }> {
    try {
      const res = await fetch("/api/ingest", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ payload, algorithm }),
      });
      if (!res.ok) {
        throw new Error(`status ${res.status}`);
      }
      return await res.json();
    } catch {
      if (attempt < 10) {
        await new Promise((r) => setTimeout(r, 1000));
        return fetchIngest(payload, algorithm, attempt + 1);
      }
      return { error: "submit failed" };
    }
  }

  async function submit(overridePayload?: string) {
    const activePayload = overridePayload ?? payload;
    setStatus("submitting");
    setResult(null);

    if (algorithm === "auto") {
      setStatus("auto: submitting all algorithms");
      const ingestResults = await Promise.all(
        ALL_ALGORITHMS.map(async (algo) => {
          const res = await fetchIngest(activePayload, algo);
          return { algo, jobId: res.jobId as string | undefined, error: res.error as string | undefined };
        })
      );

      const validJobs = ingestResults.filter((j): j is typeof j & { jobId: string } => !!j.jobId);
      if (validJobs.length === 0) {
        const firstError = ingestResults.find((j) => j.error)?.error;
        setStatus(firstError ?? "submit failed");
        return;
      }

      setStatus(`auto: waiting for ${validJobs.length} jobs…`);

      const finished = await Promise.all(
        validJobs.map(({ algo, jobId }) =>
          new Promise<{ algo: string; result: Result }>((resolve) => {
            const stream = new EventSource(`/api/jobs/${jobId}/stream`);
            stream.onmessage = async (event) => {
              const data = JSON.parse(event.data) as Result;
              if (data.status === "ready" || data.status === "failed") {
                stream.close();
                const resolved = await fetch(`/api/jobs/${jobId}`).then((r) => r.json());
                resolve({ algo, result: { ...resolved, algorithm: algo } });
              }
            };
            stream.onerror = () => {
              stream.close();
              resolve({ algo, result: { jobId, status: "failed", algorithm: algo } as Result });
            };
          })
        )
      );

      const scored = finished
        .filter((f) => f.result.status === "ready")
        .map((f) => ({ ...f, score: scoreResult(f.result) }))
        .sort((a, b) => b.score - a.score);

      if (scored.length > 0) {
        setResult(scored[0].result);
        setStatus(`ready · auto winner: ${scored[0].algo}`);
      } else {
        setStatus("auto: all algorithms failed");
      }
      return;
    }

    const ingest = await fetchIngest(activePayload, algorithm);
    if (!ingest.jobId) {
      setStatus(ingest.error ?? "submit failed");
      return;
    }
    setStatus(`job ${ingest.jobId} queued`);
    const stream = new EventSource(`/api/jobs/${ingest.jobId}/stream`);
    stream.onmessage = async (event) => {
      const data = JSON.parse(event.data) as Result;
      setStatus(data.status);
      if (data.status === "ready" || data.status === "failed") {
        stream.close();
        const resolved = await fetch(`/api/jobs/${ingest.jobId}`).then((r) => r.json());
        setResult(resolved);
        setStatus(resolved.status);
      }
    };
    stream.onerror = () => {
      stream.close();
      setStatus("stream disconnected");
    };
  }

  const histogramMarkup = histogramSvgMarkup(analytics.histogram, "Category histogram", `Grouped by ${analytics.categoryKey}`);
  const engineSvgMarkup = result?.svg ? result.svg : null;

  // Build donut candidates from low-cardinality string fields
  const donutCandidates = useMemo(() => {
    const candidates: { key: string; bars: HistogramBar[]; samples: Map<string, string[]> }[] = [];
    for (const s of analytics.fieldSchemas) {
      if (s.stringCount > 0 && s.uniqueValues.size > 1 && s.uniqueValues.size <= 10) {
        const counts = new Map<string, number>();
        const samples = new Map<string, string[]>();
        analytics.rows.forEach((row) => {
          const v = row.fields[s.key];
          if (typeof v === "string") {
            counts.set(v, (counts.get(v) ?? 0) + 1);
            const list = samples.get(v) ?? [];
            if (list.length < 5) {
              const raw = row.message || row.raw || "";
              const truncated = raw.length > 128 ? raw.slice(0, 128) + "..." : raw;
              list.push(truncated);
            }
            samples.set(v, list);
          }
        });
        const bars = Array.from(counts.entries())
          .map(([label, value]) => ({ label, value }))
          .sort((a, b) => b.value - a.value);
        candidates.push({ key: s.key, bars, samples });
      }
    }
    return candidates.slice(0, 3);
  }, [analytics]);

  return (
    <main className="app-shell">
      <section className="hero animate-fade-in-up">
        <div className="eyebrow">Realtime log workbench</div>
        <h1>Shape-aware parser. Dynamic visuals.</h1>
        <p>
          Paste any JSONL, key-value, or unstructured text. The parser infers schemas, detects types, and renders the
          right chart for each field — scatter, heatmap, donut, line, or histogram.
        </p>
      </section>

      <section className="metrics">
        <div className="metric animate-fade-in-up stagger-1">
          <span>Rows</span>
          <strong><FitText minFontSize={14}><AnimatedValue value={analytics.rows.length} /></FitText></strong>
        </div>
        <div className="metric animate-fade-in-up stagger-2">
          <span>Fields</span>
          <strong><FitText minFontSize={14}><AnimatedValue value={analytics.fieldSchemas.length} /></FitText></strong>
        </div>
        <div className="metric animate-fade-in-up stagger-3">
          <span>Numeric</span>
          <strong><FitText minFontSize={14}><AnimatedValue value={analytics.series.length} /></FitText></strong>
        </div>
        <div className="metric animate-fade-in-up stagger-4">
          <span>Correlations</span>
          <strong><FitText minFontSize={14}><AnimatedValue value={analytics.correlations.length} /></FitText></strong>
        </div>
      </section>

      <section className="workspace">
        <WindowShell show={windows.input} onClose={() => toggleWindow("input")} className="window animate-fade-in-up stagger-1">
            <div className="window-head">
              <div>
                <h2>Input stream</h2>
                <p>Paste newline-delimited JSON with nested objects, key-values, or plain text.</p>
              </div>
            </div>
            <textarea
              className={`textarea ${isDragging ? "is-dragging" : ""}`}
              value={payload}
              onChange={(e) => setPayload(e.target.value)}
              onDragOver={(e) => {
                e.preventDefault();
                setIsDragging(true);
              }}
              onDragLeave={() => setIsDragging(false)}
              onDrop={(e) => {
                e.preventDefault();
                setIsDragging(false);
                const file = e.dataTransfer.files[0];
                if (!file) return;
                const reader = new FileReader();
                reader.onload = (event) => {
                  const text = event.target?.result as string;
                  if (text) {
                    setPayload(text);
                    submit(text);
                  }
                };
                reader.readAsText(file);
              }}
            />
            <div className="controls">
              <select className="select" value={algorithm} onChange={(e) => setAlgorithm(e.target.value)}>
                <option value="auto">Auto (all algorithms)</option>
                <option value="dbscan">DBSCAN</option>
                <option value="kmeans++">K-means++</option>
                <option value="birch">BIRCH</option>
                <option value="mean_shift">Mean Shift</option>
                <option value="optics">OPTICS</option>
                <option value="gmm">GMM</option>
                <option value="agglomerative">Agglomerative</option>
              </select>
              <button className="button" onClick={() => submit()}>
                Run analysis
              </button>
            </div>
        </WindowShell>

        <WindowShell show={windows.runState} onClose={() => toggleWindow("runState")} className="window animate-fade-in-up stagger-2">
            <div className="window-head">
              <div>
                <h2>Run state</h2>
                <p>Engine status and backend summary. Supplemental trends are added below.</p>
              </div>
            </div>
            <div className="status-row">
              <span className={`status-pill ${status === "ready" ? "status-ready" : ""}`}>{status}</span>
              {analytics.invalidRows > 0 ? <span className="status-note">{analytics.invalidRows} invalid row(s)</span> : null}
            </div>
            <div className="run-summary">
              <div className="animate-fade-in-up stagger-1">
                <span>Rows</span>
                <strong><FitText minFontSize={14}><AnimatedValue value={result?.rows ?? analytics.rows.length} /></FitText></strong>
              </div>
              <div className="animate-fade-in-up stagger-2">
                <span>Clusters</span>
                <strong><FitText minFontSize={14}>{result?.clusters ?? "-"}</FitText></strong>
              </div>
              <div className="animate-fade-in-up stagger-3">
                <span>Entropy</span>
                <strong><FitText minFontSize={14}>{typeof result?.entropy === "number" ? result.entropy.toFixed(3) : "-"}</FitText></strong>
              </div>
              <div className="animate-fade-in-up stagger-4">
                <span>Trend</span>
                <strong><FitText minFontSize={14}>{typeof result?.trendSlope === "number" ? result.trendSlope.toFixed(3) : "-"}</FitText></strong>
              </div>
            </div>
        </WindowShell>
      </section>

      <WindowShell show={windows.analysis} onClose={() => toggleWindow("analysis")} className="result-stage animate-fade-in-up window">
        <div className="window-head">
          <div>
            <h2>Analysis output</h2>
            <p>The backend engine render stays intact. Export the rendered preview as PNG.</p>
          </div>
          <div style={{ display: "flex", gap: 12, alignItems: "center" }}>
            {result?.jobId ? (
              <a className="ghost-button" href={`/api/jobs/${result.jobId}/report`} target="_blank" rel="noreferrer">
                Open report
              </a>
            ) : null}
            {engineSvgMarkup ? (
              <button className="ghost-button" onClick={() => downloadSvgAsPng(engineSvgMarkup, "engine-analysis-preview")}>
                Download PNG
              </button>
            ) : null}
          </div>
        </div>
        {result?.points && result.points.length > 0 ? (
          <InteractiveClusterChart
            points={result.points}
            svgMarkup={result.svg}
            algorithm={result.algorithm}
          />
        ) : (
          <div className="preview preview-large" dangerouslySetInnerHTML={{ __html: result?.html ?? "" }} />
        )}
      </WindowShell>

      {/* Dynamic visualization grid */}
      <section className="viz-grid">
        <ChartCard title="Category histogram" subtitle={`Grouped by ${analytics.categoryKey}`} svgMarkup={histogramMarkup} downloadName="category-histogram">
          <HistogramChart bars={analytics.histogram} />
        </ChartCard>

        {analytics.series.map((series) => (
          <ChartCard key={series.key} title={series.key} subtitle="Timestamp-based line chart" svgMarkup={lineSvgMarkup(series)} downloadName={`${series.key}-trend`}>
            <LineChart series={series} />
          </ChartCard>
        ))}

        {analytics.scatterSeries.map((scatter) => (
          <ChartCard key={scatter.key} title={scatter.key} subtitle="Numeric scatter plot" svgMarkup={scatterSvgMarkup(scatter)} downloadName={`scatter-${scatter.key}`}>
            <ScatterChart scatter={scatter} />
          </ChartCard>
        ))}

        {analytics.correlations.length > 0 && (
          <ChartCard title="Correlation heatmap" subtitle="Pearson correlation between numeric fields" svgMarkup={heatmapSvgMarkup(analytics.correlations, "Correlation heatmap")} downloadName="correlation-heatmap">
            <HeatmapChart cells={analytics.correlations} />
          </ChartCard>
        )}

        {donutCandidates.map((candidate) => (
          <ChartCard key={candidate.key} title={candidate.key} subtitle="Categorical distribution" svgMarkup={donutSvgMarkup(candidate.bars, candidate.key, "Distribution")} downloadName={`donut-${candidate.key}`}>
            <DonutChart bars={candidate.bars} samples={candidate.samples} />
          </ChartCard>
        ))}

        {analytics.booleanHistograms.map((bh) => (
          <ChartCard key={bh.key} title={bh.key} subtitle="Boolean distribution" svgMarkup={histogramSvgMarkup([{ label: "true", value: bh.trueCount }, { label: "false", value: bh.falseCount }], bh.key, "Boolean")} downloadName={`boolean-${bh.key}`}>
            <BooleanChart data={bh} />
          </ChartCard>
        ))}

        {analytics.textLengthSeries.map((ts) => (
          <ChartCard key={ts.key} title={ts.key} subtitle="Text length over time" svgMarkup={lineSvgMarkup(ts)} downloadName={`textlen-${ts.key}`}>
            <LineChart series={ts} />
          </ChartCard>
        ))}

        {analytics.nullRatioBars.length > 0 && (
          <ChartCard title="Null ratio" subtitle="Fields with missing values" svgMarkup={histogramSvgMarkup(analytics.nullRatioBars, "Null ratio", "% missing")} downloadName="null-ratio">
            <HistogramChart bars={analytics.nullRatioBars} />
          </ChartCard>
        )}
      </section>

      {analytics.fieldSchemas.length > 0 && (
        <WindowShell show={windows.schema} onClose={() => toggleWindow("schema")} className="stats-section animate-fade-in-up window">
          <div className="window-head">
            <div>
              <h2>Inferred schema</h2>
              <p>Auto-detected field types, ranges, and cardinalities from the stream.</p>
            </div>
          </div>
          <StatsGrid schemas={analytics.fieldSchemas} />
        </WindowShell>
      )}

      {insights.length > 0 && (
        <WindowShell show={windows.rules} onClose={() => toggleWindow("rules")} className="stats-section animate-fade-in-up window">
          <div className="window-head">
            <div>
              <h2>Detected rules</h2>
              <p>Heuristics inferred from the analyzed payload.</p>
            </div>
          </div>
          <div className="rules-grid">
            {insights.map((item) => (
              <article key={item.title} className={`rules-card rules-card-${item.tone}`}>
                <h4>{item.title}</h4>
                <p>{item.detail}</p>
              </article>
            ))}
          </div>
        </WindowShell>
      )}

      <Dock windows={windows} onToggle={toggleWindow} />
    </main>
  );
}
