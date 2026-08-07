# RPCS3 Texture Dumper

A read-only Windows tool for dumping textures from supported games running in
RPCS3. Launch it normally for the GUI; no console window or manual memory
configuration is required.

## Supported games

| Game | Serial | Version | Deep Capture | Default orientation |
| --- | --- | --- | --- | --- |
| SOCOM 4: U.S. Navy SEALs | `BCUS98135` | 01.00 | Secondary RSX command buffers | Flip-Y |
| MAG | `BCUS98110` | 02.12 | Five RendererRing buffers | Flip-Y |
| Wolfenstein | `BLES00564` / `BLUS30298` | 01.02 | Segmented 1 MB command ring | Neutral |

## Highlights

- Native Windows GUI that automatically finds the running `rpcs3.exe`.
- Query/read-only access; the dumper never writes to RPCS3 or game memory.
- Automatic capture tuning and game-specific Deep Capture.
- BMP previews for BC1/DXT1, BC2/DXT23 and BC3/DXT45 textures.
- Correct handling of padded RSX block rows using captured `CONTROL3` pitch.
- Profile-aware texture orientation and byte-for-byte BMP deduplication.
- Clean GUI output folders containing BMP files only.

## Build

### Requirements

- Windows 10 or 11 x64
- [MSYS2](https://www.msys2.org/) with the UCRT64 GCC toolchain

Open the **MSYS2 UCRT64** terminal and install or update GCC:

```text
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
```

If the first command asks you to restart MSYS2, reopen the UCRT64 terminal and
finish the update before installing GCC.

From the project folder, run:

```cmd
.\build.cmd
```

The script finds `g++` on `PATH` or at the standard MSYS2 location,
`C:\msys64\ucrt64\bin\g++.exe`. It creates a statically linked GUI executable:

```text
build\RPCS3TextureDumper.exe
```

No PowerShell policy change, Visual Studio installation, or CMake installation
is required.

## Use

1. Start RPCS3 and boot a supported game.
2. Load the scene containing the textures you want.
3. Launch `build\RPCS3TextureDumper.exe`.
4. Select the matching game profile.
5. Leave **Deep texture capture** enabled for the most complete results.
6. Click **Dump Textures**.

Deep Capture scans known game-specific command buffers instead of relying only
on the current FIFO window. This can recover cached textures outside the exact
current frame; duplicate image content is removed automatically.

## Output

Textures accumulate in a shared folder for the selected profile:

```text
dumps\SOCOM 4 - BCUS98135\
dumps\MAG - BCUS98110\
dumps\Wolfenstein - BLES00564-BLUS30298\
```

The GUI publishes only decoded `.bmp` files. Raw payloads, FIFO snapshots and
CSV manifests are temporary during GUI captures and are removed afterward.

## Advanced diagnostics

The GUI runs the same executable as a hidden command-line worker. Developers
can launch `RPCS3TextureDumper.exe` with arguments to retain raw payloads,
manifests and RSX/FIFO diagnostics.

For CLI switches, architecture, capture research and known limitations, see
[TECHNICAL.md](TECHNICAL.md).

## Status

This is an early reverse-engineering tool. SOCOM 4 is the most mature profile;
MAG and Wolfenstein have live-confirmed RSX mappings and Deep Capture paths.
Automatic previews currently cover verified BC/DXT formats.

## Credits

Development assistance provided by OpenAI Codex.

## Disclaimer

This project is intended for research, preservation and debugging. It is not
affiliated with or endorsed by RPCS3, Sony, or the original game developers or
publishers.
