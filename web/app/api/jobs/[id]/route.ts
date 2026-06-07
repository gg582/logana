import { NextRequest, NextResponse } from "next/server";
import { getJobResult } from "../../../../lib/engine-client";

export async function GET(_: NextRequest, { params }: { params: Promise<{ id: string }> }) {
  const { id } = await params;
  try {
    const data = await getJobResult(id);
    if (!data) {
      return NextResponse.json({ error: "job not found" }, { status: 404 });
    }
    return NextResponse.json(data);
  } catch (error) {
    const message = error instanceof Error ? error.message : "result fetch failed";
    return NextResponse.json({ error: message }, { status: 502 });
  }
}
