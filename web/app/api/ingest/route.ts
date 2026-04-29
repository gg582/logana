import { NextRequest, NextResponse } from "next/server";
import { ingestLogs } from "../../../lib/engine-client";

export async function POST(request: NextRequest) {
  const body = await request.json();
  const payload = String(body.payload ?? "");
  const algorithm = String(body.algorithm ?? "dbscan");
  if (!payload.trim()) {
    return NextResponse.json({ error: "payload is required" }, { status: 400 });
  }
  const data = await ingestLogs(payload, algorithm);
  return NextResponse.json(data);
}
