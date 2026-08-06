$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
$sources = @(
    (Join-Path $projectRoot "src\entry.cpp")
    (Join-Path $projectRoot "src\main.cpp")
    (Join-Path $projectRoot "src\modules\gui.cpp")
    (Join-Path $projectRoot "src\modules\preview.cpp")
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

$compilerArgs = @(
    "-std=c++20"
    "-O2"
    "-Wall"
    "-Wextra"
    "-mwindows"
    "-municode"
    "-static"
    "-static-libgcc"
    "-static-libstdc++"
    "-DUNICODE"
    "-D_UNICODE"
    "-DNOMINMAX"
    "-DWIN32_LEAN_AND_MEAN"
)
$compilerArgs += $sources
$compilerArgs += @(
    "-luser32"
    "-lgdi32"
    "-lshell32"
    "-lole32"
    "-o"
    $output
)

& $compiler @compilerArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "[!] Build failed with exit code $LASTEXITCODE." -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host ""
Write-Host "[+] Built: $output" -ForegroundColor Green
