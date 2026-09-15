// SPDX-License-Identifier: MIT
import http from "node:http";
import { readFile } from "node:fs/promises";
import { fileURLToPath } from "node:url";
import path from "node:path";
import { renderPage } from "./render.mjs";

const args = process.argv.slice(2);
const root = path.resolve(
  fileURLToPath(
    new URL(args.includes("--dist") ? "../dist/" : "../", import.meta.url),
  ),
);
const portArg = args.indexOf("--port");
const port = Number(
  portArg >= 0 ? args[portArg + 1] : process.env.PORT || 4173,
);
const types = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".png": "image/png",
  ".svg": "image/svg+xml",
};
const publicFiles = new Set([
  "/index.html",
  "/verification.html",
  "/app.js",
  "/tour.js",
  "/motion.js",
  "/scenes.js",
]);

const server = http.createServer(async (request, response) => {
  if (!["GET", "HEAD"].includes(request.method)) {
    response.writeHead(405, { Allow: "GET, HEAD" }).end();
    return;
  }
  try {
    const url = new URL(request.url, "http://localhost");
    const pathname = decodeURIComponent(
      url.pathname === "/" ? "/index.html" : url.pathname,
    );
    const file = path.resolve(root, `.${pathname}`);
    const allowed =
      publicFiles.has(pathname) ||
      /^\/assets\/[\w-]+\.(png|svg)$/.test(pathname) ||
      /^\/styles\/[\w-]+\.css$/.test(pathname);
    if (!allowed || !file.startsWith(`${root}${path.sep}`)) {
      response.writeHead(404).end("Not found");
      return;
    }
    const contents =
      path.extname(file) === ".html" && !args.includes("--dist")
        ? await renderPage(file)
        : await readFile(file);
    response.writeHead(200, {
      "Content-Type": types[path.extname(file)] || "application/octet-stream",
      "Content-Length": contents.length,
      "X-Content-Type-Options": "nosniff",
      "Cache-Control": "no-cache",
    });
    response.end(request.method === "HEAD" ? undefined : contents);
  } catch (error) {
    response.writeHead(error instanceof URIError ? 400 : 404).end("Not found");
  }
});
server.on("error", (error) => {
  console.error(error.message);
  process.exitCode = 1;
});
server.listen(port, "127.0.0.1", () =>
  console.log(`LogitOS website: http://127.0.0.1:${port}`),
);
