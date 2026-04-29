import net from "node:net";

const HOST = "127.0.0.1";
const PORT = Number(process.env.LOGANA_ENGINE_PORT ?? 24445);

function runCommand(head: string, payload?: string): Promise<string> {
  return new Promise((resolve, reject) => {
    const socket = net.createConnection({ host: HOST, port: PORT }, () => {
      socket.write(head + "\n");
      if (payload) socket.write(payload);
    });
    let data = "";
    socket.on("data", (chunk) => {
      data += chunk.toString("utf8");
      if (data.includes("\n")) {
        socket.end();
      }
    });
    socket.on("end", () => resolve(data.trim()));
    socket.on("error", reject);
  });
}

export async function ingestLogs(payload: string, algorithm: string) {
  const raw = await runCommand(`INGEST ${algorithm} ${Buffer.byteLength(payload, "utf8")}`, payload);
  return JSON.parse(raw);
}

export async function getJobStatus(jobId: string) {
  const raw = await runCommand(`STATUS ${jobId}`);
  return JSON.parse(raw);
}

export async function getJobResult(jobId: string) {
  const raw = await runCommand(`RESULT ${jobId}`);
  return JSON.parse(raw);
}
