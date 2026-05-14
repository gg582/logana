"use client";

import { useId, useMemo, useRef, useState } from "react";
import {
  parseLogStream,
  type HistogramBar,
  type SeriesChart,
  type ScatterSeries,
  type CorrelationCell,
  type FieldSchema,
  type ParsedLogDataset,
} from "../lib/log-parser";

const SAMPLE = `{"timestamp":"2026-04-30T00:15:02.123Z","level":"INFO","service":"api","event":"login","user_id":9421,"latency_ms":12}
{"timestamp":"2026-04-30T00:15:02.445Z","level":"WARN","service":"db","event":"pool","active_conns":425,"queue_depth":18}
{"timestamp":"2026-04-30T00:15:03.001Z","level":"ERROR","service":"payments","event":"txn","duration_ms":73,"status_code":503}
{"timestamp":"2026-04-30T00:15:03.110Z","level":"INFO","service":"storage","event":"backup","iops":15400,"bytes_sent":1200}
{"timestamp":"2026-04-30T00:15:03.155Z","level":"DEBUG","service":"cache","event":"heartbeat","retry_attempt":3,"ttl_ms":4500}`;

type Result = {
  jobId: string;
  status: string;
  rows?: number;
  clusters?: number;
  entropy?: number;
  trendSlope?: number;
  svg?: string;
  html?: string;
  error?: string;
};

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
          <text x="${x + barWidth / 2}" y="${y - 12}" text-anchor="middle" fill="#ffe7b7" font-size="20" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(String(bar.value))}</text>
          <text x="${x + barWidth / 2}" y="${height - 34}" text-anchor="middle" fill="#7c6750" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(bar.label)}</text>
        </g>`;
    })
    .join("");

  return `
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}">
      <defs>
        <linearGradient id="bgGradient" x1="0" x2="1" y1="0" y2="1">
          <stop offset="0%" stop-color="#1a1310" />
          <stop offset="50%" stop-color="#241916" />
          <stop offset="100%" stop-color="#131114" />
        </linearGradient>
        <linearGradient id="barGradient" x1="0" x2="0" y1="0" y2="1">
          <stop offset="0%" stop-color="#ffd978" />
          <stop offset="100%" stop-color="#ff7c66" />
        </linearGradient>
      </defs>
      <rect width="${width}" height="${height}" rx="34" fill="url(#bgGradient)" />
      <text x="${left}" y="48" fill="#fff1d6" font-size="28" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(title)}</text>
      <text x="${left}" y="74" fill="#b69774" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(subtitle)}</text>
      <line x1="${left}" y1="${top + innerHeight}" x2="${width - right}" y2="${top + innerHeight}" stroke="#4b3a30" stroke-width="2" />
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
          <text x="24" y="${y + 6}" fill="#8d7561" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(value.toFixed(0))}</text>
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
          <stop offset="0%" stop-color="#1a1310" />
          <stop offset="50%" stop-color="#241916" />
          <stop offset="100%" stop-color="#131114" />
        </linearGradient>
      </defs>
      <rect width="${width}" height="${height}" rx="34" fill="url(#bgGradient)" />
      <text x="${left}" y="48" fill="#fff1d6" font-size="28" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(series.key)}</text>
      <text x="${left}" y="74" fill="#b69774" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">timestamp trend</text>
      ${guides}
      <line x1="${left}" y1="${top + innerHeight}" x2="${width - right}" y2="${top + innerHeight}" stroke="#4b3a30" stroke-width="2" />
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
      return `<text x="${x}" y="${height - 28}" text-anchor="middle" fill="#7c6750" font-size="16" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(value.toFixed(1))}</text>`;
    })
    .join("");

  const yGuides = [0, 0.5, 1]
    .map((ratio) => {
      const y = top + innerHeight - ratio * innerHeight;
      const value = minY + ratio * spanY;
      return `<text x="18" y="${y + 5}" fill="#7c6750" font-size="16" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(value.toFixed(1))}</text>`;
    })
    .join("");

  return `
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}">
      <defs>
        <linearGradient id="bgGradient" x1="0" x2="1" y1="0" y2="1">
          <stop offset="0%" stop-color="#1a1310" />
          <stop offset="50%" stop-color="#241916" />
          <stop offset="100%" stop-color="#131114" />
        </linearGradient>
      </defs>
      <rect width="${width}" height="${height}" rx="34" fill="url(#bgGradient)" />
      <text x="${left}" y="48" fill="#fff1d6" font-size="28" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(scatter.key)}</text>
      <text x="${left}" y="74" fill="#b69774" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">Scatter plot</text>
      <line x1="${left}" y1="${top + innerHeight}" x2="${width - right}" y2="${top + innerHeight}" stroke="#4b3a30" stroke-width="2" />
      <line x1="${left}" y1="${top}" x2="${left}" y2="${top + innerHeight}" stroke="#4b3a30" stroke-width="2" />
      ${xGuides}
      ${yGuides}
      ${dots}
    </svg>`;
}

function heatmapSvgMarkup(cells: CorrelationCell[], title: string) {
  const width = 960;
  const height = 560;
  const left = 140;
  const top = 100;
  const right = 60;
  const bottom = 60;

  const keys = Array.from(new Set(cells.flatMap((c) => [c.x, c.y]))).sort();
  if (keys.length === 0) return "";

  const cellSize = Math.min(
    (width - left - right) / keys.length,
    (height - top - bottom) / keys.length,
    80,
  );
  const innerW = cellSize * keys.length;
  const innerH = cellSize * keys.length;

  const colorFor = (v: number) => {
    const t = Math.max(0, Math.min(1, Math.abs(v)));
    if (v >= 0) {
      const r = Math.round(26 + t * (255 - 26));
      const g = Math.round(19 + t * (218 - 19));
      const b = Math.round(16 + t * (123 - 16));
      return `rgb(${r},${g},${b})`;
    }
    const r = Math.round(26 + t * (244 - 26));
    const g = Math.round(19 + t * (93 - 19));
    const b = Math.round(16 + t * (150 - 16));
    return `rgb(${r},${g},${b})`;
  };

  const rects: string[] = [];
  for (let i = 0; i < keys.length; i++) {
    for (let j = 0; j < keys.length; j++) {
      const x = left + j * cellSize;
      const y = top + i * cellSize;
      if (i === j) {
        rects.push(`<rect x="${x}" y="${y}" width="${cellSize}" height="${cellSize}" fill="#2a1f1b" rx="6" />`);
        rects.push(`<text x="${x + cellSize / 2}" y="${y + cellSize / 2 + 5}" text-anchor="middle" fill="#8d7561" font-size="14" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">1</text>`);
        continue;
      }
      const cell = cells.find((c) => (c.x === keys[i] && c.y === keys[j]) || (c.x === keys[j] && c.y === keys[i]));
      const value = cell?.value ?? 0;
      rects.push(`<rect x="${x + 1}" y="${y + 1}" width="${cellSize - 2}" height="${cellSize - 2}" fill="${colorFor(value)}" rx="6" opacity="0.9" />`);
      rects.push(`<text x="${x + cellSize / 2}" y="${y + cellSize / 2 + 5}" text-anchor="middle" fill="#fff" font-size="13" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${value.toFixed(2)}</text>`);
    }
  }

  const labels = keys
    .map((key, i) => {
      const x = left + i * cellSize + cellSize / 2;
      const y = top + i * cellSize + cellSize / 2;
      return `
        <text x="${x}" y="${top - 10}" text-anchor="middle" fill="#b69774" font-size="14" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(key)}</text>
        <text x="${left - 10}" y="${y + 5}" text-anchor="end" fill="#b69774" font-size="14" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(key)}</text>
      `;
    })
    .join("");

  return `
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" width="${width}" height="${height}">
      <defs>
        <linearGradient id="bgGradient" x1="0" x2="1" y1="0" y2="1">
          <stop offset="0%" stop-color="#1a1310" />
          <stop offset="50%" stop-color="#241916" />
          <stop offset="100%" stop-color="#131114" />
        </linearGradient>
      </defs>
      <rect width="${width}" height="${height}" rx="34" fill="url(#bgGradient)" />
      <text x="${left}" y="48" fill="#fff1d6" font-size="28" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(title)}</text>
      <text x="${left}" y="74" fill="#b69774" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">Numeric correlation matrix</text>
      ${labels}
      ${rects.join("")}
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
  const colors = ["#ffda7b", "#ff8c6a", "#f45d96", "#7ad7ff", "#8ef0b5", "#c8a4ff", "#ffb3c1", "#a0c4ff"];

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
          <stop offset="0%" stop-color="#1a1310" />
          <stop offset="50%" stop-color="#241916" />
          <stop offset="100%" stop-color="#131114" />
        </linearGradient>
      </defs>
      <rect width="${width}" height="${height}" rx="34" fill="url(#bgGradient)" />
      <text x="80" y="48" fill="#fff1d6" font-size="28" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(title)}</text>
      <text x="80" y="74" fill="#b69774" font-size="18" font-family="ui-monospace, SFMono-Regular, Menlo, monospace">${escapeXml(subtitle)}</text>
      ${paths}
    </svg>`;
}

async function downloadSvgAsPng(markup: string, name: string) {
  const blob = new Blob([markup], { type: "image/svg+xml;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  try {
    const image = await new Promise<HTMLImageElement>((resolve, reject) => {
      const img = new Image();
      img.onload = () => resolve(img);
      img.onerror = () => reject(new Error("image load failed"));
      img.src = url;
    });
    const canvas = document.createElement("canvas");
    canvas.width = image.width || 960;
    canvas.height = image.height || 560;
    const context = canvas.getContext("2d");
    if (!context) throw new Error("canvas context unavailable");
    context.drawImage(image, 0, 0);
    const pngUrl = canvas.toDataURL("image/png");
    const link = document.createElement("a");
    link.href = pngUrl;
    link.download = `${name}.png`;
    link.click();
  } finally {
    URL.revokeObjectURL(url);
  }
}

/* ─────────── React components ─────────── */

function Chrome() {
  return (
    <div className="chrome">
      <span className="dot dot-red" />
      <span className="dot dot-yellow" />
      <span className="dot dot-green" />
    </div>
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
  return (
    <div className="histogram">
      {bars.map((bar) => (
        <div className="histogram-bar" key={bar.label}>
          <span>{bar.label}</span>
          <div className="histogram-track">
            <div className="histogram-fill" style={{ width: `${(bar.value / max) * 100}%` }} />
          </div>
          <strong>{bar.value}</strong>
        </div>
      ))}
    </div>
  );
}

function LineChart({ series }: { series: SeriesChart }) {
  const svgRef = useRef<SVGSVGElement | null>(null);
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

  return (
    <svg ref={svgRef} viewBox={`0 0 ${width} ${height}`} role="img" aria-label={`${series.key} line chart`}>
      <path className="chart-grid" d={`M ${left} ${top + innerHeight} H ${width - right}`} />
      <path d={path} fill="none" stroke={series.color} strokeWidth="3.5" strokeLinecap="round" strokeLinejoin="round" />
      {points.map((point) => (
        <circle key={`${series.key}-${point.label}`} cx={point.x} cy={point.y} r="3.5" fill={series.color} />
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
      <text x={left} y={top - 2} fill="#b69774" fontSize="12" fontFamily="ui-monospace, monospace">
        {scatter.yKey}
      </text>
      <text x={width - right} y={height - 8} textAnchor="end" fill="#b69774" fontSize="12" fontFamily="ui-monospace, monospace">
        {scatter.xKey}
      </text>
      <line x1={left} y1={top + innerHeight} x2={width - right} y2={top + innerHeight} stroke="#4b3a30" strokeWidth="1.5" />
      <line x1={left} y1={top} x2={left} y2={top + innerHeight} stroke="#4b3a30" strokeWidth="1.5" />
      {points.map((p, i) => (
        <circle key={i} cx={p.x} cy={p.y} r="3.5" fill={scatter.color} opacity="0.85" />
      ))}
    </svg>
  );
}

function HeatmapChart({ cells }: { cells: CorrelationCell[] }) {
  const keys = useMemo(() => Array.from(new Set(cells.flatMap((c) => [c.x, c.y]))).sort(), [cells]);
  const cellSize = 56;
  const width = 120 + keys.length * cellSize;
  const height = 100 + keys.length * cellSize;

  const colorFor = (v: number) => {
    const t = Math.max(0, Math.min(1, Math.abs(v)));
    if (v >= 0) {
      const r = Math.round(26 + t * (255 - 26));
      const g = Math.round(19 + t * (218 - 19));
      const b = Math.round(16 + t * (123 - 16));
      return `rgb(${r},${g},${b})`;
    }
    const r = Math.round(26 + t * (244 - 26));
    const g = Math.round(19 + t * (93 - 19));
    const b = Math.round(16 + t * (150 - 16));
    return `rgb(${r},${g},${b})`;
  };

  return (
    <svg viewBox={`0 0 ${width} ${height}`} role="img" aria-label="correlation heatmap">
      {keys.map((key, i) => (
        <g key={`label-${key}`}>
          <text x={100 + i * cellSize + cellSize / 2} y={90} textAnchor="middle" fill="#b69774" fontSize="12" fontFamily="ui-monospace, monospace">
            {key}
          </text>
          <text x={90} y={100 + i * cellSize + cellSize / 2 + 4} textAnchor="end" fill="#b69774" fontSize="12" fontFamily="ui-monospace, monospace">
            {key}
          </text>
        </g>
      ))}
      {keys.map((ki, i) =>
        keys.map((kj, j) => {
          const cell = cells.find((c) => (c.x === ki && c.y === kj) || (c.x === kj && c.y === ki));
          const value = i === j ? 1 : cell?.value ?? 0;
          const x = 100 + j * cellSize;
          const y = 100 + i * cellSize;
          return (
            <g key={`${ki}-${kj}`}>
              <rect x={x + 1} y={y + 1} width={cellSize - 2} height={cellSize - 2} fill={colorFor(value)} rx={6} opacity={0.9} />
              <text x={x + cellSize / 2} y={y + cellSize / 2 + 4} textAnchor="middle" fill="#fff" fontSize="11" fontFamily="ui-monospace, monospace">
                {value.toFixed(2)}
              </text>
            </g>
          );
        }),
      )}
    </svg>
  );
}

function DonutChart({ bars }: { bars: HistogramBar[] }) {
  const total = Math.max(bars.reduce((s, b) => s + b.value, 0), 1);
  const colors = ["#ffda7b", "#ff8c6a", "#f45d96", "#7ad7ff", "#8ef0b5", "#c8a4ff", "#ffb3c1", "#a0c4ff"];
  const cx = 120;
  const cy = 120;
  const radius = 90;
  const innerRadius = 56;

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

  return (
    <svg viewBox="0 0 340 260" role="img" aria-label="donut chart">
      {slices.map((s, i) => (
        <g key={i}>
          <path d={s.d} fill={s.color} opacity={0.92} />
          <line
            x1={cx + radius * Math.cos(s.midAngle)}
            y1={cy + radius * Math.sin(s.midAngle)}
            x2={s.lx}
            y2={s.ly}
            stroke="#4b3a30"
            strokeWidth="1"
          />
          <text
            x={s.lx + (Math.cos(s.midAngle) > 0 ? 6 : -6)}
            y={s.ly + 4}
            textAnchor={Math.cos(s.midAngle) > 0 ? "start" : "end"}
            fill="#b69774"
            fontSize="11"
            fontFamily="ui-monospace, monospace"
          >
            {s.label} {s.value}
          </text>
        </g>
      ))}
      <text x={cx} y={cy + 5} textAnchor="middle" fill="#fff1d6" fontSize="18" fontFamily="ui-monospace, monospace">
        {bars.length} classes
      </text>
    </svg>
  );
}

function BooleanChart({ data }: { data: { key: string; trueCount: number; falseCount: number } }) {
  const total = data.trueCount + data.falseCount;
  const max = Math.max(total, 1);
  return (
    <div className="boolean-chart">
      <div className="boolean-row">
        <span>true</span>
        <div className="boolean-track">
          <div className="boolean-fill boolean-true" style={{ width: `${(data.trueCount / max) * 100}%` }} />
        </div>
        <strong>{data.trueCount}</strong>
      </div>
      <div className="boolean-row">
        <span>false</span>
        <div className="boolean-track">
          <div className="boolean-fill boolean-false" style={{ width: `${(data.falseCount / max) * 100}%` }} />
        </div>
        <strong>{data.falseCount}</strong>
      </div>
    </div>
  );
}

function StatsGrid({ schemas }: { schemas: FieldSchema[] }) {
  return (
    <div className="stats-grid">
      {schemas.slice(0, 8).map((s) => {
        const total = s.nullCount + s.numericCount + s.stringCount + s.boolCount + s.arrayCount + s.objectCount;
        const dominantType = Array.from(s.types.entries()).sort((a, b) => b[1] - a[1])[0]?.[0] ?? "unknown";
        return (
          <div className="stats-card" key={s.key}>
            <div className="stats-header">
              <code>{s.key}</code>
              <span className="stats-badge">{dominantType}</span>
            </div>
            <div className="stats-body">
              {s.numericCount > 0 && (
                <div className="stats-metric">
                  <span>min / max / mean</span>
                  <strong>
                    {s.min?.toFixed(2)} / {s.max?.toFixed(2)} / {s.mean?.toFixed(2)}
                  </strong>
                </div>
              )}
              {s.stringCount > 0 && (
                <div className="stats-metric">
                  <span>unique</span>
                  <strong>{formatCompactNumber(s.uniqueValues.size)}</strong>
                </div>
              )}
              <div className="stats-metric">
                <span>null rate</span>
                <strong>{((s.nullCount / Math.max(total, 1)) * 100).toFixed(1)}%</strong>
              </div>
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
  const [algorithm, setAlgorithm] = useState("dbscan");
  const [status, setStatus] = useState("idle");
  const [result, setResult] = useState<Result | null>(null);

  const analytics: ParsedLogDataset = useMemo(() => parseLogStream(payload), [payload]);

  async function submit() {
    setStatus("submitting");
    setResult(null);
    const ingest = await fetch("/api/ingest", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ payload, algorithm }),
    }).then((r) => r.json());
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
    const candidates: { key: string; bars: HistogramBar[] }[] = [];
    for (const s of analytics.fieldSchemas) {
      if (s.stringCount > 0 && s.uniqueValues.size > 1 && s.uniqueValues.size <= 10) {
        const counts = new Map<string, number>();
        analytics.rows.forEach((row) => {
          const v = row.fields[s.key];
          if (typeof v === "string") counts.set(v, (counts.get(v) ?? 0) + 1);
        });
        const bars = Array.from(counts.entries())
          .map(([label, value]) => ({ label, value }))
          .sort((a, b) => b.value - a.value);
        candidates.push({ key: s.key, bars });
      }
    }
    return candidates.slice(0, 3);
  }, [analytics]);

  return (
    <main className="app-shell">
      <section className="hero">
        <div className="eyebrow">Realtime log workbench</div>
        <h1>Shape-aware parser. Dynamic visuals.</h1>
        <p>
          Paste any JSONL, key-value, or unstructured text. The parser infers schemas, detects types, and renders the
          right chart for each field — scatter, heatmap, donut, line, or histogram.
        </p>
      </section>

      <section className="metrics">
        <div className="metric">
          <span>Rows</span>
          <strong>{analytics.rows.length}</strong>
        </div>
        <div className="metric">
          <span>Fields</span>
          <strong>{analytics.fieldSchemas.length}</strong>
        </div>
        <div className="metric">
          <span>Numeric</span>
          <strong>{analytics.series.length}</strong>
        </div>
        <div className="metric">
          <span>Correlations</span>
          <strong>{analytics.correlations.length}</strong>
        </div>
      </section>

      <section className="workspace">
        <div className="window">
          <Chrome />
          <div className="window-head">
            <div>
              <h2>Input stream</h2>
              <p>Paste newline-delimited JSON with nested objects, key-values, or plain text.</p>
            </div>
          </div>
          <textarea className="textarea" value={payload} onChange={(e) => setPayload(e.target.value)} />
          <div className="controls">
            <select className="select" value={algorithm} onChange={(e) => setAlgorithm(e.target.value)}>
              <option value="dbscan">DBSCAN</option>
              <option value="kmeans++">K-means++</option>
              <option value="birch">BIRCH</option>
              <option value="mean_shift">Mean Shift</option>
              <option value="optics">OPTICS</option>
              <option value="gmm">GMM</option>
              <option value="agglomerative">Agglomerative</option>
            </select>
            <button className="button" onClick={submit}>
              Run analysis
            </button>
          </div>
        </div>

        <div className="window">
          <Chrome />
          <div className="window-head">
            <div>
              <h2>Run state</h2>
              <p>Engine status and backend summary. Supplemental trends are added below.</p>
            </div>
          </div>
          <div className="status-row">
            <span className="status-pill">{status}</span>
            {analytics.invalidRows > 0 ? <span className="status-note">{analytics.invalidRows} invalid row(s)</span> : null}
          </div>
          <div className="run-summary">
            <div>
              <span>Rows</span>
              <strong>{result?.rows ?? analytics.rows.length}</strong>
            </div>
            <div>
              <span>Clusters</span>
              <strong>{result?.clusters ?? "-"}</strong>
            </div>
            <div>
              <span>Entropy</span>
              <strong>{typeof result?.entropy === "number" ? result.entropy.toFixed(3) : "-"}</strong>
            </div>
            <div>
              <span>Trend</span>
              <strong>{typeof result?.trendSlope === "number" ? result.trendSlope.toFixed(3) : "-"}</strong>
            </div>
          </div>
        </div>
      </section>

      <section className="result-stage">
        <div className="window">
          <Chrome />
          <div className="window-head">
            <div>
              <h2>Analysis output</h2>
              <p>The backend engine render stays intact. Export the rendered preview as PNG.</p>
            </div>
            <div style={{ display: "flex", gap: 12, alignItems: "center" }}>
              {result?.jobId ? (
                <a className="ghost-button" href={`/api/jobs/${result.jobId}/report`} target="_blank" rel="noreferrer">
                  Open SSR report
                </a>
              ) : null}
              {engineSvgMarkup ? (
                <button className="ghost-button" onClick={() => downloadSvgAsPng(engineSvgMarkup, "engine-analysis-preview")}>
                  Download PNG
                </button>
              ) : null}
            </div>
          </div>
          <div className="preview preview-large" dangerouslySetInnerHTML={{ __html: result?.html ?? "" }} />
        </div>
      </section>

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
            <DonutChart bars={candidate.bars} />
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
        <section className="stats-section">
          <div className="window">
            <Chrome />
            <div className="window-head">
              <div>
                <h2>Inferred schema</h2>
                <p>Auto-detected field types, ranges, and cardinalities from the stream.</p>
              </div>
            </div>
            <StatsGrid schemas={analytics.fieldSchemas} />
          </div>
        </section>
      )}
    </main>
  );
}
