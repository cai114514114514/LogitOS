// SPDX-License-Identifier: MIT
import { cp, mkdir, rm, writeFile } from "node:fs/promises";
import { renderPage } from "./render.mjs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const root = fileURLToPath(new URL("../", import.meta.url));
const output = path.join(root, "dist");
await rm(output, { recursive: true, force: true });
await mkdir(output, { recursive: true });
// Explicit public allowlist: serving the repository would expose build reports
// and local configuration. The deployable artifact only contains this website.
for (const entry of ["app.js", "tour.js", "motion.js", "scenes.js", "styles"]) {
  await cp(path.join(root, entry), path.join(output, entry), {
    recursive: true,
  });
}
for (const entry of ["index.html", "verification.html"]) {
  await writeFile(
    path.join(output, entry),
    await renderPage(path.join(root, entry)),
  );
}
await mkdir(path.join(output, "assets"));
for (const entry of [
  "textedit.png",
  "finder.png",
  "browser.png",
  "islands.png",
  "effects.png",
  "mark.svg",
]) {
  await cp(
    path.join(root, "assets", entry),
    path.join(output, "assets", entry),
  );
}
console.log(`Static site built: ${output}`);
