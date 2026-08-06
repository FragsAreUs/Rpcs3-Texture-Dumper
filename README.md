# RPCS3 Texture Dumper

An experimental, read-only texture dumping tool for games running in RPCS3.

Current built-in profiles are **SOCOM 4: U.S. Navy SEALs — BCUS98135 v01.00** and **MAG — BCUS98110 v02.12**. MAG support includes its confirmed primary FIFO mapping plus a five-buffer RendererRing Deep Capture path.

## Features

- Native Windows GUI with no CMD window.
- Automatically finds the running `rpcs3.exe` process.
- Read-only access to RPCS3/game memory.
- Built-in SOCOM 4 and MAG RSX memory profiles.
- Supported games are registered in the dedicated `src/modules/profiles.*`
  module, so adding a profile does not require hard-coded GUI selection logic.
- Automatic capture tuning in the GUI; no budget/history/sample/delay/max values to guess.
- Deep texture capture is profile-aware: SOCOM 4 follows its confirmed secondary RSX command buffers, while MAG scans all five TTY-confirmed RendererRing command-buffer ranges in one capture instead of waiting for GET/PUT to visit them.
- Dumps BC1/DXT1, BC2/DXT23 and BC3/DXT45 textures to viewable `.bmp` files.
- Respects RSX row pitch for linear BC textures such as `A6/A7/A8`, using packet-aware `CONTROL3` state tracking to avoid padded-row preview corruption.
- SOCOM 4 textures are automatically Flip-Y corrected for normal viewing.
- GUI output contains **BMP files only**.
- Textures are saved directly to the selected game/profile folder with no per-capture subfolders.
- Byte-for-byte duplicate BMPs are skipped both within the current capture and when the same image already exists in the profile folder.
- Contains a hidden CLI capture engine used internally by the GUI and available from PowerShell for advanced RSX/FIFO diagnostics.

## Building

### Requirements

- Windows 10/11 x64
- [MSYS2](https://www.msys2.org/) with the UCRT64 GCC toolchain
- Visual Studio Code is optional and can be used as the editor; the full Visual Studio IDE is **not** required.

### 1. Update MSYS2 first

After installing MSYS2, open an **MSYS2 UCRT64** terminal and update the installation before installing the compiler:

```text
pacman -Syu
```

If MSYS2 asks you to close the terminal during the update, close it, reopen the **MSYS2 UCRT64** terminal, and run the update again:

```text
pacman -Syu
```

Finish the MSYS2 update before continuing.

### 2. Install the compiler

In the updated **MSYS2 UCRT64** terminal, install GCC:

```text
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
```

### 3. Build the dumper

Open a normal **PowerShell** terminal in the project folder and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

`build.ps1` looks for `g++` on your PATH and also automatically checks the standard MSYS2 location:

```text
C:\msys64\ucrt64\bin\g++.exe
```

The build statically links the MinGW GCC/C++ runtime and creates the Windows GUI executable at:

```text
build\RPCS3TextureDumper.exe
```

The script reports each source-file compile and the final link as separate live
stages. GCC warnings and errors are streamed directly to the PowerShell window,
and the completed build reports its total elapsed time.

No Windows CMD window, batch build script, or Visual Studio installation is required.

## Using the GUI

1. Start RPCS3 and boot a supported game.
2. Load into the scene containing the textures you want to dump.
3. Launch `RPCS3TextureDumper.exe`.
4. Select the matching **Game profile** (SOCOM 4 or MAG). The window title,
   output folder and capture mode update from that profile.
5. Leave **Deep texture capture** enabled for the best results. On MAG this scans all five confirmed RendererRing command buffers in one capture.
6. Click **Dump Textures**.

MAG Deep Capture inventories the five renderer command buffers reported by the
game (`0x001000..0x200000`) on every run. This substantially reduces the need
to move the camera just to make a different renderer buffer become active.
Inactive ring buffers can retain cached command data, so Deep Capture can also
recover valid textures that are not visible in the exact current frame; normal
BMP deduplication removes repeated image content.

Capture limits and FIFO retry timing are selected automatically for the active
profile. Normal GUI use does not require entering numeric tuning values.

Choose the matching game profile in the GUI. By default, textures are placed in that profile's folder, for example:

```text
dumps\BCUS98135\
dumps\BCUS98110\
```

The GUI publishes only `.bmp` files there. Internal FIFO captures, manifests and raw payloads are temporary and are removed automatically after the previews are produced.

## Why there is a hidden CLI engine

`RPCS3TextureDumper.exe` is a Windows GUI application and does **not** require a CMD window. When launched normally with no arguments it opens the GUI. When launched with command-line arguments, the same EXE routes those arguments to its internal CLI capture engine.

The GUI uses that engine as an invisible worker process with `CREATE_NO_WINDOW` and redirects its output back into the GUI capture log. Keeping the capture engine this way lets the GUI reuse the same proven RSX/FIFO code used by our diagnostic commands instead of maintaining a second copy of the capture logic.

Normal texture dumping does not require CMD or direct CLI use. The CLI remains useful for development and reverse-engineering diagnostics and can be invoked from PowerShell when needed.

Manual budget, FIFO-history, sample-count, delay and candidate-limit switches
remain available only as advanced CLI diagnostics. The GUI uses `--auto-tune`
so those implementation details do not need to be adjusted by hand.

## Current Status

This is an early reverse-engineering tool. SOCOM 4 `BCUS98135` is the mature profile; MAG `BCUS98110` v02.12 has confirmed RSX mappings, primary FIFO capture and five-buffer RendererRing Deep Capture support. BC/DXT texture formats are the currently verified preview formats.

The dumper does **not** modify RPCS3 or game memory.

For implementation details, RSX/FIFO research, CLI switches and known limitations, see [TECHNICAL.md](TECHNICAL.md).

## Disclaimer

This project is intended for research, preservation and debugging. It is not affiliated with or endorsed by RPCS3, Sony, or the original game developers/publishers.
