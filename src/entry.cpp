#include <windows.h>
#include <shellapi.h>

#include "cli.hpp"
#include "modules/gui.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;

    const int result = (argc <= 1) ? gui::run() : cli_main(argc, argv);
    LocalFree(argv);
    return result;
}
