const http = require("node:http");
const next = require("next");

const dev = false;
const hostname = process.env.HOST || "0.0.0.0";
const port = Number(process.env.PORT || 23345);

const app = next({ dev, hostname, port });
const handle = app.getRequestHandler();

app.prepare().then(() => {
  const server = http.createServer((req, res) => handle(req, res));
  server.listen(port, hostname);
}).catch((error) => {
  console.error("failed to start HTTP/1.1 server", error);
  process.exit(1);
});
