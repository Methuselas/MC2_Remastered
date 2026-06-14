// mc2_launcher.cpp — MC2 OpenGL Campaign Mod Launcher
//
// Scans ./mods/ for any folder that contains a data/ subdir and treats it as a
// mod — NO mod.json required. Folders are classified by content:
//   * compatibility layer  — folder name contains "-compat", or mod.json says
//                            type="dependency". Shown as a checkbox.
//   * campaign             — has data/missions with mission .fit files and is
//                            not a compat layer. Shown in the campaign list.
//
// For the selected campaign the launcher auto-detects which compatibility layer
// it needs and pre-checks it:
//   * MechCommander Omnitech (MCO): mission Parts reference ObjectNumber > 1188
//     (mech FitIDs only mco-compat provides) AND warrior brains call magicAttack.
//   * MC2X "fat" packs: ObjectNumber > 1188 but no magicAttack -> mc2x-compat.
//   * otherwise ambiguous (stock-range objtypes, indistinguishable from pure
//     stock) -> nothing auto-checked; the player ticks a layer if needed.
//
// On launch it sets MC2_ACTIVE_MOD=<campaign folder> and MC2_MOD_DEPS=<comma-
// separated checked compat folders> in mc2.exe's environment. If a campaign is
// launched with no compatibility layer checked it warns first ("are you sure?").

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define UNICODE 0
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDC_MODLIST   1001
#define IDC_LAUNCH    1002
#define IDC_CANCEL    1003
#define IDC_LABEL     1004
#define IDC_DESC      1005
#define IDC_COMPATLBL 1006
#define IDC_COMPAT0   1100  // compat checkboxes: IDC_COMPAT0 + index

#define MAX_MODS 64

enum CompatKind { CK_UNKNOWN = 0, CK_MCO = 1, CK_MC2X = 2 };

struct ModEntry {
    char folderName[256];   // OS directory name — passed as MC2_ACTIVE_MOD / MC2_MOD_DEPS
    char name[256];         // display name (mod.json "name" or folder)
    bool isCompat;          // compatibility layer (checkbox)
    bool isCampaign;        // has data/missions
    CompatKind needs;       // auto-detected compat for a campaign
};

static ModEntry s_campaigns[MAX_MODS];
static int      s_campCount = 0;
static ModEntry s_compats[MAX_MODS];
static int      s_compatCount = 0;

static HINSTANCE s_hInst;
static HWND      s_hListBox;
static HWND      s_hDesc;
static HWND      s_hCompat[MAX_MODS];

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

static bool DirExists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Case-insensitive substring search.
static bool ContainsCI(const char* hay, const char* needle) {
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; p++)
        if (_strnicmp(p, needle, nl) == 0) return true;
    return false;
}

// ---- Campaign compat auto-detection --------------------------------------
//
// Scan a directory's files: .fit -> max ObjectNumber; .abl -> magicAttack.
// Reads into a fixed buffer (mission/brain files are small text). Sets the two
// flags; short-circuits each once its signal is found.

static void ScanDirSignals(const char* dir, bool* hasHighObj, bool* hasMagic) {
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof(pattern), "%s\\*", dir);
    pattern[sizeof(pattern)-1] = '\0';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const char* ext = strrchr(fd.cFileName, '.');
        if (!ext) continue;
        bool isFit = _stricmp(ext, ".fit") == 0;
        bool isAbl = _stricmp(ext, ".abl") == 0 || _stricmp(ext, ".abi") == 0;
        if (!isFit && !isAbl) continue;
        if (isFit && *hasHighObj) continue;
        if (isAbl && *hasMagic) continue;

        char path[MAX_PATH];
        _snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        path[sizeof(path)-1] = '\0';
        HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh == INVALID_HANDLE_VALUE) continue;
        // Read up to 256 KB — enough for any mission .fit / brain.
        static char buf[262144];
        DWORD got = 0;
        ReadFile(fh, buf, sizeof(buf)-1, &got, NULL);
        CloseHandle(fh);
        buf[got] = '\0';

        if (isAbl) {
            if (ContainsCI(buf, "magicattack")) *hasMagic = true;
            continue;
        }
        // .fit: find every "ObjectNumber" and parse the int after '='.
        for (const char* p = buf; (p = strstr(p, "ObjectNumber")) != NULL; ) {
            p += 12;
            while (*p == ' ' || *p == '\t' || *p == '=') p++;
            int v = atoi(p);
            if (v > 1188) { *hasHighObj = true; break; }
        }
    } while (FindNextFileA(h, &fd) && !(*hasHighObj && *hasMagic));
    FindClose(h);
}

static CompatKind DetectCompat(const char* modFolderAbs) {
    char missions[MAX_PATH], warriors[MAX_PATH];
    _snprintf(missions, sizeof(missions), "%s\\data\\missions", modFolderAbs);
    _snprintf(warriors, sizeof(warriors), "%s\\data\\missions\\warriors", modFolderAbs);
    bool hasHighObj = false, hasMagic = false;
    ScanDirSignals(missions, &hasHighObj, &hasMagic);
    if (!(hasHighObj && hasMagic)) ScanDirSignals(warriors, &hasHighObj, &hasMagic);
    // magicAttack is an Omnitech-only brain routine -> definitive MCO signal, even
    // when the pack stays in stock objtype range (e.g. MC2-Exodus: objNum 878 but
    // magicAttack everywhere). A high ObjectNumber without it means an MC2X "fat"
    // pack (e.g. cveg, 1443). Otherwise stock-range -> can't tell from pure stock.
    if (hasMagic)   return CK_MCO;
    if (hasHighObj) return CK_MC2X;
    return CK_UNKNOWN;
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

        char modDir[MAX_PATH], dataDir[MAX_PATH], missDir[MAX_PATH], jsonPath[MAX_PATH];
        _snprintf(modDir,  sizeof(modDir),  "%s%s", modsPath, fd.cFileName);
        _snprintf(dataDir, sizeof(dataDir), "%s\\data", modDir);
        _snprintf(missDir, sizeof(missDir), "%s\\data\\missions", modDir);
        _snprintf(jsonPath,sizeof(jsonPath),"%s\\mod.json", modDir);

        // A mod is any folder with a data/ subdir — no mod.json required.
        if (!DirExists(dataDir)) continue;

        // Optional mod.json for nicer names / explicit type.
        char name[256] = "", type[32] = "";
        HANDLE fh = CreateFileA(jsonPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (fh != INVALID_HANDLE_VALUE) {
            char buf[4096] = {}; DWORD got = 0;
            ReadFile(fh, buf, sizeof(buf)-1, &got, NULL); CloseHandle(fh); buf[got] = '\0';
            JsonGetString(buf, "name", name, sizeof(name));
            JsonGetString(buf, "type", type, sizeof(type));
        }
        if (!name[0]) _snprintf(name, sizeof(name), "%s", fd.cFileName);

        bool isCompat = (_stricmp(type, "dependency") == 0) || ContainsCI(fd.cFileName, "-compat");

        if (isCompat) {
            if (s_compatCount >= MAX_MODS) continue;
            ModEntry& e = s_compats[s_compatCount++];
            _snprintf(e.folderName, sizeof(e.folderName), "%s", fd.cFileName);
            _snprintf(e.name, sizeof(e.name), "%s", name);
            e.isCompat = true; e.isCampaign = false; e.needs = CK_UNKNOWN;
        } else if (DirExists(missDir)) {
            if (s_campCount >= MAX_MODS) continue;
            ModEntry& e = s_campaigns[s_campCount++];
            _snprintf(e.folderName, sizeof(e.folderName), "%s", fd.cFileName);
            _snprintf(e.name, sizeof(e.name), "%s", name);
            e.isCompat = false; e.isCampaign = true;
            e.needs = DetectCompat(modDir);
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
}

// Find the compat-layer index whose folder matches a detected kind, or -1.
static int CompatIndexForKind(CompatKind k) {
    const char* tag = (k == CK_MCO) ? "mco" : (k == CK_MC2X) ? "mc2x" : NULL;
    if (!tag) return -1;
    for (int i = 0; i < s_compatCount; i++)
        if (ContainsCI(s_compats[i].folderName, tag)) return i;
    return -1;
}

// ---- UI sync --------------------------------------------------------------

static void UpdateForSelection() {
    int sel = (int)SendMessageA(s_hListBox, LB_GETCURSEL, 0, 0);
    // sel 0 = Base Game; sel-1 indexes s_campaigns.
    CompatKind needs = CK_UNKNOWN;
    const char* status = "Base game - no mod, no compatibility layer.";
    int wantIdx = -1;
    if (sel > 0 && sel <= s_campCount) {
        needs = s_campaigns[sel-1].needs;
        wantIdx = CompatIndexForKind(needs);
        if (needs == CK_MCO)
            status = "Detected: MechCommander Omnitech campaign. mco-compat auto-selected.";
        else if (needs == CK_MC2X)
            status = "Detected: MC2X campaign. mc2x-compat auto-selected.";
        else
            status = "Could not auto-detect. If this is an MC2X pack, tick a layer; pure-stock needs none.";
    }
    // Set checkboxes: tick only the auto-detected one (player may override).
    for (int i = 0; i < s_compatCount; i++)
        SendMessageA(s_hCompat[i], BM_SETCHECK,
                     (i == wantIdx) ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextA(s_hDesc, status);
}

// ---- Launch mc2.exe -------------------------------------------------------

static void DoLaunch(HWND hwnd) {
    int sel = (int)SendMessageA(s_hListBox, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) sel = 0;

    // Build MC2_MOD_DEPS from checked compat layers.
    char deps[1024] = "";
    int checkedCount = 0;
    for (int i = 0; i < s_compatCount; i++) {
        if (SendMessageA(s_hCompat[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            if (deps[0]) strncat(deps, ",", sizeof(deps)-strlen(deps)-1);
            strncat(deps, s_compats[i].folderName, sizeof(deps)-strlen(deps)-1);
            checkedCount++;
        }
    }

    if (sel == 0) {
        SetEnvironmentVariableA("MC2_ACTIVE_MOD", NULL);
        SetEnvironmentVariableA("MC2_MOD_DEPS", NULL);
    } else if (sel <= s_campCount) {
        const ModEntry& camp = s_campaigns[sel-1];
        // Warn if a campaign that needs (or might need) a compat layer has none.
        if (checkedCount == 0) {
            char msg[512];
            _snprintf(msg, sizeof(msg),
                "Campaign \"%s\" selected with NO compatibility layer.\n\n%s\n\nLaunch anyway?",
                camp.name,
                camp.needs == CK_MCO  ? "It looks like a MechCommander Omnitech pack and will almost certainly fail to load without mco-compat." :
                camp.needs == CK_MC2X ? "It looks like an MC2X pack and will likely fail without mc2x-compat." :
                                        "If it is a content pack it may fail to load; if it is pure stock this is fine.");
            if (MessageBoxA(hwnd, msg, "No compatibility layer",
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                return;
        }
        SetEnvironmentVariableA("MC2_ACTIVE_MOD", camp.folderName);
        SetEnvironmentVariableA("MC2_MOD_DEPS", deps[0] ? deps : NULL);
    }

    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, sizeof(exePath));
    char* lastSlash = strrchr(exePath, '\\');
    if (!lastSlash) lastSlash = strrchr(exePath, '/');
    if (lastSlash) *(lastSlash+1) = '\0'; else exePath[0] = '\0';

    char mc2Path[MAX_PATH];
    _snprintf(mc2Path, sizeof(mc2Path), "%smc2.exe", exePath);
    mc2Path[sizeof(mc2Path)-1] = '\0';
    if (exePath[0]) SetCurrentDirectoryA(exePath);

    STARTUPINFOA si = {}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(mc2Path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
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

        HWND hLabel = CreateWindowA("STATIC", "Select a campaign to launch, or choose Base Game:",
            WS_CHILD | WS_VISIBLE, 10, 10, 380, 18, hwnd, (HMENU)IDC_LABEL, s_hInst, NULL);
        SendMessageA(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        s_hListBox = CreateWindowA("LISTBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            10, 32, 380, 140, hwnd, (HMENU)IDC_MODLIST, s_hInst, NULL);
        SendMessageA(s_hListBox, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)"Base Game (no mod)");
        for (int i = 0; i < s_campCount; i++)
            SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)s_campaigns[i].name);
        SendMessageA(s_hListBox, LB_SETCURSEL, 0, 0);

        HWND hCompatLbl = CreateWindowA("STATIC", "Compatibility layer(s):",
            WS_CHILD | WS_VISIBLE, 10, 182, 380, 18, hwnd, (HMENU)IDC_COMPATLBL, s_hInst, NULL);
        SendMessageA(hCompatLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        int y = 204;
        for (int i = 0; i < s_compatCount; i++) {
            char label[300];
            _snprintf(label, sizeof(label), "%s  (%s)", s_compats[i].name, s_compats[i].folderName);
            s_hCompat[i] = CreateWindowA("BUTTON", label,
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                14, y, 376, 20, hwnd, (HMENU)(INT_PTR)(IDC_COMPAT0 + i), s_hInst, NULL);
            SendMessageA(s_hCompat[i], WM_SETFONT, (WPARAM)hFont, TRUE);
            y += 22;
        }

        s_hDesc = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT,
            10, y + 4, 380, 34, hwnd, (HMENU)IDC_DESC, s_hInst, NULL);
        SendMessageA(s_hDesc, WM_SETFONT, (WPARAM)hFont, TRUE);
        int by = y + 44;

        HWND hLaunch = CreateWindowA("BUTTON", "Launch",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 10, by, 185, 28,
            hwnd, (HMENU)IDC_LAUNCH, s_hInst, NULL);
        SendMessageA(hLaunch, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hCancel = CreateWindowA("BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE, 205, by, 185, 28,
            hwnd, (HMENU)IDC_CANCEL, s_hInst, NULL);
        SendMessageA(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        UpdateForSelection();
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_MODLIST && HIWORD(wParam) == LBN_SELCHANGE)
            UpdateForSelection();
        if (LOWORD(wParam) == IDC_LAUNCH ||
            (LOWORD(wParam) == IDC_MODLIST && HIWORD(wParam) == LBN_DBLCLK))
            DoLaunch(hwnd);
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

// Headless diagnostic: print discovered campaigns + auto-detected compat and the
// compat layers, then exit. Invoked with "--list" (no GUI). For verification/CI.
static void PrintListing() {
    FILE* f = fopen("launcher_list.txt", "w");
    if (!f) return;
    fprintf(f, "campaigns (%d):\n", s_campCount);
    for (int i = 0; i < s_campCount; i++) {
        const char* k = s_campaigns[i].needs == CK_MCO ? "MCO -> mco-compat"
                      : s_campaigns[i].needs == CK_MC2X ? "MC2X -> mc2x-compat"
                      : "unknown (none auto)";
        fprintf(f, "  %-24s  %s\n", s_campaigns[i].folderName, k);
    }
    fprintf(f, "compat layers (%d):\n", s_compatCount);
    for (int i = 0; i < s_compatCount; i++)
        fprintf(f, "  %s\n", s_compats[i].folderName);
    fclose(f);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmd, int) {
    s_hInst = hInst;

    char launcherDir[MAX_PATH];
    GetModuleFileNameA(NULL, launcherDir, sizeof(launcherDir));
    char* ls = strrchr(launcherDir, '\\');
    if (!ls) ls = strrchr(launcherDir, '/');
    if (ls) *(ls+1) = '\0'; else strcpy(launcherDir, ".\\");

    char modsPath[MAX_PATH];
    _snprintf(modsPath, sizeof(modsPath), "%smods\\", launcherDir);
    modsPath[sizeof(modsPath)-1] = '\0';

    ScanMods(modsPath);

    if (lpCmd && strstr(lpCmd, "--list")) { PrintListing(); return 0; }

    // No campaigns discovered: launch base game directly.
    if (s_campCount == 0) {
        SetEnvironmentVariableA("MC2_ACTIVE_MOD", NULL);
        SetEnvironmentVariableA("MC2_MOD_DEPS", NULL);
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

    // Size the window to fit the actual layout (must match WM_CREATE geometry):
    //   checkboxes start y=204, +22 each; desc; Launch/Cancel at y_after+44, h28.
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    int clientW = 400;                                  // 10 + 380 + 10
    int clientH = 204 + s_compatCount * 22 + 84;        // checkboxes + desc + buttons + margin
    RECT rc = { 0, 0, clientW, clientH };
    AdjustWindowRect(&rc, style, FALSE);
    int wW = rc.right - rc.left;
    int wH = rc.bottom - rc.top;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    HWND hwnd = CreateWindowA("MC2LauncherWnd",
        "MechCommander 2 OpenGL - Launcher",
        style, (sx - wW) / 2, (sy - wH) / 2, wW, wH,
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
