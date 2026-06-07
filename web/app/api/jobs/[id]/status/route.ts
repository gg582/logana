import { NextRequest, NextResponse } from "next/server";
import { getJobStatus, getJobResult } from "../../../../../lib/engine-client";

export async function GET(_: NextRequest, { params }: { params: Promise<{ id: string }> }) {
  const { id } = await params;
  try {
    const status = (await getJobStatus(id)) as Record<string, unknown>;
    if (status.status === "completed") {
      const result = (await getJobResult(id)) as Record<string, unknown>;
      status.status = result.status === "failed" ? "failed" : "ready";
    }
    const mapped = {
      ...status,
      position: (status.queuePosition as number) ?? 0,
      etaMs: 0,
    };
    return NextResponse.json(mapped);
  } catch (error) {
    const message = error instanceof Error ? error.message : "status fetch failed";
    return NextResponse.json({ error: message }, { status: 502 });
  }
}
