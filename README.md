# RPCS3 Texture Dumper

An experimental, read-only texture dumping tool for games running in RPCS3.

The current version is built and tested around **SOCOM 4: U.S. Navy SEALs — BCUS98135 v01.00**.

## Features

- Native Windows GUI with no CMD window.
- Automatically finds the running `rpcs3.exe` process.
- Read-only access to RPCS3/game memory.
- Built-in SOCOM 4 RSX memory profile.
- Deep texture capture follows the game's confirmed secondary RSX command buffers.
- Dumps BC1/DXT1, BC2/DXT23 and BC3/DXT45 textures to viewable `.bmp` files.
- SOCOM 4 textures are automatically Flip-Y corrected for normal viewing.
- GUI output contains **BMP files only**.
- Textures are saved directly to the selected game/profile folder with no per-capture subfolders.
- Advanced CLI modes are still available for RSX/FIFO diagnostics.

## Building

The easiest build environment is **Windows + VS Code + MSYS2 UCRT64 GCC**.

Install GCC from an MSYS2 UCRT64 terminal:

```text
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
```

Then open PowerShell in the project folder and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The executable will be created at:

```text
build\RPCS3TextureDumper.exe
```

## Using the GUI

1. Start RPCS3 and boot SOCOM 4.
2. Load into the scene containing the textures you want to dump.
3. Launch `RPCS3TextureDumper.exe`.
4. Leave **Deep texture capture** enabled for the best results.
5. Click **Dump Textures**.

By default, textures are placed in:

```text
dumps\BCUS98135\
```

The GUI publishes only `.bmp` files there. Internal FIFO captures, manifests and raw payloads are temporary and are removed automatically after the previews are produced.

## Current Status

This is an early reverse-engineering tool. SOCOM 4 `BCUS98135` is the currently confirmed game profile and BC/DXT texture formats are the currently verified preview formats.

The dumper does **not** modify RPCS3 or game memory.

For implementation details, RSX/FIFO research, CLI switches and known limitations, see [TECHNICAL.md](TECHNICAL.md).

## Disclaimer

This project is intended for research, preservation and debugging. It is not affiliated with or endorsed by RPCS3, Sony, or the original game developers/publishers.
