#include "gui.hpp"
#include "profiles.hpp"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#endif

namespace fs = std::filesystem;

namespace
{
constexpr wchar_t kClassName[] = L"RPCS3TextureDumperWindow";
constexpr UINT WM_DUMPER_LOG = WM_APP + 1;
constexpr UINT WM_DUMPER_DONE = WM_APP + 2;

enum ControlId
{
    ID_OUTPUT = 100,
    ID_PROFILE,
    ID_BROWSE,
    ID_DUMP,
    ID_STOP,
    ID_OPEN,
    ID_DEEP,
    ID_VARIANTS,
    ID_LOG,
    ID_STATUS
};

struct State
{
    HWND window = nullptr;
    HWND output = nullptr;
    HWND dump = nullptr;
    HWND stop = nullptr;
    HWND log = nullptr;
    HWND status = nullptr;
    HWND profile = nullptr;
    PROCESS_INFORMATION child{};
    HANDLE pipe_read = nullptr;
    fs::path profile_output;
    fs::path worker_output;
    bool running = false;
};

State g;

std::wstring exe_path()
{
    std::wstring result(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
    result.resize(n);
    return result;
}

std::wstring default_output_path()
{
    return (fs::path(exe_path()).parent_path() / L"dumps" / profiles::default_profile().id).wstring();
}

const profiles::GameProfile& selected_profile()
{
    if (!g.profile) return profiles::default_profile();
    const LRESULT selected = SendMessageW(g.profile, CB_GETCURSEL, 0, 0);
    const auto available = profiles::all();
    if (selected == CB_ERR || static_cast<std::size_t>(selected) >= available.size())
        return profiles::default_profile();
    return available[static_cast<std::size_t>(selected)];
}

std::wstring selected_profile_id()
{
    return std::wstring(selected_profile().id);
}

std::wstring selected_profile_name()
{
    return std::wstring(selected_profile().game_name);
}

std::wstring default_output_path_for_selected_profile()
{
    return (fs::path(exe_path()).parent_path() / L"dumps" / selected_profile_id()).wstring();
}

void refresh_profile_ui()
{
    if (!g.window) return;
    const auto& profile = selected_profile();
    EnableWindow(GetDlgItem(g.window, ID_DEEP), profile.deep_capture != profiles::DeepCaptureKind::none);
    SetWindowTextW(g.output, default_output_path_for_selected_profile().c_str());
    const std::wstring game(profile.game_name);
    SetWindowTextW(g.status, (L"Ready - start RPCS3 and load " + game + L".").c_str());
    SetWindowTextW(g.window, (L"RPCS3 Texture Dumper - " + game).c_str());
}

std::wstring worker_run_name()
{
    SYSTEMTIME t{};
    GetLocalTime(&t);
    wchar_t name[64]{};
    std::swprintf(name, sizeof(name) / sizeof(name[0]), L"run_%04u%02u%02u_%02u%02u%02u",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return name;
}

std::wstring text_of(HWND h)
{
    const int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<std::size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(n);
    return s;
}

std::wstring control_text(int id)
{
    return text_of(GetDlgItem(g.window, id));
}

void append_log(const std::wstring& s)
{
    const int len = GetWindowTextLengthW(g.log);
    SendMessageW(g.log, EM_SETSEL, len, len);
    SendMessageW(g.log, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(s.c_str()));
    SendMessageW(g.log, EM_SCROLLCARET, 0, 0);
}

std::wstring quote(const std::wstring& s)
{
    std::wstring out = L"\"";
    for (wchar_t c : s)
    {
        if (c == L'\"') out += L'\\';
        out += c;
    }
    out += L'\"';
    return out;
}

std::wstring decode_pipe_text(const char* data, int size)
{
    if (size <= 0) return {};
    int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, size, nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (chars <= 0)
    {
        cp = CP_ACP;
        flags = 0;
        chars = MultiByteToWideChar(cp, flags, data, size, nullptr, 0);
    }
    if (chars <= 0) return {};
    std::wstring out(static_cast<std::size_t>(chars), L'\0');
    MultiByteToWideChar(cp, flags, data, size, out.data(), chars);
    return out;
}

void set_running(bool running)
{
    g.running = running;
    EnableWindow(g.dump, !running);
    EnableWindow(g.stop, running);
    const std::wstring ready = L"Ready - start RPCS3 and load " + selected_profile_name() + L".";
    SetWindowTextW(g.status, running ? L"Capturing textures from RPCS3..." : ready.c_str());
}

void finish_child()
{
    if (g.child.hThread) { CloseHandle(g.child.hThread); g.child.hThread = nullptr; }
    if (g.child.hProcess) { CloseHandle(g.child.hProcess); g.child.hProcess = nullptr; }
    if (g.pipe_read) { CloseHandle(g.pipe_read); g.pipe_read = nullptr; }
    set_running(false);
}

void cleanup_worker_output()
{
    if (g.worker_output.empty()) return;
    // This is always the exact per-capture directory created by launch_dump()
    // beneath the system temporary directory.
    std::error_code ec;
    fs::remove_all(g.worker_output, ec);
    g.worker_output.clear();
}

struct PublishResult
{
    std::size_t published = 0;
    std::size_t duplicates = 0;
};

bool fingerprint_file(const fs::path& path, std::uint64_t& fingerprint)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    // FNV-1a is used only to find likely duplicate candidates. A full
    // byte-for-byte comparison below is always required before a BMP is skipped.
    std::uint64_t hash = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::array<char, 64 * 1024> buffer{};

    while (in)
    {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = in.gcount();
        for (std::streamsize i = 0; i < count; ++i)
        {
            hash ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(i)]);
            hash *= prime;
        }
    }

    if (!in.eof()) return false;
    fingerprint = hash;
    return true;
}

bool files_are_identical(const fs::path& a, const fs::path& b)
{
    std::error_code ec;
    const std::uintmax_t size_a = fs::file_size(a, ec);
    if (ec) return false;
    const std::uintmax_t size_b = fs::file_size(b, ec);
    if (ec || size_a != size_b) return false;

    std::ifstream left(a, std::ios::binary);
    std::ifstream right(b, std::ios::binary);
    if (!left || !right) return false;

    std::array<char, 64 * 1024> left_buffer{};
    std::array<char, 64 * 1024> right_buffer{};
    std::uintmax_t remaining = size_a;
    while (remaining != 0)
    {
        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uintmax_t>(remaining, left_buffer.size()));
        left.read(left_buffer.data(), static_cast<std::streamsize>(chunk));
        right.read(right_buffer.data(), static_cast<std::streamsize>(chunk));
        const std::streamsize left_count = left.gcount();
        const std::streamsize right_count = right.gcount();
        if (left_count != static_cast<std::streamsize>(chunk) || right_count != static_cast<std::streamsize>(chunk))
            return false;
        if (std::memcmp(left_buffer.data(), right_buffer.data(), chunk) != 0)
            return false;
        remaining -= chunk;
    }
    return true;
}

using BmpIndex = std::unordered_map<std::uint64_t, std::vector<fs::path>>;

bool is_known_duplicate(const fs::path& candidate, std::uint64_t fingerprint, const BmpIndex& index)
{
    const auto found = index.find(fingerprint);
    if (found == index.end()) return false;
    for (const fs::path& existing : found->second)
    {
        if (files_are_identical(candidate, existing)) return true;
    }
    return false;
}

PublishResult publish_bmp_output()
{
    PublishResult result{};
    if (g.worker_output.empty() || g.profile_output.empty()) return result;
    std::error_code ec;
    fs::create_directories(g.profile_output, ec);
    if (ec)
    {
        cleanup_worker_output();
        return result;
    }

    // Index every BMP already published for this profile so repeat captures do
    // not create another file for identical image contents.
    BmpIndex known_bmps;
    for (fs::directory_iterator it(g.profile_output, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code item_ec;
        if (!it->is_regular_file(item_ec) || item_ec) continue;
        const fs::path existing = it->path();
        if (_wcsicmp(existing.extension().c_str(), L".bmp") != 0) continue;
        std::uint64_t fingerprint = 0;
        if (fingerprint_file(existing, fingerprint))
            known_bmps[fingerprint].push_back(existing);
    }
    ec.clear();

    for (fs::directory_iterator it(g.worker_output, ec), end; !ec && it != end; it.increment(ec))
    {
        std::error_code item_ec;
        if (!it->is_regular_file(item_ec) || item_ec) continue;
        const fs::path src = it->path();
        if (_wcsicmp(src.extension().c_str(), L".bmp") != 0) continue;

        std::uint64_t fingerprint = 0;
        const bool fingerprinted = fingerprint_file(src, fingerprint);
        if (fingerprinted && is_known_duplicate(src, fingerprint, known_bmps))
        {
            ++result.duplicates;
            continue;
        }

        const fs::path destination = g.profile_output / src.filename();
        std::error_code copy_ec;
        fs::copy_file(src, destination, fs::copy_options::overwrite_existing, copy_ec);
        if (copy_ec) continue;

        ++result.published;
        if (fingerprinted)
            known_bmps[fingerprint].push_back(destination);
    }

    // The public profile folder receives BMPs only; discard the worker's
    // manifests, FIFO captures and raw payloads after publishing previews.
    cleanup_worker_output();
    return result;
}

void reader_thread(HANDLE pipe, HANDLE process, HWND window)
{
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(pipe, buffer, sizeof(buffer), &got, nullptr) && got)
    {
        auto* message = new std::wstring(decode_pipe_text(buffer, static_cast<int>(got)));
        PostMessageW(window, WM_DUMPER_LOG, 0, reinterpret_cast<LPARAM>(message));
    }
    WaitForSingleObject(process, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process, &exit_code);
    PostMessageW(window, WM_DUMPER_DONE, exit_code, 0);
}

bool launch_dump()
{
    if (g.running) return false;
    const std::wstring output_root = control_text(ID_OUTPUT);
    if (output_root.empty())
    {
        MessageBoxW(g.window, L"Choose an output folder first.", L"RPCS3 Texture Dumper", MB_OK | MB_ICONWARNING);
        return false;
    }

    std::error_code ec;
    const std::wstring run_name = worker_run_name();
    // The selected output is already the active profile folder
    // (for example dumps\BCUS98135). Publish BMPs directly into it.
    g.profile_output = fs::path(output_root);
    g.worker_output = fs::temp_directory_path(ec) / L"RPCS3TextureDumper_work" /
                      (run_name + L"_" + std::to_wstring(GetCurrentProcessId()));
    if (ec)
    {
        MessageBoxW(g.window, L"The temporary working folder could not be resolved.", L"RPCS3 Texture Dumper", MB_OK | MB_ICONERROR);
        return false;
    }
    fs::create_directories(g.profile_output, ec);
    if (ec)
    {
        MessageBoxW(g.window, L"The output folder could not be created.", L"RPCS3 Texture Dumper", MB_OK | MB_ICONERROR);
        return false;
    }
    fs::create_directories(g.worker_output, ec);
    if (ec)
    {
        MessageBoxW(g.window, L"The temporary working folder could not be created.", L"RPCS3 Texture Dumper", MB_OK | MB_ICONERROR);
        return false;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE pipe_write = nullptr;
    if (!CreatePipe(&g.pipe_read, &pipe_write, &sa, 0))
    {
        cleanup_worker_output();
        return false;
    }
    SetHandleInformation(g.pipe_read, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_input == INVALID_HANDLE_VALUE)
    {
        CloseHandle(pipe_write);
        CloseHandle(g.pipe_read); g.pipe_read = nullptr;
        cleanup_worker_output();
        return false;
    }

    const std::wstring exe = exe_path();
    const auto& profile = selected_profile();
    const std::wstring profile_id(profile.id);
    const std::wstring profile_name(profile.game_name);
    std::wostringstream cmd;
    cmd << quote(exe)
        << L" --profile " << profile_id << L" --fifo-capture --dump --auto-tune"
        << L" --out " << quote(g.worker_output.wstring());
    if (profile.deep_capture == profiles::DeepCaptureKind::socom_secondary_calls &&
        SendMessageW(GetDlgItem(g.window, ID_DEEP), BM_GETCHECK, 0, 0) == BST_CHECKED)
        cmd << L" --fifo-follow-calls";
    if (profile.deep_capture == profiles::DeepCaptureKind::renderer_ring &&
        SendMessageW(GetDlgItem(g.window, ID_DEEP), BM_GETCHECK, 0, 0) == BST_CHECKED)
        cmd << L" --renderer-ring";
    if (SendMessageW(GetDlgItem(g.window, ID_VARIANTS), BM_GETCHECK, 0, 0) == BST_CHECKED)
        cmd << L" --preview-variants";

    std::wstring command = cmd.str();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = pipe_write;
    si.hStdError = pipe_write;
    si.hStdInput = null_input;

    ZeroMemory(&g.child, sizeof(g.child));
    const BOOL ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, fs::path(exe).parent_path().c_str(), &si, &g.child);
    CloseHandle(pipe_write);
    CloseHandle(null_input);
    if (!ok)
    {
        CloseHandle(g.pipe_read); g.pipe_read = nullptr;
        cleanup_worker_output();
        MessageBoxW(g.window, L"Could not start the texture capture engine.", L"RPCS3 Texture Dumper", MB_OK | MB_ICONERROR);
        return false;
    }

    SetWindowTextW(g.log, L"");
    append_log(L"RPCS3 Texture Dumper - " + profile_name + L"\r\nOutput: " + g.profile_output.wstring() + L"\r\nStarting capture...\r\n");
    if (profile.deep_capture == profiles::DeepCaptureKind::renderer_ring &&
        SendMessageW(GetDlgItem(g.window, ID_DEEP), BM_GETCHECK, 0, 0) == BST_CHECKED)
        append_log(L"[*] Deep Capture scans every configured RendererRing command buffer for this profile.\r\n");
    append_log(L"\r\n");
    set_running(true);
    std::thread(reader_thread, g.pipe_read, g.child.hProcess, g.window).detach();
    return true;
}

void choose_folder()
{
    BROWSEINFOW bi{};
    bi.hwndOwner = g.window;
    bi.lpszTitle = L"Choose where dumped textures will be saved";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(g.output, path);
    CoTaskMemFree(pidl);
}

HWND add_control(const wchar_t* cls, const wchar_t* text, DWORD style,
                 int x, int y, int w, int h, int id)
{
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, g.window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           GetModuleHandleW(nullptr), nullptr);
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        g.window = hwnd;
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        auto add = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id)
        {
            HWND c = add_control(cls, text, style, x, y, w, h, id);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            return c;
        };

        add(L"STATIC", L"RPCS3 Texture Dumper", SS_LEFT, 18, 14, 260, 22, -1);
        g.status = add(L"STATIC", L"Ready - start RPCS3 and load a supported game.", SS_LEFT, 18, 39, 650, 20, ID_STATUS);

        add(L"STATIC", L"Game profile", SS_LEFT, 18, 72, 100, 20, -1);
        g.profile = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 122, 68, 300, 240, ID_PROFILE);
        for (const auto& profile : profiles::all())
        {
            const std::wstring label(profile.display_name);
            SendMessageW(g.profile, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(g.profile, CB_SETCURSEL, 0, 0);

        add(L"STATIC", L"Output folder", SS_LEFT, 18, 106, 100, 20, -1);
        g.output = add(L"EDIT", default_output_path().c_str(), WS_BORDER | ES_AUTOHSCROLL, 122, 102, 505, 24, ID_OUTPUT);
        add(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 636, 101, 92, 26, ID_BROWSE);

        HWND deep = add(L"BUTTON", L"Deep texture capture (when supported)", BS_AUTOCHECKBOX, 18, 143, 270, 22, ID_DEEP);
        SendMessageW(deep, BM_SETCHECK, BST_CHECKED, 0);
        add(L"BUTTON", L"Write orientation diagnostics", BS_AUTOCHECKBOX, 295, 143, 225, 22, ID_VARIANTS);

        add(L"STATIC", L"Capture tuning: Automatic - no manual values required", SS_LEFT, 18, 181, 520, 20, -1);

        g.dump = add(L"BUTTON", L"Dump Textures", BS_DEFPUSHBUTTON, 18, 210, 130, 32, ID_DUMP);
        g.stop = add(L"BUTTON", L"Stop", BS_PUSHBUTTON, 158, 210, 90, 32, ID_STOP);
        EnableWindow(g.stop, FALSE);
        add(L"BUTTON", L"Open Output Folder", BS_PUSHBUTTON, 258, 210, 150, 32, ID_OPEN);

        add(L"STATIC", L"Capture log", SS_LEFT, 18, 258, 100, 20, -1);
        g.log = add(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                    18, 280, 710, 273, ID_LOG);
        refresh_profile_ui();
        return 0;
    }
    case WM_SIZE:
        if (g.log)
        {
            RECT rc{}; GetClientRect(hwnd, &rc);
            MoveWindow(g.log, 18, 280, std::max(100L, rc.right - 36), std::max(80L, rc.bottom - 298), TRUE);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == ID_PROFILE && HIWORD(wparam) == CBN_SELCHANGE)
        {
            refresh_profile_ui();
            return 0;
        }
        switch (LOWORD(wparam))
        {
        case ID_BROWSE: choose_folder(); return 0;
        case ID_DUMP: launch_dump(); return 0;
        case ID_STOP:
            if (g.running && g.child.hProcess)
            {
                append_log(L"\r\n[!] Stopping capture...\r\n");
                TerminateProcess(g.child.hProcess, 2);
            }
            return 0;
        case ID_OPEN:
        {
            fs::path folder = g.profile_output.empty() ? fs::path(control_text(ID_OUTPUT)) : g.profile_output;
            std::error_code ec; fs::create_directories(folder, ec);
            ShellExecuteW(hwnd, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        }
        break;
    case WM_DUMPER_LOG:
    {
        auto* s = reinterpret_cast<std::wstring*>(lparam);
        if (s) { append_log(*s); delete s; }
        return 0;
    }
    case WM_DUMPER_DONE:
    {
        const DWORD code = static_cast<DWORD>(wparam);
        const PublishResult publish = publish_bmp_output();
        std::wostringstream ss;
        ss << L"\r\n" << (code == 0 ? L"[+] Texture capture finished successfully.\r\n" : L"[!] Texture capture exited with code ") ;
        if (code != 0) ss << code << L".\r\n";
        ss << L"[+] Published " << publish.published << L" new BMP texture(s) to:\r\n    " << g.profile_output.wstring() << L"\r\n";
        if (publish.duplicates != 0)
            ss << L"[*] Skipped " << publish.duplicates << L" duplicate BMP texture(s).\r\n";
        append_log(ss.str());
        finish_child();
        return 0;
    }
    case WM_CLOSE:
        if (g.running)
        {
            if (MessageBoxW(hwnd, L"A texture capture is still running. Stop it and exit?", L"RPCS3 Texture Dumper",
                            MB_YESNO | MB_ICONQUESTION) != IDYES) return 0;
            if (g.child.hProcess)
            {
                TerminateProcess(g.child.hProcess, 2);
                WaitForSingleObject(g.child.hProcess, 5000);
            }
            cleanup_worker_output();
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
}

namespace gui
{
int run()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, kClassName, L"RPCS3 Texture Dumper",
                                WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 770, 630,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd)
    {
        CoUninitialize();
        return 1;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
}
