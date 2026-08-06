$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$sources = @(
    (Join-Path $projectRoot "src\cli.cpp")
    (Join-Path $projectRoot "src\entry.cpp")
    (Join-Path $projectRoot "src\modules\capture_engine.cpp")
    (Join-Path $projectRoot "src\modules\diagnostics.cpp")
    (Join-Path $projectRoot "src\modules\gui.cpp")
    (Join-Path $projectRoot "src\modules\options.cpp")
    (Join-Path $projectRoot "src\modules\process_memory.cpp")
    (Join-Path $projectRoot "src\modules\profiles.cpp")
    (Join-Path $projectRoot "src\modules\preview.cpp")
    (Join-Path $projectRoot "src\modules\rsx_maps.cpp")
    (Join-Path $projectRoot "src\modules\rsx_texture.cpp")
)
$buildDir = Join-Path $projectRoot "build"
$output = Join-Path $buildDir "RPCS3TextureDumper.exe"

$gpp = Get-Command g++ -ErrorAction SilentlyContinue
if ($gpp) {
    $compiler = $gpp.Source
} else {
    $defaultMsys2 = Join-Path $env:SystemDrive "msys64\ucrt64\bin\g++.exe"
    if (Test-Path $defaultMsys2) {
        $compiler = $defaultMsys2
    } else {
        Write-Host "[!] MinGW-w64 g++ was not found." -ForegroundColor Red
        Write-Host "[!] Expected it at: $defaultMsys2"
        Write-Host "[!] In the MSYS2 UCRT64 terminal install it with:"
        Write-Host "[!]   pacman -S --needed mingw-w64-ucrt-x86_64-gcc"
        exit 1
    }
}

# MSYS2's GCC driver launches cc1plus/as/ld and loads DLLs from ucrt64\bin.
# When VS Code's PowerShell inherited no MSYS2 PATH, g++.exe itself could run
# while those child tools failed.  Make the selected toolchain self-contained.
$compilerDir = Split-Path -Parent $compiler
$pathEntries = $env:Path -split ";"
if ($pathEntries -notcontains $compilerDir) {
    $env:Path = "$compilerDir;$env:Path"
}

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

Write-Host "[*] Compiler: $compiler"
Write-Host "[*] Building RPCS3TextureDumper..."
Write-Host ""

$commonCompileArgs = @(
    "-std=c++20"
    "-O2"
    "-Wall"
    "-Wextra"
    "-DUNICODE"
    "-D_UNICODE"
    "-DNOMINMAX"
    "-DWIN32_LEAN_AND_MEAN"
)

$objects = @()
$totalSteps = $sources.Count + 1
$step = 0
$buildTimer = [System.Diagnostics.Stopwatch]::StartNew()

foreach ($source in $sources) {
    $step++
    $sourceName = Split-Path -Leaf $source
    $objectName = [System.IO.Path]::GetFileNameWithoutExtension($sourceName) + ".o"
    $object = Join-Path $buildDir $objectName
    $objects += $object

    Write-Host ("[{0}/{1}] Compiling {2}..." -f $step, $totalSteps, $sourceName) -ForegroundColor Cyan
    $compileArgs = @()
    $compileArgs += $commonCompileArgs
    $compileArgs += @(
        "-c"
        $source
        "-o"
        $object
    )

    # Invoke GCC directly so warnings/errors are streamed to this PowerShell
    # window as GCC produces them instead of being captured and printed later.
    & $compiler @compileArgs
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        Write-Host ""
        Write-Host "[!] Compilation failed in $sourceName with exit code $exitCode." -ForegroundColor Red
        exit $exitCode
    }
}

$step++
Write-Host ("[{0}/{1}] Linking RPCS3TextureDumper.exe..." -f $step, $totalSteps) -ForegroundColor Cyan
$linkArgs = @(
    "-mwindows"
    "-municode"
    "-static"
    "-static-libgcc"
    "-static-libstdc++"
)
$linkArgs += $objects
$linkArgs += @(
    "-luser32"
    "-lgdi32"
    "-lshell32"
    "-lole32"
    "-o"
    $output
)

& $compiler @linkArgs
$exitCode = $LASTEXITCODE
if ($exitCode -ne 0) {
    Write-Host ""
    Write-Host "[!] Link failed with exit code $exitCode." -ForegroundColor Red
    exit $exitCode
}

# Object files are only needed by the linker. Keep them when a build fails so
# they remain available for diagnostics, but remove them after a successful
# link so compiler intermediates do not clutter the build folder.
Write-Host "[+] Cleaning intermediate object files..."
foreach ($object in $objects) {
    Remove-Item -LiteralPath $object -Force -ErrorAction SilentlyContinue
}

$buildTimer.Stop()
Write-Host ""
Write-Host "[+] Built: $output" -ForegroundColor Green
Write-Host ("[+] Build completed in {0:N1} seconds." -f $buildTimer.Elapsed.TotalSeconds) -ForegroundColor Green
