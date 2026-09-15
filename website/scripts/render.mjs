// SPDX-License-Identifier: MIT
import { readFile } from "node:fs/promises";
import path from "node:path";

// Expand only authored section names. Dev and build share this function so the
// static artifact contains the same complete HTML, including when JS is off.
export async function renderPage(file) {
  const source = await readFile(file, "utf8");
  const markers = [...source.matchAll(/<!-- include:([a-z-]+) -->/g)];
  const fragments = await Promise.all(
    markers.map(async ([marker, name]) => {
      const content = await readFile(
        path.join(path.dirname(file), "sections", `${name}.html`),
        "utf8",
      );
      return [marker, content];
    }),
  );
  let html = source;
  for (const [marker, content] of fragments)
    html = html.replace(marker, () => content);
  return Buffer.from(html);
}
