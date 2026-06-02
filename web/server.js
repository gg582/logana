const http = require("node:http");
const next = require("next");

const dev = false;
const hostname = process.env.HOST || "0.0.0.0";
const port = Number(process.env.PORT || 23345);

const app = next({ dev, hostname, port });
const handle = app.getRequestHandler();

app.prepare().then(() => {
  const server = http.createServer((req, res) => handle(req, res));
  server.on("upgrade", (_req, socket) => {
    socket.end(
      "HTTP/1.1 426 Upgrade Required\r\n" +
      "Connection: close\r\n" +
      "Content-Type: application/json\r\n" +
      "\r\n" +
      "{\"error\":\"HTTP upgrade is not supported\"}"
    );
  });
  server.listen(port, hostname);
}).catch((error) => {
  console.error("failed to start HTTP/1.1 server", error);
  process.exit(1);
});
