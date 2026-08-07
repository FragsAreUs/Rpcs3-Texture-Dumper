@echo off
setlocal EnableExtensions

set "PROJECT_ROOT=%~dp0"
set "BUILD_DIR=%PROJECT_ROOT%build"
set "OUTPUT=%BUILD_DIR%\RPCS3TextureDumper.exe"
set "COMPILER="

for %%I in (g++.exe) do set "COMPILER=%%~$PATH:I"
if not defined COMPILER (
    if exist "%SystemDrive%\msys64\ucrt64\bin\g++.exe" (
        set "COMPILER=%SystemDrive%\msys64\ucrt64\bin\g++.exe"
    )
)

if not defined COMPILER (
    echo [!] MinGW-w64 g++ was not found.
    echo [!] Expected it on PATH or at:
    echo [!]   %SystemDrive%\msys64\ucrt64\bin\g++.exe
    echo [!] In the MSYS2 UCRT64 terminal install it with:
    echo [!]   pacman -S --needed mingw-w64-ucrt-x86_64-gcc
    exit /b 1
)

for %%I in ("%COMPILER%") do set "COMPILER_DIR=%%~dpI"
set "PATH=%COMPILER_DIR%;%PATH%"

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if errorlevel 1 (
    echo [!] Could not create the build directory.
    exit /b 1
)

echo [*] Compiler: %COMPILER%
echo [*] Building RPCS3TextureDumper...
echo.

pushd "%PROJECT_ROOT%"
if errorlevel 1 (
    echo [!] Could not enter the project directory.
    exit /b 1
)

"%COMPILER%" ^
    -std=c++20 ^
    -O2 ^
    -Wall ^
    -Wextra ^
    -DUNICODE ^
    -D_UNICODE ^
    -DNOMINMAX ^
    -DWIN32_LEAN_AND_MEAN ^
    "src\cli.cpp" ^
    "src\entry.cpp" ^
    "src\modules\capture_engine.cpp" ^
    "src\modules\diagnostics.cpp" ^
    "src\modules\gui.cpp" ^
    "src\modules\options.cpp" ^
    "src\modules\process_memory.cpp" ^
    "src\modules\profiles.cpp" ^
    "src\modules\preview.cpp" ^
    "src\modules\rsx_maps.cpp" ^
    "src\modules\rsx_texture.cpp" ^
    -mwindows ^
    -municode ^
    -static ^
    -static-libgcc ^
    -static-libstdc++ ^
    -luser32 ^
    -lgdi32 ^
    -lshell32 ^
    -lole32 ^
    -o "%OUTPUT%"

set "BUILD_EXIT=%ERRORLEVEL%"
popd

if not "%BUILD_EXIT%"=="0" (
    echo.
    echo [!] Build failed with exit code %BUILD_EXIT%.
    exit /b %BUILD_EXIT%
)

echo.
echo [+] Built: %OUTPUT%
exit /b 0
