// mc2_launcher.cpp — MC2 OpenGL Campaign Mod Launcher
//
// Scans ./mods/ for mod.json files, shows a selection dialog,
// then launches mc2.exe with MC2_ACTIVE_MOD=<id> in its environment.
//
// Selecting "Base Game" leaves MC2_ACTIVE_MOD unset — mc2.exe runs in
// pure base-game mode with no mod overlay active.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define UNICODE 0
#include <windows.h>
#include <stdio.h>
#include <string.h>

#define IDC_MODLIST 1001
#define IDC_LAUNCH  1002
#define IDC_CANCEL  1003
#define IDC_LABEL   1004
#define IDC_DESC    1005

#define MAX_MODS 64

struct ModEntry {
    char id[128];
    char name[256];
    char type[32];
    char description[512];
};

static ModEntry s_mods[MAX_MODS];
static int      s_modCount = 0;
static HINSTANCE s_hInst;
static HWND     s_hListBox;
static HWND     s_hDesc;

// ---- Minimal JSON field extractor ----------------------------------------

static bool JsonGetString(const char* json, const char* key, char* out, int outSz) {
    char needle[256];
    _snprintf(needle, sizeof(needle), "\"%s\"", key);
    needle[sizeof(needle)-1] = '\0';
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;
    const char* end = strchr(p, '"');
    if (!end) return false;
    int len = (int)(end - p);
    if (len >= outSz) len = outSz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

// ---- Mod discovery --------------------------------------------------------

static void ScanMods(const char* modsPath) {
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof(pattern), "%s*", modsPath);
    pattern[sizeof(pattern)-1] = '\0';

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        if (s_modCount >= MAX_MODS) break;

        char jsonPath[MAX_PATH];
        _snprintf(jsonPath, sizeof(jsonPath), "%s%s\\mod.json", modsPath, fd.cFileName);
        jsonPath[sizeof(jsonPath)-1] = '\0';

        HANDLE fh = CreateFileA(jsonPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE) continue;

        char buf[4096] = {};
        DWORD bytesRead = 0;
        ReadFile(fh, buf, sizeof(buf)-1, &bytesRead, NULL);
        CloseHandle(fh);
        buf[bytesRead] = '\0';

        ModEntry& e = s_mods[s_modCount];
        if (!JsonGetString(buf, "id",   e.id,   sizeof(e.id)))
            _snprintf(e.id, sizeof(e.id), "%s", fd.cFileName);
        if (!JsonGetString(buf, "name", e.name, sizeof(e.name)))
            _snprintf(e.name, sizeof(e.name), "%s", fd.cFileName);
        if (!JsonGetString(buf, "type", e.type, sizeof(e.type)))
            strcpy(e.type, "campaign");
        if (!JsonGetString(buf, "description", e.description, sizeof(e.description)))
            e.description[0] = '\0';

        s_modCount++;
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

// ---- Launch mc2.exe -------------------------------------------------------

static void DoLaunch(HWND hwnd) {
    int sel = (int)SendMessageA(s_hListBox, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) sel = 0;

    if (sel == 0) {
        // Base Game: clear MC2_ACTIVE_MOD from this process env so child
        // inherits an unset variable.
        SetEnvironmentVariableA("MC2_ACTIVE_MOD", NULL);
    } else if (sel <= s_modCount) {
        SetEnvironmentVariableA("MC2_ACTIVE_MOD", s_mods[sel-1].id);
    }

    // Look for mc2.exe in the same directory as this launcher.
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, sizeof(exePath));
    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) lastSlash = strrchr(exePath, '/');
    if (lastSlash) *(lastSlash+1) = '\0';
    else exePath[0] = '\0';

    char mc2Path[MAX_PATH];
    _snprintf(mc2Path, sizeof(mc2Path), "%smc2.exe", exePath);
    mc2Path[sizeof(mc2Path)-1] = '\0';

    // Set CWD to the deploy directory so mc2.exe relative paths work.
    if (exePath[0]) SetCurrentDirectoryA(exePath);

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(mc2Path, NULL, NULL, NULL, FALSE,
                        0, NULL, NULL, &si, &pi)) {
        char msg[512];
        _snprintf(msg, sizeof(msg), "Failed to launch %s\n\nError code: %lu", mc2Path, GetLastError());
        MessageBoxA(hwnd, msg, "Launch Error", MB_ICONERROR | MB_OK);
        return;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    PostQuitMessage(0);
}

// ---- Window procedure -----------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // Label
        HWND hLabel = CreateWindowA("STATIC",
            "Select a campaign to launch, or choose Base Game:",
            WS_CHILD | WS_VISIBLE, 10, 10, 360, 18, hwnd, (HMENU)IDC_LABEL, s_hInst, NULL);
        SendMessageA(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Listbox
        s_hListBox = CreateWindowA("LISTBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            10, 34, 360, 160, hwnd, (HMENU)IDC_MODLIST, s_hInst, NULL);
        SendMessageA(s_hListBox, WM_SETFONT, (WPARAM)hFont, TRUE);

        SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)"Base Game (no mod)");
        for (int i = 0; i < s_modCount; i++)
            SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)s_mods[i].name);
        SendMessageA(s_hListBox, LB_SETCURSEL, 0, 0);

        // Description static
        s_hDesc = CreateWindowA("STATIC", "",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, 200, 360, 36, hwnd, (HMENU)IDC_DESC, s_hInst, NULL);
        SendMessageA(s_hDesc, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Buttons
        HWND hLaunch = CreateWindowA("BUTTON", "Launch",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            10, 244, 170, 28, hwnd, (HMENU)IDC_LAUNCH, s_hInst, NULL);
        SendMessageA(hLaunch, WM_SETFONT, (WPARAM)hFont, TRUE);

        HWND hCancel = CreateWindowA("BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE,
            200, 244, 170, 28, hwnd, (HMENU)IDC_CANCEL, s_hInst, NULL);
        SendMessageA(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_MODLIST && HIWORD(wParam) == LBN_SELCHANGE) {
            int sel = (int)SendMessageA(s_hListBox, LB_GETCURSEL, 0, 0);
            const char* desc = "";
            if (sel > 0 && sel <= s_modCount)
                desc = s_mods[sel-1].description;
            SetWindowTextA(s_hDesc, desc);
        }
        if (LOWORD(wParam) == IDC_LAUNCH ||
            (LOWORD(wParam) == IDC_MODLIST && HIWORD(wParam) == LBN_DBLCLK)) {
            DoLaunch(hwnd);
        }
        if (LOWORD(wParam) == IDC_CANCEL)
            PostQuitMessage(0);
        break;

    case WM_CLOSE:
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// ---- Entry point ----------------------------------------------------------

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int) {
    s_hInst = hInst;

    // Scan from the launcher's own directory so it works regardless of CWD.
    char launcherDir[MAX_PATH];
    GetModuleFileNameA(NULL, launcherDir, sizeof(launcherDir));
    char* ls = strrchr(launcherDir, '\\');
    if (!ls) ls = strrchr(launcherDir, '/');
    if (ls) *(ls+1) = '\0';
    else strcpy(launcherDir, ".\\");

    char modsPath[MAX_PATH];
    _snprintf(modsPath, sizeof(modsPath), "%smods\\", launcherDir);
    modsPath[sizeof(modsPath)-1] = '\0';

    ScanMods(modsPath);

    // If no mods found, skip UI and launch directly in base game mode.
    if (s_modCount == 0) {
        SetEnvironmentVariableA("MC2_ACTIVE_MOD", NULL);
        char mc2Path[MAX_PATH];
        _snprintf(mc2Path, sizeof(mc2Path), "%smc2.exe", launcherDir);
        SetCurrentDirectoryA(launcherDir);
        STARTUPINFOA si = {}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        CreateProcessA(mc2Path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        if (pi.hProcess) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        return 0;
    }

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "MC2LauncherWnd";
    RegisterClassA(&wc);

    int wW = 400, wH = 316;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowA("MC2LauncherWnd",
        "MechCommander 2 OpenGL — Launcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        (sx - wW) / 2, (sy - wH) / 2, wW, wH,
        NULL, NULL, hInst, NULL);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
