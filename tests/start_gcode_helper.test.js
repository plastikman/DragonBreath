const assert = require("node:assert/strict");
const { processGcode } = require("../tools/start-gcode-helper/processor.js");

const template = [
  "M140 S{bed_temperature_initial_layer_single}",
  "M104 S140",
  "G28 X Y",
  "M190 S{bed_temperature_initial_layer_single}",
  "G90",
  "G0 Z5 F10000",
  "WAIT_CHAMBER_TEMP TIMEOUT=180",
  "G28 Z",
  "BED_MESH_CALIBRATE"
].join("\n");

const result = processGcode(template, "{overall_chamber_temperature}");
assert.equal((result.text.match(/^M141 /gm) || []).length, 1);
assert.equal((result.text.match(/^M191 /gm) || []).length, 1);
assert.match(result.text, /M140 .*\nM141 S\{overall_chamber_temperature\}/);
assert.match(result.text, /G0 Z5 F10000\nM106 P2 S178\nM191 S\{overall_chamber_temperature\}\nM107 P2\n; WAIT_CHAMBER_TEMP TIMEOUT=180\nG28 Z/);

const updated = processGcode(result.text, "55");
assert.equal((updated.text.match(/^M141 /gm) || []).length, 1);
assert.equal((updated.text.match(/^M191 /gm) || []).length, 1);
assert.match(updated.text, /M141 S55/);
assert.match(updated.text, /M106 P2 S178\nM191 S55\nM107 P2/);
assert.equal((updated.text.match(/^; WAIT_CHAMBER_TEMP/gm) || []).length, 1);

const outsideFan = template + "\nM106 P2 S255\n";
assert.match(processGcode(outsideFan, "55").text, /M107 P2\n; WAIT_CHAMBER_TEMP TIMEOUT=180\nG28 Z[\s\S]*M106 P2 S255/);

assert.throws(() => processGcode(template.replace("M190", "; M190"), "55"), /Couldn’t find M190/);
assert.throws(() => processGcode(template, "-1"), /Enter a temperature/);
console.log("start G-code helper tests: PASS");
