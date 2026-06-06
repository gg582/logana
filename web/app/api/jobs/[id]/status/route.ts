import { NextRequest, NextResponse } from "next/server";
import { getSchedulerStatus } from "../../../../../lib/scheduler";

export async function GET(_: NextRequest, { params }: { params: Promise<{ id: string }> }) {
  const { id } = await params;
  const status = getSchedulerStatus(id);
  if (!status) {
    return NextResponse.json({ error: "job not found" }, { status: 404 });
  }
  return NextResponse.json(status);
}
