import { NextRequest, NextResponse } from "next/server";

export async function GET(_: NextRequest) {
  return NextResponse.json(
    { error: "SSE stream is deprecated. Use GET /jobs/:id/status for polling." },
    { status: 410 }
  );
}
