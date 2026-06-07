import { NextRequest, NextResponse } from "next/server";
import { getJobReport } from "../../../../../lib/engine-client";

export async function GET(_: NextRequest, { params }: { params: Promise<{ id: string }> }) {
  const { id } = await params;
  try {
    const html = await getJobReport(id);
    return new NextResponse(html, {
      headers: { "Content-Type": "text/html; charset=utf-8" },
    });
  } catch (error) {
    const message = error instanceof Error ? error.message : "report fetch failed";
    return NextResponse.json({ error: message }, { status: 502 });
  }
}
