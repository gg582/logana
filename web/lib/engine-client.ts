const PORT = Number(process.env.LOGANA_ENGINE_PORT ?? 24445);
const BASE_URL = process.env.LOGANA_ENGINE_BASE_URL ?? `http://127.0.0.1:${PORT}`;

async function requestJson(path: string, init?: RequestInit) {
  const response = await fetch(`${BASE_URL}${path}`, {
    ...init,
    headers: {
      "content-type": "application/json",
      ...(init?.headers ?? {}),
    },
    cache: "no-store",
  });

  const text = await response.text();
  const data = text ? JSON.parse(text) : {};
  if (!response.ok) {
    throw new Error(typeof data?.error === "string" ? data.error : `engine request failed: ${response.status}`);
  }
  return data;
}

export async function ingestLogs(payload: string, algorithm: string) {
  return requestJson("/ingest", {
    method: "POST",
    body: JSON.stringify({ payload, algorithm }),
  });
}

export async function getJobStatus(jobId: string) {
  return requestJson(`/jobs/${jobId}/status`);
}

export async function getJobResult(jobId: string) {
  return requestJson(`/jobs/${jobId}`);
}

export async function getJobReport(jobId: string) {
  const response = await fetch(`${BASE_URL}/jobs/${jobId}/view`, {
    cache: "no-store",
  });
  if (!response.ok) {
    throw new Error(`engine report request failed: ${response.status}`);
  }
  return response.text();
}
