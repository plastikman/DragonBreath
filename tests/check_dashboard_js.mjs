import fs from "node:fs";

// The dashboard/control UI is owned by dragon-core. In CI the ESP-IDF build
// materializes the pinned component under managed_components; sibling checkouts are
// convenient for local development before that build has run.
const candidates = [
  new URL("../managed_components/dc_ui/www/app.html", import.meta.url),
  new URL("../../dragon-core/components/dc_ui/www/app.html", import.meta.url),
];
const source = candidates.find((p) => fs.existsSync(p));
if (!source) throw new Error("dc_ui SPA not found; run the ESP-IDF dependency build first");
const html = fs.readFileSync(source, "utf8");

const scripts = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/g)].map(
  (m) => m[1],
);
if (!scripts.length) throw new Error("dashboard script not found");

// Syntax-check each inline script (compile only; never executed). The token
// helpers are runtime dependencies, not syntax dependencies.
for (const script of scripts) {
  new Function(script);
}

// Guard against duplicate element ids in the served markup.
const ids = [...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]);
const seen = new Set();
const dup = ids.find((id) => seen.has(id) || (seen.add(id), false));
if (dup) throw new Error(`duplicate element id: ${dup}`);

console.log("dashboard JavaScript syntax: PASS");
