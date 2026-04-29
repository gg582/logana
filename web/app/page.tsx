"use client";

import { useId, useMemo, useRef, useState } from "react";

import { parseLogStream, type HistogramBar, type SeriesChart } from "../lib/log-parser";

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

  const path = points
    .map((point, index) => `${index === 0 ? "M" : "L"} ${point.x.toFixed(2)} ${point.y.toFixed(2)}`)
    .join(" ");

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

export default function Home() {
  const [payload, setPayload] = useState(SAMPLE);
  const [algorithm, setAlgorithm] = useState("dbscan");
  const [status, setStatus] = useState("idle");
  const [result, setResult] = useState<Result | null>(null);

  const analytics = useMemo(() => parseLogStream(payload), [payload]);

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

  const histogramMarkup = histogramSvgMarkup(
    analytics.histogram,
    "Category histogram",
    `Grouped by ${analytics.categoryKey}`,
  );
  const engineSvgMarkup = result?.svg ? result.svg : null;

  return (
    <main className="app-shell">
      <section className="hero">
        <div className="eyebrow">Realtime log workbench</div>
        <h1>Batch tiny packets. Keep the pipeline hot.</h1>
        <p>
          JSONL logs in, category counts and timestamp trends out. Paste a stream, run it, and export the
          visual readout as PNG when you need to share it.
        </p>
      </section>

      <section className="metrics">
        <div className="metric">
          <span>Rows</span>
          <strong>{analytics.rows.length}</strong>
        </div>
        <div className="metric">
          <span>Categories</span>
          <strong>{analytics.histogram.length}</strong>
        </div>
        <div className="metric">
          <span>Series</span>
          <strong>{analytics.series.length}</strong>
        </div>
        <div className="metric">
          <span>Clock Key</span>
          <strong>{analytics.timestampKey}</strong>
        </div>
      </section>

      <section className="workspace">
        <div className="window">
          <Chrome />
          <div className="window-head">
            <div>
              <h2>Input stream</h2>
              <p>Paste newline-delimited JSON with a timestamp field and any numeric metrics.</p>
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
              <p>Engine status and backend summary stay here. Supplemental trends are added below without replacing the main result.</p>
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
              <p>The existing engine render stays intact. Export the rendered preview as PNG if you need a static artifact.</p>
            </div>
            {engineSvgMarkup ? (
              <button className="ghost-button" onClick={() => downloadSvgAsPng(engineSvgMarkup, "engine-analysis-preview")}>
                Download PNG
              </button>
            ) : null}
          </div>
          <div className="preview preview-large" dangerouslySetInnerHTML={{ __html: result?.html ?? "" }} />
        </div>
      </section>

      <section className="viz-grid">
        <ChartCard
          title="Category histogram"
          subtitle={`Grouped by ${analytics.categoryKey}`}
          svgMarkup={histogramMarkup}
          downloadName="category-histogram"
        >
          <HistogramChart bars={analytics.histogram} />
        </ChartCard>

        {analytics.series.map((series) => (
          <ChartCard
            key={series.key}
            title={series.key}
            subtitle="Timestamp-based line chart"
            svgMarkup={lineSvgMarkup(series)}
            downloadName={`${series.key}-trend`}
          >
            <LineChart series={series} />
          </ChartCard>
        ))}
      </section>
    </main>
  );
}
