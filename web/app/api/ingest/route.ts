import { NextRequest, NextResponse } from "next/server";
import { enqueueJob } from "../../../lib/scheduler";

export async function POST(request: NextRequest) {
  try {
    const body = await request.json();
    const payload = String(body.payload ?? "");
    const algorithm = String(body.algorithm ?? "dbscan");
    if (!payload.trim()) {
      return NextResponse.json({ error: "payload is required" }, { status: 400 });
    }
    const job = enqueueJob(payload, algorithm);
    return NextResponse.json({
      jobId: job.id,
      status: job.status,
      position: 1,
      etaMs: 0,
    });
  } catch (error) {
    const message = error instanceof Error ? error.message : "enqueue failed";
    return NextResponse.json({ error: message }, { status: 500 });
  }
}
