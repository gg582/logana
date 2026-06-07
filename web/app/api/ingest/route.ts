import { NextRequest, NextResponse } from "next/server";
import { ingestLogs } from "../../../lib/engine-client";

export async function POST(request: NextRequest) {
  try {
    const body = await request.json();
    const payload = String(body.payload ?? "");
    const algorithm = String(body.algorithm ?? "dbscan");
    const options = body.options && typeof body.options === "object" ? body.options : undefined;
    if (!payload.trim()) {
      return NextResponse.json({ error: "payload is required" }, { status: 400 });
    }
    const data = await ingestLogs(payload, algorithm, options);
    return NextResponse.json(data);
  } catch (error) {
    const message = error instanceof Error ? error.message : "ingest failed";
    return NextResponse.json({ error: message }, { status: 500 });
  }
}
