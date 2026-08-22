## Portable Windows release

```powershell
pixi run package
```

The archive and its SHA-256 checksum are written to `dist/`. The ZIP has a
Scoop-friendly layout containing `liminal.exe`, its runtime DLLs, the project
license, and the bundled runtime license notices.

To build and install an independently runnable local copy:

```powershell
pixi run install
```

This stores each distinct build under `~/.local/share/liminal` and updates
`~/.local/bin/liminal.cmd`. Add `~/.local/bin` to `PATH` once if it is not already
there. Content-addressed install directories allow reinstalling while an older
build is still running.
