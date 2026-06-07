import { getJobStatus } from "../../../../../lib/engine-client";

export async function GET(_: Request, { params }: { params: Promise<{ id: string }> }) {
  const { id } = await params;
  const stream = new ReadableStream({
    async start(controller) {
      const encoder = new TextEncoder();
      while (true) {
        const status = await getJobStatus(id);
        controller.enqueue(encoder.encode(`data: ${JSON.stringify(status)}\n\n`));
        if (status.status === "ready" || status.status === "failed" || status.error) {
          break;
        }
        await new Promise((resolve) => setTimeout(resolve, 500));
      }
      controller.close();
    },
  });

  return new Response(stream, {
    headers: {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-cache, no-transform",
      Connection: "keep-alive",
    },
  });
}
