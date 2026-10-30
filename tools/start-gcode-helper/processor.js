(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  if (root) root.StartGcodeProcessor = api;
})(typeof globalThis === "undefined" ? this : globalThis, function () {
  "use strict";

  function command(line) {
    return line.split(";")[0].trim().replace(/^(?:N\d+\s+)?/, "").toUpperCase();
  }

  function isCommand(line, code) {
    return new RegExp("^" + code + "(?:\\s|$)", "i").test(command(line));
  }

  function isSafeTemperature(value) {
    const text = String(value).trim();
    if (/^\{[A-Za-z_][A-Za-z0-9_]*\}$/.test(text)) return true;
    if (!/^\d{1,3}(?:\.\d+)?$/.test(text)) return false;
    const number = Number(text);
    return number >= 0 && number <= 120;
  }

  function processGcode(input, chamberTarget) {
    if (typeof input !== "string" || !input.trim()) throw new Error("Choose a G-code file or paste your start G-code first.");
    const target = String(chamberTarget || "").trim();
    if (!isSafeTemperature(target)) throw new Error("Enter a temperature from 0 to 120 °C, or a slicer variable such as {overall_chamber_temperature}.");

    const newline = input.includes("\r\n") ? "\r\n" : "\n";
    const hadTrailingNewline = /(?:\r\n|\n)$/.test(input);
    const lines = input.replace(/\r\n/g, "\n").split("\n");
    if (hadTrailingNewline) lines.pop();

    const bedTargetIndex = lines.findIndex((line) => isCommand(line, "M140"));
    if (bedTargetIndex < 0) throw new Error("Couldn’t find an M140 bed-temperature command. Add the app’s changes to start G-code that includes M140 and M190.");
    let bedWaitIndex = lines.findIndex((line, index) => index > bedTargetIndex && isCommand(line, "M190"));
    if (bedWaitIndex < 0) bedWaitIndex = lines.findIndex((line) => isCommand(line, "M190"));
    if (bedWaitIndex < 0) throw new Error("Couldn’t find M190, which marks the bed wait. The file was left unchanged.");

    let endIndex = lines.findIndex((line, index) => index > bedWaitIndex && (isCommand(line, "G28") || isCommand(line, "BED_MESH_CALIBRATE")));
    if (endIndex < 0) endIndex = lines.length;

    const alreadyHasChamberTarget = lines.slice(0, endIndex).some((line) => isCommand(line, "M141"));
    const alreadyHasChamberWait = lines.slice(0, endIndex).some((line) => isCommand(line, "M191"));
    const alreadyHasHelperFan = lines.slice(bedWaitIndex + 1, endIndex).some((line) => /^M106\s+P2\s+S178(?:\s|$)/i.test(command(line)));
    if (alreadyHasChamberTarget || alreadyHasChamberWait || alreadyHasHelperFan) {
      throw new Error("This start G-code already contains DragonBreath chamber commands. Use the original, unmodified slicer start G-code; do not run this helper on code that has already been modified.");
    }

    lines.splice(bedTargetIndex + 1, 0, "M141 S" + target);
    if (bedTargetIndex < bedWaitIndex) bedWaitIndex++;
    endIndex = lines.findIndex((line, index) => index > bedWaitIndex && (isCommand(line, "G28") || isCommand(line, "BED_MESH_CALIBRATE")));
    if (endIndex < 0) endIndex = lines.length;
    const safeZIndex = lines.findIndex((line, index) => index > bedWaitIndex && index < endIndex && /^G0\s+Z5(?:\.0+)?(?:\s|$)/i.test(command(line)));
    let insertAt = safeZIndex >= 0 ? safeZIndex + 1 : bedWaitIndex + 1;
    let legacyWaitsCommented = 0;
    for (let i = bedWaitIndex + 1; i < endIndex; i++) {
      if (isCommand(lines[i], "WAIT_CHAMBER_TEMP")) {
        lines[i] = lines[i].replace(/^(\s*)/, "$1; ");
        legacyWaitsCommented++;
      }
    }

    lines.splice(insertAt, 0, "M106 P2 S178", "M191 S" + target, "M107 P2");
    let output = lines.join(newline);
    if (hadTrailingNewline) output += newline;
    return {
      text: output,
      message: "Added the chamber target and 70% fan-assisted chamber wait" +
        (legacyWaitsCommented ? " and commented out the old WAIT_CHAMBER_TEMP." : ".")
    };
  }

  return { processGcode };
});
