import { NextRequest, NextResponse } from "next/server";
import { getJobResult } from "../../../../lib/engine-client";

export async function GET(_: NextRequest, { params }: { params: Promise<{ id: string }> }) {
  const { id } = await params;
  const data = await getJobResult(id);
  return NextResponse.json(data);
}
