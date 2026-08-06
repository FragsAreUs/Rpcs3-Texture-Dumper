# Texture Dumper modules

The user-facing program stays focused on texture dumping. These source modules
exist to keep the implementation easy to extend without destabilizing the
working RSX capture code in `main.cpp`.

- `gui.*` - native Win32 texture-dumping front end. It launches the same EXE in
  invisible CLI worker mode, captures output, and owns folder/workflow UI only.
  The worker uses a per-run temporary directory; the GUI publishes only `.bmp`
  files directly into the user-selected profile folder and then removes its temporary
  raw/FIFO/CSV diagnostics.
- `../entry.cpp` - Windows GUI-subsystem entry point. No console is created when
  the tool is launched normally; arguments are routed to the internal CLI
  engine. The GUI invokes that same engine with `CREATE_NO_WINDOW`, so there is
  no separate CLI executable or CMD dependency.
- `preview.*` - verified BC1/BC2/BC3 color-preview decoder and BMP writer.
  Flip-Y is the normal SOCOM 4 preview orientation. Optional orientation
  variants are diagnostic output only.

As the broader RPCS3 reverse-engineering project grows, unrelated features such
as shadow controls, MLAA experiments, patch management, or memory tools should
be separate tools in a future suite rather than added to this GUI.

Good future extraction boundaries inside the texture dumper are `profiles`,
`capture`, and additional format decoders. Move those only when their interfaces
are stable; the current live FIFO/CALL capture path is intentionally left in
`main.cpp` for now because it has been validated against SOCOM 4.
