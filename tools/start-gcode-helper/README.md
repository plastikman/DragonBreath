# Start G-code Helper

A dependency-free static page for adding DragonBreath chamber control to a
printer's start G-code. It runs completely in the browser; files are never
uploaded to a server.

The helper places `M141 S<target>` after the first `M140`, then looks for the
first `G0 Z5` after `M190` and adds this wait sequence beneath it:

```gcode
M106 P2 S178
M191 S<target>
M107 P2
```

`S178` is 70% of the 0–255 fan scale. The target defaults to the Orca/Bambu
template variable `{overall_chamber_temperature}`. For already-sliced G-code,
enter the numeric chamber target instead. If a chamber wait already exists in
the startup area, the helper updates it and its adjacent P2 fan commands rather
than adding a second wait. It also comments out an active `WAIT_CHAMBER_TEMP`
in that startup section because `M191` now performs the wait. Unrelated fan
commands later in the print are kept.

The page expects start G-code containing `M140` and `M190`. Review the preview
before downloading and using the updated file.

## GitHub Pages

The repository workflow `.github/workflows/start-gcode-helper-pages.yml`
publishes this directory to GitHub Pages when it changes on `main`. In the
repository settings, set **Pages → Build and deployment → Source** to **GitHub
Actions**. The page is then available at the DragonBreath GitHub Pages URL.
