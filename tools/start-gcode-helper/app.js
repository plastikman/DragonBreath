(function () {
  const fileInput = document.getElementById("file-input");
  const source = document.getElementById("source");
  const target = document.getElementById("target");
  const status = document.getElementById("status");
  const dropzone = document.getElementById("dropzone");
  const filename = document.getElementById("filename");
  const resultCard = document.getElementById("result-card");
  const output = document.getElementById("output");
  let downloadName = "start-gcode-updated.gcode";

  function showError(message) {
    status.textContent = message;
    status.classList.add("error");
    resultCard.hidden = true;
  }

  function readFile(file) {
    if (!file) return;
    if (file.size > 20 * 1024 * 1024) {
      showError("This file is over 20 MB. Choose a start G-code file or paste the start section instead.");
      return;
    }
    const reader = new FileReader();
    reader.onload = () => {
      source.value = String(reader.result || "");
      downloadName = file.name.replace(/\.(?:gcode|gco|gc|g|txt)$/i, "") + "-updated.gcode";
      filename.textContent = file.name;
      status.textContent = "File loaded locally. Review the text, then add the commands.";
      status.classList.remove("error");
      resultCard.hidden = true;
    };
    reader.onerror = () => showError("The file could not be read. Try pasting the start G-code instead.");
    reader.readAsText(file);
  }

  fileInput.addEventListener("change", () => readFile(fileInput.files[0]));
  for (const eventName of ["dragenter", "dragover"]) {
    dropzone.addEventListener(eventName, (event) => {
      event.preventDefault();
      dropzone.classList.add("dragover");
    });
  }
  for (const eventName of ["dragleave", "drop"]) {
    dropzone.addEventListener(eventName, (event) => {
      event.preventDefault();
      dropzone.classList.remove("dragover");
    });
  }
  dropzone.addEventListener("drop", (event) => readFile(event.dataTransfer.files[0]));

  document.getElementById("process").addEventListener("click", () => {
    try {
      const result = window.StartGcodeProcessor.processGcode(source.value, target.value);
      output.value = result.text;
      document.getElementById("result-summary").textContent = result.message;
      status.textContent = "Done. The original text is unchanged; review the updated version below.";
      status.classList.remove("error");
      resultCard.hidden = false;
    } catch (error) {
      showError(error.message);
    }
  });

  document.getElementById("download").addEventListener("click", () => {
    const blob = new Blob([output.value], { type: "text/plain;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const link = document.createElement("a");
    link.href = url;
    link.download = downloadName;
    link.click();
    URL.revokeObjectURL(url);
  });
})();
