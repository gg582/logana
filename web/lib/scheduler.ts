import { ingestLogs, getJobStatus, getJobResult } from "./engine-client";
import * as os from "os";

type SchedulerJob = {
  id: string;
  payload: string;
  algorithm: string;
  status: "queued" | "submitted" | "ready" | "failed";
  engineJobId?: string;
  createdAt: number;
  submittedAt?: number;
  completedAt?: number;
  result?: unknown;
  error?: string;
};

const MAX_CONCURRENT = 2;
const CPU_THRESHOLD = 0.95;
const MEM_THRESHOLD = 0.05;
const DEFAULT_AVG_MS = 3000;
const MAX_HISTORY = 10;
const TICK_MS = 1000;

let pendingJobs: SchedulerJob[] = [];
const activeJobs = new Map<string, SchedulerJob>();
const completedJobs = new Map<string, SchedulerJob>();
const processingTimes: number[] = [];

function generateId(): string {
  return `${Date.now()}-${Math.random().toString(36).slice(2, 9)}`;
}

function isResourceAvailable(): boolean {
  const cpus = os.cpus().length || 1;
  const load = os.loadavg()[0];
  const freeMemRatio = os.freemem() / os.totalmem();
  if (load > cpus * CPU_THRESHOLD) return false;
  if (freeMemRatio < MEM_THRESHOLD) return false;
  return true;
}

function getAverageProcessingTime(): number {
  if (processingTimes.length === 0) return DEFAULT_AVG_MS;
  const sum = processingTimes.reduce((a, b) => a + b, 0);
  return Math.round(sum / processingTimes.length);
}

function updateAverageProcessingTime(job: SchedulerJob) {
  if (!job.submittedAt || !job.completedAt) return;
  const duration = job.completedAt - job.submittedAt;
  processingTimes.push(duration);
  if (processingTimes.length > MAX_HISTORY) processingTimes.shift();
}

function getQueuePosition(jobId: string): number {
  return pendingJobs.findIndex((j) => j.id === jobId);
}

function getEtaMs(jobId: string): number {
  const pos = getQueuePosition(jobId);
  if (pos === -1) return 0;
  return (pos + 1) * getAverageProcessingTime();
}

export function enqueueJob(payload: string, algorithm: string): SchedulerJob {
  const job: SchedulerJob = {
    id: generateId(),
    payload,
    algorithm,
    status: "queued",
    createdAt: Date.now(),
  };
  pendingJobs.push(job);
  return job;
}

export function getSchedulerJob(jobId: string): SchedulerJob | undefined {
  const pending = pendingJobs.find((j) => j.id === jobId);
  if (pending) return pending;
  const active = activeJobs.get(jobId);
  if (active) return active;
  return completedJobs.get(jobId);
}

export async function getSchedulerStatus(jobId: string) {
  const job = getSchedulerJob(jobId);
  if (!job) return null;

  const position = getQueuePosition(jobId);
  const etaMs = position >= 0 ? getEtaMs(jobId) : 0;

  let engineStatus: string = job.status;
  if (job.engineJobId && (job.status === "submitted" || job.status === "ready" || job.status === "failed")) {
    try {
      const s = (await getJobStatus(job.engineJobId)) as { status?: string };
      if (s.status) engineStatus = s.status;
    } catch {
      // ignore engine errors
    }
  }

  return {
    jobId: job.id,
    status: engineStatus,
    position: position >= 0 ? position + 1 : 0,
    etaMs,
    engineJobId: job.engineJobId,
    updatedMs: job.completedAt || job.submittedAt || job.createdAt,
  };
}

export async function getSchedulerResult(jobId: string): Promise<unknown | null> {
  const job = getSchedulerJob(jobId);
  if (!job) return null;
  if (job.result) return job.result;
  if (job.engineJobId) {
    try {
      const result = await getJobResult(job.engineJobId);
      job.result = result;
      return result;
    } catch {
      return null;
    }
  }
  return null;
}

async function monitorEngineJob(job: SchedulerJob) {
  if (!job.engineJobId) return;
  while (true) {
    await new Promise((resolve) => setTimeout(resolve, TICK_MS));
    try {
      const status = await getJobStatus(job.engineJobId);
      if (
        status.status === "ready" ||
        status.status === "failed" ||
        status.error
      ) {
        const result = (await getJobResult(job.engineJobId)) as Record<string, unknown>;
        job.result = result;
        const failed = result.status === "failed" || status.status === "failed" || status.error;
        job.status = failed ? "failed" : "ready";
        if (status.error && (!result.error || typeof result.error !== "string")) {
          job.result = { ...result, error: status.error, status: "failed" };
        }
        job.completedAt = Date.now();
        completedJobs.set(job.id, job);
        activeJobs.delete(job.id);
        updateAverageProcessingTime(job);
        break;
      }
    } catch {
      // continue polling engine
    }
  }
}

async function ingestWithRetry(payload: string, algorithm: string, maxRetries = 12): Promise<{ jobId?: string }> {
  let lastError: Error | undefined;
  for (let i = 0; i < maxRetries; i++) {
    try {
      return (await ingestLogs(payload, algorithm)) as { jobId?: string };
    } catch (e) {
      lastError = e instanceof Error ? e : new Error(String(e));
      await new Promise((r) => setTimeout(r, 5000));
    }
  }
  throw lastError ?? new Error("ingest failed after max retries");
}

setInterval(async () => {
  if (activeJobs.size >= MAX_CONCURRENT) return;
  if (!isResourceAvailable()) return;
  if (pendingJobs.length === 0) return;

  const job = pendingJobs.shift()!;
  try {
    const res = await ingestWithRetry(job.payload, job.algorithm);
    job.engineJobId = res.jobId;
    job.status = "submitted";
    job.submittedAt = Date.now();
    activeJobs.set(job.id, job);
    monitorEngineJob(job);
  } catch (e) {
    job.status = "failed";
    job.error = e instanceof Error ? e.message : String(e);
    job.completedAt = Date.now();
    completedJobs.set(job.id, job);
  }
}, TICK_MS);
