# Texture Dumper modules

The implementation is split by responsibility so game profiles, capture
strategies, diagnostics, and format support can evolve independently.

- `gui.*` - native Win32 texture-dumping front end. It launches the same EXE in
  invisible CLI worker mode, captures output, and owns folder/workflow UI only.
  Numeric capture tuning is intentionally automatic in the GUI; manual budget,
  FIFO-history, retry and candidate limits stay in the hidden diagnostic CLI.
  The worker uses a per-run temporary directory; the GUI publishes only `.bmp`
  files directly into the user-selected profile folder and then removes its temporary
  raw/FIFO/CSV diagnostics. Publication also rejects byte-for-byte duplicate BMPs
  already present in the profile folder or earlier in the same capture.
- `../cli.cpp` and `options.*` - command-line orchestration, argument parsing,
  validation, and automatic capture tuning. The CLI connects modules but owns
  no RSX parsing or process-memory algorithms.
- `process_memory.*` - read-only RPCS3 process discovery, handle lifetime,
  module lookup, guest VM-base discovery, and verified remote reads.
- `profiles.*` - the single registry for supported games. A profile owns its
  title ID/version, GUI label, aliases, CellGcmControl EA, RSX IO maps, Deep
  Capture type, and known command-buffer ranges. The GUI builds its profile
  selector from this registry instead of hard-coding SOCOM 4 or MAG.
- `rsx_maps.*` - parsing and resolution of RSX IO-to-EA mappings from either a
  game profile or an RPCS3 log.
- `rsx_texture.*` - shared texture descriptors, FIFO packet parsing, CONTROL3
  pitch recovery, payload sizing, raw dumping, and preview dispatch.
- `diagnostics.*` - opt-in tile, GCM control/context, and broad RSX command
  scans. These are isolated from the normal game-specific capture path.
- `capture_engine.*` - primary FIFO, SOCOM secondary CALL-buffer, MAG
  RendererRing, and legacy descriptor capture strategies. It consumes the
  shared process, mapping, texture, and profile interfaces.
- `preview.*` - verified BC1/BC2/BC3 color-preview decoder and BMP writer.
  Flip-Y remains the normal SOCOM 4/MAG preview orientation, while Wolfenstein
  uses neutral output. Linear BC formats use the captured RSX CONTROL3 pitch
  so padded block rows decode correctly. Optional orientation variants are
  diagnostic output only.
- `../entry.cpp` - minimal Windows GUI-subsystem entry point. No console is
  created for a normal GUI launch; argument-bearing launches route to
  `cli_main()` in the same executable.

As the broader RPCS3 reverse-engineering project grows, unrelated features such
as shadow controls, MLAA experiments, patch management, or memory tools should
be separate tools in a future suite rather than added to this GUI.

To add a game, extend `profiles.*` and add a capture strategy only if its RSX
command-buffer behavior differs from the existing strategies. To add a texture
format, extend `rsx_texture.*` and `preview.*`; GUI and process discovery should
not need changes.
