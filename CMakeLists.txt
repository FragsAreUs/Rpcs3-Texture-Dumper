cmake_minimum_required(VERSION 3.20)
project(RPCS3TextureDumper LANGUAGES CXX)

add_executable(RPCS3TextureDumper WIN32
    src/entry.cpp
    src/main.cpp
    src/modules/gui.cpp
    src/modules/profiles.cpp
    src/modules/preview.cpp
)
target_compile_features(RPCS3TextureDumper PRIVATE cxx_std_20)
target_compile_definitions(RPCS3TextureDumper PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(RPCS3TextureDumper PRIVATE user32 gdi32 shell32 ole32)

if(MSVC)
    target_compile_options(RPCS3TextureDumper PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(RPCS3TextureDumper PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MINGW)
    # The GUI-subsystem executable uses a Unicode wWinMain entry point.
    # Link the MinGW runtime statically so the resulting tool can run outside
    # an MSYS2 shell without libgcc/libstdc++ runtime DLLs on PATH.
    target_link_options(RPCS3TextureDumper PRIVATE -municode -static -static-libgcc -static-libstdc++)
endif()
