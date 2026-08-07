# RPCS3 Texture Dumper v0.1

Experimental, read-only Windows runtime texture dumper for RPCS3 with a native portable GUI and the original CLI engine.

The first target is SOCOM 4 (`BCUS98135`), but the scanner is not tied to that title.

## What v0.1 does

- Attaches read-only to `rpcs3.exe`.
- Resolves RPCS3's PS3 VM base using the native-code signature described in the supplied RPCS3 reverse-engineering guide, with a signature-independent PS3 PPU ELF mapping fallback for newer RPCS3 builds.
- Verifies the guest ELF mapping before scanning.
- Can parse `sys_rsx_context_iomap(...)` messages from an uncompressed `RPCS3.log` to reconstruct RSX MAIN-memory IO-to-EA mappings for other titles.
- Can use a built-in `BCUS98135`/`SOCOM4` RSX mapping profile, confirmed across the supplied SOCOM 4 logs, so a live scan does not depend on RPCS3 finalizing its log at shutdown.
- Can scan committed PS3 VM pages for plausible big-endian `CellGcmTexture`-style descriptors (legacy diagnostic).
- Can scan the mapped RSX IO arenas for coherent `NV4097_SET_TEXTURE_*` FIFO packets with `--rsx-scan`. This finds genuine packet shapes, but mapped command-buffer memory can retain historical packets after they stop being active.
- Can scan for `CellGcmControl` (`PUT/GET/REF`) and `CellGcmContextData` (`begin/end/current/callback`) state with `--fifo-scan`, sample it repeatedly, and rank moving candidates first. Control candidates are restricted to high guest mappings so ordinary ELF pointers cannot exhaust the candidate list before RPCS3's `0x50100000` RSX control mapping is reached.
- Can snapshot SOCOM 4's confirmed live primary FIFO with `--fifo-capture`. The corrected live scan identified `CellGcmControl` at guest EA `0x50100040`; this mode reads that control, resolves its current GET/PUT through the confirmed IO map, writes only that bounded primary FIFO window, and scans the snapshot for inline texture packets.
- Can follow the six SOCOM 4 secondary command-buffer CALL targets confirmed by the live FIFO/context correlation with `--fifo-follow-calls`. The observed frame cycle is `0x421E4700+0x4216C580`, `0x421EC780+0x42194600`, `0x421F4800+0x421BC680`, then back to the first pair.
- For linear fragment textures, the RSX scan walks real FIFO packet boundaries to recover `NV4097_SET_TEXTURE_CONTROL3`. SOCOM 4's captured secondary streams write CONTROL3 immediately **after** the texture register block, so that following write is preferred; a prior packet-aware state value is only a fallback for other stream layouts. This also handles CONTROL3 embedded in incrementing multi-method packets while refusing to reinterpret arbitrary argument words as packet headers. The low 20 bits are recorded as row pitch, matching RPCS3's fragment-texture state and preventing non-power-of-two DXT textures from being sized as tightly packed when the game supplied a padded pitch.
- Can scan guest state for packed `CellGcmTileInfo` entries with `--tile-scan`. `--tile-offset` highlights tile regions that cover a specific LOCAL/MAIN RSX offset; this is the diagnostic needed before detiling raw LOCAL-memory captures.
- Resolves both `CELL_GCM_LOCATION_LOCAL` and mapped `CELL_GCM_LOCATION_MAIN` texture sources.
- Deduplicates matching descriptors.
- Dumps the raw texture payload and a CSV manifest containing the descriptor address, resolved guest address, format, dimensions, pitch, remap, mip count and estimated byte size.
- Emits viewable 24-bit `.bmp` previews beside dumped BC1/DXT1 (`0x86`) and BC2/BC3/DXT23/45 (`0x87`/`0x88`) payloads. Live SOCOM 4 testing established Flip-Y as its useful viewing orientation, while Wolfenstein uses neutral orientation. Optional orientation diagnostics emit the other three transforms for the selected profile. Direct CLI dumps keep the original `.bin` untouched; the GUI uses raw files only as temporary decoding input and removes them after publishing BMPs.
- Double-clicking `RPCS3TextureDumper.exe` opens a native texture-dumping GUI with no console/CMD window. The executable is linked as a Windows GUI application; command-line arguments still enter the existing CLI engine, which the GUI uses as its invisible worker.
- The GUI defaults directly to the active profile folder, `dumps\SOCOM 4 - BCUS98135` beside the EXE. BMPs accumulate in that profile folder instead of creating a separate timestamped folder for every capture, making the final static EXE safe to move into its own standalone folder.
- GUI profile folders contain **BMP files only**. FIFO captures, CSV manifests and raw `.bin` payloads are kept in a private temporary worker directory while decoding and are removed after the BMPs are published. The advanced CLI still retains those diagnostic files when invoked directly.

It does **not** write to RPCS3 or game memory.

## Raw + verified previews

RSX textures are not all stored as ordinary linear RGBA pixels. RPCS3 itself has format-specific upload/deswizzle handling, so the CLI diagnostic path preserves the original bytes and metadata. BC1/BC2/BC3 additionally get verified BMP color previews; the GUI publishes those verified previews only. Other formats remain available as raw CLI diagnostics until their SOCOM 4 layout is proven from live captures.

Linear BC previews are pitch-aware. When the texture format carries
`CELL_GCM_TEXTURE_LN` (`0x20`) and CONTROL3 supplied a valid pitch, the decoder
steps between compressed block rows using that RSX pitch instead of assuming a
tightly packed BC surface. This matters for SOCOM 4 `A6/A7/A8` textures: a
confirmed 300x500 `A8` sample uses a 1680-byte row pitch even though its packed
BC3 row width is only 1200 bytes.

RPCS3's current renderer represents decoded fragment/vertex texture state and performs format/layout work in its RSX texture code:

- https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/RSXTexture.cpp
- https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Common/TextureUtils.cpp
- https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/Common/tiled_dma_copy.hpp
- https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/RSX/GCM.h

## Build

You do **not** need the full Visual Studio IDE. Visual Studio Code works well with
MinGW-w64; VS Code itself is the editor and needs a separate C++ compiler.

Recommended VS Code setup:

1. Install the Microsoft **C/C++** extension in VS Code.
2. Install **MSYS2** from https://www.msys2.org/.
3. Open the **MSYS2 UCRT64** terminal and update/install GCC:

```text
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-gcc
```

If the first command asks you to close/reopen MSYS2, do that, reopen the UCRT64
terminal, then run the install command.

4. In a normal VS Code **PowerShell** terminal, open this project directory and run:

```powershell
.\build.ps1
```

If PowerShell says script execution is disabled, you can run this one script
without changing your permanent policy:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

`build.ps1` automatically uses `g++` from PATH or the normal
`C:\msys64\ucrt64\bin\g++.exe` installation. When the normal MSYS2 install is
detected, the script also adds its `ucrt64\bin` directory to the build-process
PATH automatically so GCC's internal compiler, assembler, linker and runtime
DLLs can be found from an ordinary VS Code PowerShell terminal.

The executable is written to `build\RPCS3TextureDumper.exe`. No CMake install is
required for this route. MinGW builds statically link the GCC/C++ runtime so the
resulting executable does not require MSYS2 runtime DLLs on PATH when launched.

You can also build with CMake on Windows.

## GUI texture dumping

After building, simply double-click:

```text
build\RPCS3TextureDumper.exe
```

The GUI is intentionally focused on texture dumping only. It exposes:

- selectable SOCOM 4 / `BCUS98135` v01.00, MAG / `BCUS98110` v02.12, and one region-neutral Wolfenstein v01.02 profile accepting `BLES00564` or `BLUS30298`
- centralized `src/modules/profiles.*` registry for per-game identity, RSX maps,
  CellGcmControl address and Deep Capture command-buffer configuration
- output-root folder picker
- **Dump Textures**, **Stop**, and **Open Output Folder** buttons
- profile-aware deep texture capture: SOCOM 4 follows its confirmed secondary command buffers, MAG scans its five confirmed RendererRing allocations, and Wolfenstein scans its segmented one-megabyte command ring
- automatic GUI capture tuning; budget, recent-history size, sample/delay, and maximum-texture controls remain CLI diagnostics
- profile-aware default orientation: user-confirmed Flip-Y for SOCOM 4/MAG and neutral for Wolfenstein, with optional diagnostic variants
- a live capture log

### Hidden CLI worker architecture

The project deliberately keeps a CLI capture engine inside the same executable. `src/entry.cpp` routes a normal no-argument launch to the GUI and an argument-bearing launch to `cli_main()`. The CLI is therefore not a second executable and does not imply a visible console window.

The GUI launches the same EXE with the proven CLI capture arguments using redirected standard output/error and `CREATE_NO_WINDOW`. This keeps the Win32 GUI small while ensuring GUI captures and developer diagnostics share one RSX/FIFO implementation. It also avoids duplicating the most timing-sensitive part of the dumper. Neither the front end nor its worker needs Windows CMD.

The worker process only opens RPCS3 for query/read access. Stopping the capture terminates only that read-only worker, not RPCS3. Developers can still invoke CLI switches explicitly from PowerShell when collecting RSX/FIFO diagnostics.

The CLI is an orchestration layer rather than a capture monolith:

- `options.*` parses and validates CLI settings.
- `process_memory.*` owns RPCS3 discovery, handles, VM-base detection, and reads.
- `rsx_maps.*` resolves profile/log IO mappings.
- `rsx_texture.*` owns reusable texture packet parsing, pitch, sizing, and dumps.
- `diagnostics.*` contains broad GCM/RSX investigative scans.
- `capture_engine.*` contains the supported live capture strategies.

For the confirmed SOCOM 4 profile, GUI `--auto-tune` currently uses a 1 GiB
temporary payload budget, an 8 MiB recent-FIFO window, up to 100 samples at
100 ms intervals, and a 4096-candidate ceiling. The FIFO loop exits as soon as
it has a useful sample, so the sample count is a retry ceiling rather than a
fixed delay. CLI diagnostics can omit `--auto-tune` and set these values
individually when investigating capture behavior.

GUI captures publish directly into the selected profile folder, for example:

```text
RPCS3TextureDumper.exe
dumps\
  SOCOM 4 - BCUS98135\
    tex_0000_....bmp
    tex_0001_....bmp
```

Only BMPs are published into GUI profile folders. Before publication, the GUI indexes BMPs already in the profile folder and the BMPs accepted earlier in the current run. A fast content fingerprint narrows duplicate candidates, then a full byte-for-byte comparison confirms equality before a file is skipped. This prevents different RSX descriptor addresses from producing multiple copies of the same decoded image without relying on the fingerprint alone. Existing files are never deleted by deduplication. Repeated captures reuse the same profile folder; a texture with the same generated filename is refreshed in place when its contents differ. The worker's raw `.bin`, FIFO and CSV data lives under the system temporary directory during the capture and is removed when publishing finishes. Direct CLI use keeps the existing raw/CSV diagnostic behavior for reverse-engineering work.

This layout is designed for the eventual standalone tool: move the statically linked EXE to its own folder and it will create/manage its dump folders relative to itself. No RPCS3 installation path is required; the tool finds the running `rpcs3.exe` process.

## First SOCOM 4 test

1. Start RPCS3 and boot SOCOM 4.
2. Load into the scene containing the texture you want.
3. Keep the game running.
4. First locate the moving GCM FIFO state using the confirmed SOCOM 4 profile:

```bat
RPCS3TextureDumper.exe --profile BCUS98135 --fifo-scan --out socom4_fifo_scan1
```

Keep the game unpaused while this runs. It takes eight snapshots, 250 ms apart, and writes:

- `gcm_controls.csv`
- `gcm_contexts.csv`

Rows with changing `GET`/`PUT` or `current` values are the strongest live candidates. Share both CSVs before using the historical command scan as evidence for a specific texture.

The supplied corrected SOCOM 4 scan has now confirmed the real moving control block at guest EA `0x50100040`. Its GET/PUT values resolve through IO map 0 into the `0x401xxxxx` primary command stream. Capture the current primary execution window while the target scene is actively rendering with:

```bat
RPCS3TextureDumper.exe --profile BCUS98135 --fifo-capture --list-only --out socom4_fifo_capture1
```

This writes:

- `active_fifo.csv` — the exact control, GET/PUT/REF, translated EAs and captured size
- `active_fifo.bin` — the bounded primary GET-to-PUT command bytes
- `active_fifo_textures.csv` — coherent texture packets found inline in that live window
- `fifo_history.bin` — up to 2 MiB of the immediately preceding primary FIFO plus the active span, bounded by the current IO map; this catches state-setting commands that RSX consumed just before the sampled GET

For best odds, keep the camera moving slightly while running the command. If GET equals PUT, capture mode retries automatically using `--fifo-samples` and `--fifo-sample-ms` (eight attempts, 250 ms apart by default). Wrapped GET/PUT samples are also rejected and retried instead of aborting the capture; if the final sample is wrapped, the most recent safe linear sample is used when one was observed. Ring-boundary reconstruction itself is still intentionally not guessed. `--control-ea HEX` can override the confirmed SOCOM 4 control address for diagnostics or other titles.

`--fifo-history-mb N` changes how far capture mode looks immediately behind GET (default 2 MiB, maximum 64 MiB). Unlike `--rsx-scan`, this history is anchored to the live control position instead of treating all mapped command-buffer memory as equally current.

For SOCOM 4, add `--fifo-follow-calls` to capture the secondary command buffers referenced by CALL packets found in that recent primary history. Capture mode now keeps sampling when it sees an active primary span but the history has not yet reached one of the six confirmed SOCOM 4 CALL words, instead of immediately accepting an early-frame snapshot. This writes `fifo_calls.csv` plus one `fifo_call_XXXXXXXX.bin` for each confirmed secondary IO target that appeared in the captured history. Each called buffer is parsed only through its first packet-boundary RSX `RETURN`, so stale tail bytes are excluded from texture discovery. `fifo_called_textures.csv` records the unique live texture bindings found there, and `--dump` writes those referenced payloads within the normal dump budget. Supported BC payloads also receive `.bmp` previews automatically.

For the easter-egg/sign hunt, use:

```bat
RPCS3TextureDumper.exe --profile BCUS98135 --fifo-capture --fifo-follow-calls --dump --out socom4_sign_capture
```

5. A manifest-only RSX command scan is still useful for inventorying packet history:

```bat
RPCS3TextureDumper.exe --profile BCUS98135 --rsx-scan --list-only --out socom4_rsx_scan1
```

The dumper prints the VM base and each RSX IO mapping, then scans those mapped RSX arenas and writes:

- `rsx_textures.csv`
- raw `.bin` payloads only when `--dump` is requested

Please inspect/share `rsx_textures.csv` before enabling payload dumping. Once the bindings look sane, dump raw payloads with the built-in 1 GiB write budget:

```bat
RPCS3TextureDumper.exe --profile BCUS98135 --rsx-scan --out socom4_textures --dump
```

Use `--budget-mb 256` (or another value) to change that cap.

### SOCOM 4 tiled-memory diagnostic

If a LOCAL texture has valid BC/DXT metadata but decodes as scrambled blocks, scan the live guest tile state while the same scene is loaded. For the current 300x500 candidate at LOCAL offset `0x04585F80`:

```bat
RPCS3TextureDumper.exe --profile BCUS98135 --tile-scan --tile-offset 0x04585F80 --out socom4_tile_scan1
```

This writes `rsx_tiles.csv` and prints any packed tile region that covers the requested offset. For the supplied SOCOM 4 capture, the real contiguous table at `0x01BA0AE0..0x01BA0BC0` does **not** cover LOCAL offset `0x04585F80`, so the current 300x500 candidate is not RSX-tiled. Its scrambled payload is therefore more consistent with a historical FIFO packet whose LOCAL allocation was later reused/overwritten.

For other games, `--log PATH` remains available when an uncompressed log containing
the title's `sys_rsx_context_iomap` calls is available. v0.1 intentionally has
no zlib dependency.

## Useful switches

```text
--process rpcs3.exe       Process name (default: rpcs3.exe)
--profile NAME            Built-in RSX mapping profile (SOCOM4, MAG, WOLFENSTEIN)
--log PATH                RPCS3.log path
--out DIR                 Output directory (default: rpcs3_texture_dump)
--guest-start HEX         Descriptor scan start EA (default: 0x00010000)
--guest-end HEX           Descriptor scan end EA (default: 0x80000000)
--vm-base HEX             Override auto-detected host VM base
--max N                   Maximum unique dumps (default: 2000)
--auto-tune               Choose safe capture limits/retry timing automatically
--dump                    Write raw payloads; supported BC formats also get BMP previews
--preview-variants        Also emit the three non-default BMP orientations
--budget-mb N             Total payload write budget (default: 1024 MB)
--fifo-scan               Find moving CellGcmControl/context state (recommended next SOCOM 4 test)
--fifo-capture            Snapshot the active primary GET-to-PUT FIFO window
--fifo-follow-calls       Dump confirmed SOCOM 4 secondary buffers called by recent history
--renderer-ring           Scan RendererRing buffers configured by the selected profile
--mag-renderer-ring       Legacy alias for --renderer-ring
--control-ea HEX          Override CellGcmControl EA (SOCOM 4 default: 0x50100040)
--fifo-history-mb N       Recently executed FIFO history before GET (default: 2 MB; max: 64)
--fifo-sample-ms N        Delay between FIFO snapshots/capture retries (default: 250 ms; range: 16..5000)
--fifo-samples N          Number of FIFO snapshots/capture attempts (default: 8; range: 2..100)
--rsx-scan                Scan mapped RSX memory for fragment-texture FIFO packet history
--tile-scan               Scan guest state for packed CellGcmTileInfo entries
--tile-offset HEX         Highlight tile regions covering this RSX offset
--descriptor-scan         Use the original CellGcmTexture-like structure heuristic
--list-only               Force manifest-only mode
```

For SOCOM 4, the built-in profile contains the mappings confirmed across the supplied logs, including IO `0x00000000 -> EA 0x40000000` and IO `0x03600000 -> EA 0x43600000`. Other titles can still learn mappings from a completed RPCS3 log.

For MAG `BCUS98110` v02.12, the supplied runtime log confirms the renderer's post-launch `mage_g.self` RSX mappings: IO `0x00000000 -> EA 0x50000000` for `0x02300000` bytes and IO `0x0E000000 -> EA 0x34E00000` for `0x00100000` bytes. Its RSX context maps at `0x60100000`, giving the profile a `CellGcmControl` EA of `0x60100040`. MAG's TTY reports five RendererRing command-buffer ranges: `0x001000..0x0A4000`, `0x0A4000..0x0D3000`, `0x0D3000..0x11B000`, `0x11B000..0x197000` and `0x197000..0x200000`. `--renderer-ring` scans the allocations stored by the selected profile directly on each Deep Capture. The supplied 218-BMP sample contained bindings from only buffers 0 and 3, demonstrating why active-FIFO-only capture was sensitive to camera movement. Ring scanning intentionally accepts cached command-buffer contents when they still reference readable textures; GUI publication removes byte-identical BMP duplicates.

For Wolfenstein `BLES00564`/`BLUS30298` v01.02, the supplied runtime logs identify PPU hash `eb633143c2ad46e28e8b2d2374b0a331168c2c66` and confirm IO `0x00000000 -> EA 0x70000000` for `0x00100000` bytes plus IO `0x00100000 -> EA 0x70100000` for `0x01D00000` bytes. The supplied BLUS `EBOOT.elf` and `wolfsp.elf` are byte-identical, and both regional runtime logs report the same PPU hash and RSX layout. The RSX context maps at `0x80100000`, and live testing confirms the standard `CellGcmControl` EA of `0x80100040`. The first live captures produced valid moving primary FIFO windows and recent history but no inline texture bindings. Control-flow diagnostics then confirmed JUMPs chaining consecutive `0x8000`-byte segments throughout IO `0x00000000..0x00100000`, identifying a segmented one-megabyte command ring. Wolfenstein's CommandRing Deep Capture scans that confirmed range directly; GUI deduplication removes repeated BMP payloads from cached command data.

## Expected limitations

- The legacy VM-base signature follows RPCS3 native code and can break after emulator changes. v0.1 now falls back to RPCS3's 4 GiB-aligned guest VM layout and verifies the ELF64 big-endian PowerPC64 header at guest EA `0x10000`; `--vm-base` remains available for diagnostics.
- Descriptor scanning is heuristic. SOCOM 4's full 32-bit guest scan produced only two questionable matches, so it is retained only as a legacy diagnostic.
- The first RSX packet scanner deliberately accepts only incremental FIFO packets that start at `SET_TEXTURE_OFFSET` and include at least `OFFSET..IMAGE_RECT`. It scans mapped command-buffer memory, not the hardware GET-to-PUT execution window, so a valid-looking row can describe an old binding whose texture memory has already been reused. `--fifo-scan` is the first step toward constraining later packet parsing to the active FIFO.
- `--fifo-scan` is intentionally diagnostic and repeatedly samples RPCS3's current public GCM layouts. The corrected SOCOM 4 capture identified the moving `CellGcmControl` at `0x50100040` and moving secondary contexts around `0x421xxxxx`.
- `--fifo-capture` snapshots the primary FIFO only from a safe linear sample where GET and PUT resolve through the same known IO map and GET is not numerically beyond PUT. Wrapped samples are retried automatically rather than reconstructed as a guessed ring span. With `--fifo-follow-calls`, the six confirmed SOCOM 4 secondary `0x421xxxxx` command contexts are followed; arbitrary CALL/JUMP graphs for other games are not reconstructed yet.
- RSX MAIN mappings must either come from a built-in profile or appear in the supplied log. A texture whose mapping is absent cannot be resolved automatically.
- LOCAL texture addressing uses RPCS3's normal `0xC0000000 + offset` guest mapping convention.
- v0.1 still estimates payload size, but the RSX route now recovers packet-associated `SET_TEXTURE_CONTROL3` pitch state for linear textures when available. `--tile-scan` can recover packed tile-region metadata, but raw texture payloads are not automatically detiled yet. Mip chains, cubemaps, unusual formats and command streams that set pitch state outside the captured packet window will need more state reconstruction in later versions.
- Automatic previews currently cover the BC/DXT color data only. Alpha is intentionally omitted from the 24-bit BMP preview. Direct CLI dumps preserve the original compressed `.bin` unchanged; GUI worker raws are temporary and are removed after BMP publication.

For the next iteration, keep `textures.csv` and a few `.bin` files corresponding to obvious large textures; those will tell us exactly which SOCOM formats/deswizzle paths to implement first.
