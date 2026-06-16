// mc2_launcher.cpp — MC2 OpenGL Campaign Mod Launcher
//
// Scans ./mods/ for any folder that contains a data/ subdir and treats it as a
// mod — NO mod.json required. Folders sort into THREE buckets:
//   * Compatibility Base — mco-compat / mc2x-compat. Folder name contains
//                         "-compat", or mod.json says type="dependency". These
//                         BOTH replace object2.pak/buildings.csv, so they are
//                         MUTUALLY EXCLUSIVE -> shown as a RADIO group (pick one,
//                         plus a "None" entry). Default = None.
//   * Add-ons            — genuine stackable asset packs (a weapon pack etc.):
//                         any non-campaign folder with data/ that is NOT a base
//                         (mod.json type="assets", or no playable missions and
//                         not "-compat"). Shown as CHECKBOXES (stack any).
//   * Campaign          — has data/missions with real mission content (.fit) and
//                         is not a base/add-on, OR mod.json says type="campaign".
//                         Shown in the campaign list (pick one).
//
// Bucketing rule (load-bearing): the 3 ABL library files corebrain.abx /
// orders.abx / miscfunc.abx are AI plumbing a compat layer ships — they are NOT
// mission content and must never make a folder look like a campaign. (Campaign
// detection keys on .fit, which already excludes those .abx libs; the exclusion
// is made explicit below.) An explicit mod.json "type" always wins over the
// content guess.
//
// For the selected campaign the launcher auto-detects which compatibility base
// it needs and auto-SELECTS that base radio (CK_MCO -> mco-compat radio,
// CK_MC2X -> mc2x-compat radio; Stock/undetected -> the "None" radio):
//   * MechCommander Omnitech (MCO): mission Parts reference ObjectNumber > 1188
//     (mech FitIDs only mco-compat provides) AND warrior brains call magicAttack.
//   * MC2X "fat" packs: ObjectNumber > 1188 but no magicAttack -> mc2x-compat.
//   * otherwise ambiguous (stock-range objtypes, indistinguishable from pure
//     stock) -> base radio stays None; the player picks a base if needed.
//
// On launch it sets MC2_ACTIVE_MOD=<campaign folder> (EMPTY for Stock) and
// MC2_MOD_DEPS = [checked stackable add-ons] then [selected base, if not None],
// comma-separated, in mc2.exe's environment. Add-ons are listed BEFORE the base
// because file.cpp treats MC2_MOD_DEPS as highest-priority-first, so add-ons
// override the base. Stock + None + no add-ons = pure base game (empty deps).
// The "no base picked - are you sure?" warning fires ONLY for a campaign that
// auto-needs a compat base (MCO/MC2X) but whose base radio is still None;
// Stock never warns.
//
// "Import..." button: picks a raw install/pack folder and shells out to the
// python importer brain (tools/mc2_import/import_tool.py) — the launcher is a
// THIN shell. It probes the folder (read-only classify), confirms with the user,
// then runs the import on a worker thread with a modal progress dialog. The
// importer owns all detection / idempotency / staging / atomicity. After import
// the mod list, base radios and add-on checkboxes are refreshed in place (fixed
// slots — never destroyed/recreated).

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define UNICODE 0
#include <windows.h>
#include <shlobj.h>
#include <commctrl.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IDC_MODLIST   1001
#define IDC_LAUNCH    1002
#define IDC_CANCEL    1003
#define IDC_LABEL     1004
#define IDC_DESC      1005
#define IDC_BASELBL   1006  // "Compatibility Base (pick one):" label
#define IDC_IMPORT    1007
#define IDC_ADDONLBL  1008  // "Add-ons (stack any):" label
#define IDC_BASE0     1050  // base radios: IDC_BASE0 + slot index (slot 0 = "None")
#define IDC_ADDON0    1100  // add-on checkboxes: IDC_ADDON0 + slot index
#define IDC_FASTERWEAPONS 1300  // gameplay toggle (NOT a mod): faster weapons

// Progress-dialog controls.
#define IDC_PROG_CANCEL 1200
#define IDC_PROG_BAR    1201
#define IDC_PROG_TEXT   1202

#define WM_APP_IMPORT_DONE (WM_APP + 1)

#define MAX_MODS    64
// FIXED set of base-radio slots: slot 0 is always "None"; the rest hold detected
// base layers (mco-compat, mc2x-compat). 1 None + up to 3 bases.
#define BASE_SLOTS  4
// FIXED set of stackable add-on checkbox slots (created once).
#define ADDON_SLOTS 4

enum CompatKind { CK_UNKNOWN = 0, CK_MCO = 1, CK_MC2X = 2 };

struct ModEntry {
    char folderName[256];   // OS directory name — passed as MC2_ACTIVE_MOD / MC2_MOD_DEPS
    char name[256];         // display name (mod.json "name" or folder)
    bool isCompat;          // compatibility layer (base or add-on)
    bool isCampaign;        // has data/missions
    CompatKind needs;       // auto-detected compat for a campaign
};

static ModEntry s_campaigns[MAX_MODS];
static int      s_campCount = 0;
// Compatibility BASES (mutually exclusive -> radio). Both mco-compat and
// mc2x-compat replace object2.pak/buildings.csv so only one can be active.
static ModEntry s_bases[MAX_MODS];
static int      s_baseCount = 0;
// Stackable ADD-ONS (genuine asset packs -> checkboxes; any number may stack).
static ModEntry s_addons[MAX_MODS];
static int      s_addonCount = 0;

static HINSTANCE s_hInst;
static HWND      s_hMainWnd;
static HWND      s_hListBox;
static HWND      s_hDesc;
static HWND      s_hImport;
static HWND      s_hBase[BASE_SLOTS];     // FIXED radio slots — slot 0 = "None"; never destroy
static HWND      s_hAddon[ADDON_SLOTS];   // FIXED add-on checkbox slots — never destroy
static HWND      s_hFasterWeapons;        // gameplay toggle (NOT a mod): faster weapons

static char      s_launcherDir[MAX_PATH];   // trailing-slash launcher directory
static char      s_modsPath[MAX_PATH];      // <launcherDir>mods\

// ---- Import state ---------------------------------------------------------
static volatile bool s_importing = false;
static HWND          s_hProgWnd  = NULL;
static HANDLE        s_importThread = NULL;
static char          s_importFolder[MAX_PATH] = "";  // folder being imported
static char          s_importerPath[MAX_PATH] = "";  // resolved import_tool.py OR frozen .exe
static bool          s_importerFrozen = false;       // true => s_importerPath is a standalone .exe (no python)
static char          s_pyExe[64] = "";               // "py" or "python" (dev-mode fallback only)
static volatile bool s_importForce = false;          // re-run with --force
static volatile bool s_importCancelled = false;
static HANDLE        s_importChildProc = NULL;        // child handle for Cancel/TerminateProcess
static char          s_autoJson[65536] = "";          // captured auto stdout
static DWORD         s_autoExit = 0;

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

static bool FileExists(const char* path) {
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
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

// The three ABL library files a compat layer ships under data/missions. They are
// campaign-brain plumbing (magicAttack etc.), NOT playable missions. A folder
// that contains ONLY these (e.g. mco-compat) must stay an add-on, never a
// campaign — so the "has playable missions" test below skips them explicitly.
static bool IsAblLibName(const char* fileName) {
    return _stricmp(fileName, "corebrain.abx") == 0 ||
           _stricmp(fileName, "orders.abx")    == 0 ||
           _stricmp(fileName, "miscfunc.abx")  == 0;
}

// True if <missDir> holds at least one real mission file (.fit) that is not one
// of the 3 ABL libs. (.abx libs are excluded outright; .fit is the mission key.)
static bool HasPlayableMissions(const char* missDir) {
    char pattern[MAX_PATH];
    _snprintf(pattern, sizeof(pattern), "%s\\*", missDir);
    pattern[sizeof(pattern)-1] = '\0';
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (IsAblLibName(fd.cFileName)) continue;   // skip corebrain/orders/miscfunc
        const char* ext = strrchr(fd.cFileName, '.');
        if (!ext) continue;
        if (_stricmp(ext, ".fit") == 0) { found = true; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

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

        // --- Bucketing ---
        // 1. Explicit mod.json "type" wins over content-detection:
        //      "campaign"               -> Campaign bucket
        //      "dependency" / "assets"  -> Add-on bucket
        // 2. Name "-compat" -> Add-on bucket.
        // 3. Otherwise content-detect: a folder with real playable missions
        //    (.fit, excluding the 3 ABL libs) -> Campaign; everything else with
        //    a data/ dir (a pure asset pack) -> Add-on (never vanish).
        bool typeCampaign = (_stricmp(type, "campaign") == 0);
        bool typeAddon    = (_stricmp(type, "dependency") == 0) ||
                            (_stricmp(type, "assets") == 0);

        bool isCampaign;
        if (typeCampaign)                          isCampaign = true;       // explicit override
        else if (typeAddon)                        isCampaign = false;      // explicit override
        else if (ContainsCI(fd.cFileName, "-compat")) isCampaign = false;   // name convention
        else                                       isCampaign = HasPlayableMissions(missDir);

        if (!isCampaign) {
            // Non-campaign content. Split into BASE vs stackable ADD-ON:
            //   BASE  = mod.json type=="dependency", OR folder name has "-compat".
            //           Bases (mco-compat/mc2x-compat) replace object2.pak/
            //           buildings.csv -> mutually exclusive -> radio group.
            //   ADD-ON= everything else (mod.json type=="assets", or any other
            //           non-campaign folder with data/) -> stackable -> checkbox.
            bool typeDependency = (_stricmp(type, "dependency") == 0);
            bool isBase = typeDependency || ContainsCI(fd.cFileName, "-compat");
            if (isBase) {
                if (s_baseCount >= MAX_MODS) continue;
                ModEntry& e = s_bases[s_baseCount++];
                _snprintf(e.folderName, sizeof(e.folderName), "%s", fd.cFileName);
                _snprintf(e.name, sizeof(e.name), "%s", name);
                e.isCompat = true; e.isCampaign = false; e.needs = CK_UNKNOWN;
            } else {
                if (s_addonCount >= MAX_MODS) continue;
                ModEntry& e = s_addons[s_addonCount++];
                _snprintf(e.folderName, sizeof(e.folderName), "%s", fd.cFileName);
                _snprintf(e.name, sizeof(e.name), "%s", name);
                e.isCompat = true; e.isCampaign = false; e.needs = CK_UNKNOWN;
            }
        } else {
            // Campaign bucket (radio / list).
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

// Find the BASE index whose folder matches a detected kind, or -1. (Radio slot
// for base i is i+1, since base-radio slot 0 is the "None" entry.)
static int BaseIndexForKind(CompatKind k) {
    const char* tag = (k == CK_MCO) ? "mco" : (k == CK_MC2X) ? "mc2x" : NULL;
    if (!tag) return -1;
    for (int i = 0; i < s_baseCount; i++)
        if (ContainsCI(s_bases[i].folderName, tag)) return i;
    return -1;
}

// ---- UI sync --------------------------------------------------------------

// ---- Two-column layout geometry (single source of truth) ------------------
// Both WM_CREATE control creation and the refresh helpers use these so a rescan
// keeps the two-column layout identical to the initial build.
//
// Right column (top -> bottom): base label, BASE_SLOTS radios, gap, add-on
// label, ADDON_SLOTS checkboxes. The right column is taller than the listbox,
// so the bottom row (status + buttons) clears whichever column is taller.
#define LP_MARGIN     10
#define LP_LIST_X     LP_MARGIN
#define LP_LIST_Y     40
#define LP_LIST_W     320
#define LP_LIST_H     200
#define LP_RCOL_X     (LP_LIST_X + LP_LIST_W + 15)   // right column left edge
#define LP_RCOL_W     230                             // right column width
#define LP_RLBL_Y     14                              // base label Y (matches left label)
#define LP_ROW_H      20                              // radio/checkbox height
#define LP_ROW_STEP   22                              // radio/checkbox vertical step
#define LP_LBL_H      18                              // sub-group label height
// Base radio group: label at LP_LIST_Y row top, radios stacked below it.
#define LP_BASE_LBL_Y LP_LIST_Y                                    // base group label top
#define LP_BASE_Y     (LP_BASE_LBL_Y + LP_LBL_H + 2)              // first base radio top
// Add-on group: a gap below the base radios, then label, then checkboxes.
#define LP_ADDON_LBL_Y (LP_BASE_Y + BASE_SLOTS * LP_ROW_STEP + 10) // add-on group label top
#define LP_ADDON_Y    (LP_ADDON_LBL_Y + LP_LBL_H + 2)            // first add-on checkbox top
#define LP_ADDON_BOTTOM (LP_ADDON_Y + ADDON_SLOTS * LP_ROW_STEP)  // bottom of add-on slots
// Options group: a gap below the add-on checkboxes, then an "Options:" label,
// then the "Faster weapons" gameplay toggle. This is NOT a mod -- it sets engine
// env vars on launch, separate from the MC2_ACTIVE_MOD/MC2_MOD_DEPS logic.
#define LP_OPT_LBL_Y  (LP_ADDON_BOTTOM + 14)                      // "Options:" label top
#define LP_OPT_Y      (LP_OPT_LBL_Y + LP_LBL_H + 2)               // faster-weapons checkbox top
#define LP_RCOL_BOTTOM (LP_OPT_Y + LP_ROW_STEP)                   // bottom of right column
#define LP_LCOL_BOTTOM (LP_LIST_Y + LP_LIST_H)                    // bottom of left column
// Bottom (status + buttons) clears the taller of the two columns.
#define LP_COLS_BOTTOM ((LP_RCOL_BOTTOM > LP_LCOL_BOTTOM) ? LP_RCOL_BOTTOM : LP_LCOL_BOTTOM)
#define LP_CLIENT_W   (LP_RCOL_X + LP_RCOL_W + LP_MARGIN)  // 10+320+15+230+10 = 585
#define LP_BOTTOM_Y   (LP_COLS_BOTTOM + 8)            // status STATIC top (below both columns)
#define LP_DESC_H     34
#define LP_BTN_Y      (LP_BOTTOM_Y + LP_DESC_H + 6)   // Launch/Cancel row top
#define LP_BTN_H      28

// Show/hide + relabel the FIXED base-radio slots from the current s_bases[] set.
// Slot 0 is always the "None" radio (always shown). Slot i+1 maps to base i.
// Never destroys/recreates a window — geometry is constant. Default selection is
// "None" (slot 0) whenever the set changes.
static void RefreshBaseSlots() {
    for (int i = 0; i < BASE_SLOTS; i++) {
        if (!s_hBase[i]) continue;
        if (i == 0) {
            SetWindowTextA(s_hBase[i], "None");
            ShowWindow(s_hBase[i], SW_SHOW);
        } else if ((i - 1) < s_baseCount) {
            char label[300];
            _snprintf(label, sizeof(label), "%s  (%s)",
                      s_bases[i-1].name, s_bases[i-1].folderName);
            label[sizeof(label)-1] = '\0';
            SetWindowTextA(s_hBase[i], label);
            ShowWindow(s_hBase[i], SW_SHOW);
        } else {
            SetWindowTextA(s_hBase[i], "");
            ShowWindow(s_hBase[i], SW_HIDE);
        }
    }
    // Default the exclusive group to "None".
    for (int i = 0; i < BASE_SLOTS; i++)
        if (s_hBase[i])
            SendMessageA(s_hBase[i], BM_SETCHECK,
                         (i == 0) ? BST_CHECKED : BST_UNCHECKED, 0);
}

// Show/hide + relabel the FIXED add-on checkbox slots from s_addons[]. Never
// destroys/recreates a window — geometry is constant. With no add-ons deployed,
// all slots hide and only the "Add-ons (stack any):" label remains visible.
static void RefreshAddonSlots() {
    for (int i = 0; i < ADDON_SLOTS; i++) {
        if (!s_hAddon[i]) continue;
        if (i < s_addonCount) {
            char label[300];
            _snprintf(label, sizeof(label), "%s  (%s)",
                      s_addons[i].name, s_addons[i].folderName);
            label[sizeof(label)-1] = '\0';
            SetWindowTextA(s_hAddon[i], label);
            ShowWindow(s_hAddon[i], SW_SHOW);
        } else {
            SetWindowTextA(s_hAddon[i], "");
            SendMessageA(s_hAddon[i], BM_SETCHECK, BST_UNCHECKED, 0);
            ShowWindow(s_hAddon[i], SW_HIDE);
        }
    }
}

// Select base-radio slot `slot` (0 = None) exclusively. Mirrors what
// BS_AUTORADIOBUTTON does on a user click, but lets code drive auto-detect.
static void SelectBaseRadio(int slot) {
    for (int i = 0; i < BASE_SLOTS; i++)
        if (s_hBase[i])
            SendMessageA(s_hBase[i], BM_SETCHECK,
                         (i == slot) ? BST_CHECKED : BST_UNCHECKED, 0);
}

// Return the currently selected base-radio slot (0 = None), or 0 if none.
static int SelectedBaseSlot() {
    for (int i = 0; i < BASE_SLOTS; i++)
        if (s_hBase[i] && SendMessageA(s_hBase[i], BM_GETCHECK, 0, 0) == BST_CHECKED)
            return i;
    return 0;
}

static void UpdateForSelection() {
    int sel = (int)SendMessageA(s_hListBox, LB_GETCURSEL, 0, 0);
    // sel 0 = Stock (base game); sel-1 indexes s_campaigns.
    const char* status = "Stock campaign. Pick a base only if needed; stack any add-ons.";
    if (sel > 0 && sel <= s_campCount) {
        CompatKind needs = s_campaigns[sel-1].needs;
        int baseIdx = BaseIndexForKind(needs);   // index into s_bases[], or -1
        if (needs == CK_MCO && baseIdx >= 0)
            status = "Detected: MechCommander Omnitech campaign. mco-compat base auto-selected.";
        else if (needs == CK_MC2X && baseIdx >= 0)
            status = "Detected: MC2X campaign. mc2x-compat base auto-selected.";
        else if (needs != CK_UNKNOWN)
            status = "Detected a compat need, but no matching base layer is installed.";
        else
            status = "Could not auto-detect. If this is an MC2X pack, pick a base; pure-stock needs none.";
        // Auto-SELECT the matching base radio (slot = baseIdx+1; slot 0 = None).
        // Stock or undetected -> None (slot 0). Add-on checkboxes left as-is so the
        // player's stacked picks survive a campaign change.
        SelectBaseRadio(baseIdx >= 0 ? baseIdx + 1 : 0);
    } else {
        // Stock: base radio -> None. Add-on checkboxes preserved.
        SelectBaseRadio(0);
    }
    SetWindowTextA(s_hDesc, status);
}

// Reset + re-scan mods, repopulate listbox + compat slots. Used after import.
static void RescanAndRepopulate(const char* selectFolder) {
    s_campCount = 0;
    s_baseCount = 0;
    s_addonCount = 0;
    ScanMods(s_modsPath);

    SendMessageA(s_hListBox, LB_RESETCONTENT, 0, 0);
    SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)"Stock (base game)");
    int wantSel = 0;
    for (int i = 0; i < s_campCount; i++) {
        SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)s_campaigns[i].name);
        if (selectFolder && selectFolder[0] &&
            _stricmp(s_campaigns[i].folderName, selectFolder) == 0)
            wantSel = i + 1;   // +1 for the Base Game row
    }
    SendMessageA(s_hListBox, LB_SETCURSEL, wantSel, 0);
    RefreshBaseSlots();
    RefreshAddonSlots();
    UpdateForSelection();
}

// ---- Launch mc2.exe -------------------------------------------------------

static void DoLaunch(HWND hwnd) {
    int sel = (int)SendMessageA(s_hListBox, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) sel = 0;

    // Build MC2_MOD_DEPS = [checked stackable add-ons] then [selected base].
    // file.cpp treats MC2_MOD_DEPS as highest-priority-FIRST, so add-ons are
    // listed BEFORE the base and thus override it. The base (mco/mc2x-compat),
    // if not "None", goes last.
    char deps[1024] = "";
    int addonChecked = 0;
    for (int i = 0; i < s_addonCount && i < ADDON_SLOTS; i++) {
        if (SendMessageA(s_hAddon[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            if (deps[0]) strncat(deps, ",", sizeof(deps)-strlen(deps)-1);
            strncat(deps, s_addons[i].folderName, sizeof(deps)-strlen(deps)-1);
            addonChecked++;
        }
    }
    int baseSlot = SelectedBaseSlot();   // 0 = None; slot i -> s_bases[i-1]
    if (baseSlot > 0 && (baseSlot - 1) < s_baseCount) {
        if (deps[0]) strncat(deps, ",", sizeof(deps)-strlen(deps)-1);
        strncat(deps, s_bases[baseSlot-1].folderName, sizeof(deps)-strlen(deps)-1);
    }

    if (sel == 0) {
        // Stock (base game). With add-ons checked this is the "stock campaign +
        // borrowed mechs/weapons" path: export an EMPTY active mod + the checked
        // deps. Slice 1's engine mounts deps with no active campaign (and guards
        // the dep ABL libs from overriding stock AI). No warning either way:
        //   - Stock + nothing  = pure base game.
        //   - Stock + add-ons  = a valid, intentional combo.
        SetEnvironmentVariableA("MC2_ACTIVE_MOD", "");          // empty, not unset
        SetEnvironmentVariableA("MC2_MOD_DEPS", deps[0] ? deps : NULL);
    } else if (sel <= s_campCount) {
        const ModEntry& camp = s_campaigns[sel-1];
        // Warn ONLY for a campaign that auto-NEEDS a compat base (MCO/MC2X
        // detected) but the base radio is still "None". An auto-undetected
        // campaign (pure-stock-range) is assumed fine; Stock is handled above.
        if (baseSlot == 0 && camp.needs != CK_UNKNOWN) {
            char msg[512];
            _snprintf(msg, sizeof(msg),
                "Campaign \"%s\" selected with NO compatibility base picked.\n\n%s\n\nLaunch anyway?",
                camp.name,
                camp.needs == CK_MCO  ? "It looks like a MechCommander Omnitech pack and will almost certainly fail to load without the mco-compat base." :
                                        "It looks like an MC2X pack and will likely fail without the mc2x-compat base.");
            if (MessageBoxA(hwnd, msg, "No base picked",
                            MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                return;
        }
        SetEnvironmentVariableA("MC2_ACTIVE_MOD", camp.folderName);
        SetEnvironmentVariableA("MC2_MOD_DEPS", deps[0] ? deps : NULL);
    }

    // Gameplay "Faster weapons" toggle (NOT a mod): drive engine env vars in the
    // child process. NULL unsets the var so unchecked == vanilla weapons.
    BOOL fw = (SendMessageA(s_hFasterWeapons, BM_GETCHECK, 0, 0) == BST_CHECKED);
    SetEnvironmentVariableA("MC2_DIRECT_FIRE_STRAIGHT", fw ? "1" : NULL);
    SetEnvironmentVariableA("MC2_PROJECTILE_SPEED_MULT", fw ? "2" : NULL);

    char mc2Path[MAX_PATH];
    _snprintf(mc2Path, sizeof(mc2Path), "%smc2.exe", s_launcherDir);
    mc2Path[sizeof(mc2Path)-1] = '\0';
    if (s_launcherDir[0]) SetCurrentDirectoryA(s_launcherDir);

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

// ---- Import: helpers ------------------------------------------------------

// Resolve the python launcher: prefer "py" (-3), then "python". Returns ""
// (empty) if neither is on PATH. Stores the chosen verb in s_pyExe.
static bool ResolvePython() {
    char found[MAX_PATH];
    if (SearchPathA(NULL, "py", ".exe", sizeof(found), found, NULL)) {
        strcpy(s_pyExe, "py");
        return true;
    }
    if (SearchPathA(NULL, "python", ".exe", sizeof(found), found, NULL)) {
        strcpy(s_pyExe, "python");
        return true;
    }
    return false;
}

// Resolve the importer. PREFER the standalone frozen exe (no python needed):
//   <launcherDir>importer\import_tool\import_tool.exe   (PyInstaller onedir)
// then %MC2_IMPORTER_DIR%\import_tool.exe (frozen, env override),
// then FALL BACK to the dev python tree:
//   <launcherDir>importer\mc2_import\import_tool.py
//   %MC2_IMPORTER_DIR%\import_tool.py
// Sets s_importerFrozen accordingly. Returns true and fills s_importerPath.
static bool ResolveImporter() {
    char cand[MAX_PATH];
    char envDir[MAX_PATH] = "";
    DWORD n = GetEnvironmentVariableA("MC2_IMPORTER_DIR", envDir, sizeof(envDir));
    bool haveEnv = (n > 0 && n < sizeof(envDir));
    char envSep = haveEnv && (envDir[n-1] == '\\' || envDir[n-1] == '/') ? '\0' : '\\';

    // (1) frozen exe under the deploy's importer\ (preferred — ships without python).
    _snprintf(cand, sizeof(cand), "%simporter\\import_tool\\import_tool.exe", s_launcherDir);
    cand[sizeof(cand)-1] = '\0';
    if (FileExists(cand)) { strcpy(s_importerPath, cand); s_importerFrozen = true; return true; }

    // (2) frozen exe via MC2_IMPORTER_DIR.
    if (haveEnv) {
        if (envSep) _snprintf(cand, sizeof(cand), "%s\\import_tool.exe", envDir);
        else        _snprintf(cand, sizeof(cand), "%simport_tool.exe", envDir);
        cand[sizeof(cand)-1] = '\0';
        if (FileExists(cand)) { strcpy(s_importerPath, cand); s_importerFrozen = true; return true; }
    }

    // (3) dev python tree under importer\mc2_import\.
    _snprintf(cand, sizeof(cand), "%simporter\\mc2_import\\import_tool.py", s_launcherDir);
    cand[sizeof(cand)-1] = '\0';
    if (FileExists(cand)) { strcpy(s_importerPath, cand); s_importerFrozen = false; return true; }

    // (4) dev .py via MC2_IMPORTER_DIR.
    if (haveEnv) {
        if (envSep) _snprintf(cand, sizeof(cand), "%s\\import_tool.py", envDir);
        else        _snprintf(cand, sizeof(cand), "%simport_tool.py", envDir);
        cand[sizeof(cand)-1] = '\0';
        if (FileExists(cand)) { strcpy(s_importerPath, cand); s_importerFrozen = false; return true; }
    }
    return false;
}

// Run a child process with stdout (and stderr) redirected to a pipe, blocking
// until exit. Captures up to outSz-1 bytes of stdout into outBuf. If childProcOut
// is non-NULL it receives the live process handle (for Cancel/TerminateProcess);
// the caller must NOT close it (this fn closes it on return). Returns false if the
// process could not be created; *exitCode set on success.
static bool RunCaptured(const char* cmdLine, char* outBuf, int outSz,
                        DWORD* exitCode, HANDLE* childProcOut) {
    if (outBuf && outSz) outBuf[0] = '\0';

    SECURITY_ATTRIBUTES sa = {}; sa.nLength = sizeof(sa); sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);   // read end not inherited

    STARTUPINFOA si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    // CreateProcessA may modify the command line buffer -> use a writable copy.
    char* cl = (char*)malloc(strlen(cmdLine) + 1);
    if (!cl) { CloseHandle(rd); CloseHandle(wr); return false; }
    strcpy(cl, cmdLine);

    BOOL ok = CreateProcessA(NULL, cl, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cl);
    CloseHandle(wr);   // parent's write end — child holds the other
    if (!ok) { CloseHandle(rd); return false; }

    if (childProcOut) *childProcOut = pi.hProcess;

    // Drain stdout while the child runs (prevents pipe-full deadlock).
    int used = 0;
    for (;;) {
        char chunk[4096];
        DWORD got = 0;
        if (!ReadFile(rd, chunk, sizeof(chunk), &got, NULL) || got == 0) break;
        if (outBuf && used < outSz - 1) {
            int copy = (int)got;
            if (copy > outSz - 1 - used) copy = outSz - 1 - used;
            memcpy(outBuf + used, chunk, copy);
            used += copy;
        }
    }
    if (outBuf) outBuf[used] = '\0';

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    if (exitCode) *exitCode = code;

    CloseHandle(rd);
    if (childProcOut) *childProcOut = NULL;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Build a quoted command line. FROZEN exe runs directly (no python):
//   "<importer.exe>" <sub> "<folder>" [extra] --json
// DEV python tree:
//   py -3 "<importer.py>" <sub> "<folder>" [extra] --json
static void BuildCmd(char* out, int outSz, const char* sub, const char* folder,
                     const char* extra) {
    // Strip a trailing slash off the folder: a quoted arg ending in '\' escapes the
    // closing quote (e.g. a drive-root pick "A:\") and mangles the command line.
    char fld[MAX_PATH];
    _snprintf(fld, sizeof(fld), "%s", folder ? folder : "");
    fld[sizeof(fld)-1] = '\0';
    for (size_t fl = strlen(fld); fl > 0 && (fld[fl-1] == '\\' || fld[fl-1] == '/'); )
        fld[--fl] = '\0';

    if (s_importerFrozen)
        _snprintf(out, outSz, "\"%s\" %s \"%s\" %s--json",
                  s_importerPath, sub, fld, extra ? extra : "");
    else if (_stricmp(s_pyExe, "py") == 0)
        _snprintf(out, outSz, "py -3 \"%s\" %s \"%s\" %s--json",
                  s_importerPath, sub, fld, extra ? extra : "");
    else
        _snprintf(out, outSz, "python \"%s\" %s \"%s\" %s--json",
                  s_importerPath, sub, fld, extra ? extra : "");
    out[outSz-1] = '\0';
}

// ---- Import: worker thread ------------------------------------------------
//
// Runs `auto` (optionally with --force), captures stdout/exit into globals, then
// PostMessage(WM_APP_IMPORT_DONE). All UI work happens back on the main thread.

static unsigned __stdcall ImportWorker(void* arg) {
    // s_launcherDir keeps a TRAILING SLASH. A quoted arg ending in a backslash --
    // --deploy "...rc1\" -- has the closing quote ESCAPED by that backslash under the
    // Windows CRT/CommandLineToArgv rules, so the deploy argument swallows the rest of
    // the command line (--force, --json) and the importer mis-parses. Strip trailing
    // slashes before quoting. (This broke EVERY GUI import; the CLI path passed an
    // unslashed --deploy, which is why it worked there.)
    char deployDir[MAX_PATH];
    _snprintf(deployDir, sizeof(deployDir), "%s", s_launcherDir);
    deployDir[sizeof(deployDir)-1] = '\0';
    for (size_t dl = strlen(deployDir); dl > 0 && (deployDir[dl-1] == '\\' || deployDir[dl-1] == '/'); )
        deployDir[--dl] = '\0';

    char deployArg[MAX_PATH + 16];
    _snprintf(deployArg, sizeof(deployArg), "--deploy \"%s\" ", deployDir);
    deployArg[sizeof(deployArg)-1] = '\0';

    char extra[MAX_PATH + 32];
    if (s_importForce)
        _snprintf(extra, sizeof(extra), "%s--force ", deployArg);
    else
        _snprintf(extra, sizeof(extra), "%s", deployArg);
    extra[sizeof(extra)-1] = '\0';

    char cmd[2 * MAX_PATH + 256];
    BuildCmd(cmd, sizeof(cmd), "auto", s_importFolder, extra);

    s_autoExit = 1;
    s_autoJson[0] = '\0';
    HANDLE dummy = NULL;
    // Publish the child handle for the Cancel button via s_importChildProc.
    // RunCaptured sets *childProcOut to the live handle then clears it on exit.
    RunCaptured(cmd, s_autoJson, sizeof(s_autoJson), &s_autoExit, &s_importChildProc);

    PostMessageA(s_hMainWnd, WM_APP_IMPORT_DONE, (WPARAM)s_autoExit, 0);
    return 0;
}

// ---- Import: progress dialog ----------------------------------------------

static LRESULT CALLBACK ProgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        HWND t = CreateWindowA("STATIC",
            "Importing... (this can take a few minutes)",
            WS_CHILD | WS_VISIBLE, 16, 14, 320, 18, hwnd, (HMENU)IDC_PROG_TEXT, s_hInst, NULL);
        SendMessageA(t, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND bar = CreateWindowExA(0, PROGRESS_CLASSA, NULL,
            WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 16, 40, 320, 18,
            hwnd, (HMENU)IDC_PROG_BAR, s_hInst, NULL);
        SendMessageA(bar, PBM_SETMARQUEE, TRUE, 30);
        HWND c = CreateWindowA("BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE, 138, 70, 80, 26, hwnd, (HMENU)IDC_PROG_CANCEL, s_hInst, NULL);
        SendMessageA(c, WM_SETFONT, (WPARAM)hFont, TRUE);
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_PROG_CANCEL) {
            s_importCancelled = true;
            HANDLE c = s_importChildProc;
            if (c) TerminateProcess(c, 1);   // staging makes a killed import safe
            EnableWindow(GetDlgItem(hwnd, IDC_PROG_CANCEL), FALSE);
            SetWindowTextA(GetDlgItem(hwnd, IDC_PROG_TEXT), "Cancelling...");
        }
        break;
    case WM_CLOSE:
        // Block manual close while running — Cancel is the path out.
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wParam, lParam);
    }
    return 0;
}

static void RegisterProgClass() {
    static bool done = false;
    if (done) return;
    WNDCLASSA wc = {};
    wc.lpfnWndProc   = ProgProc;
    wc.hInstance     = s_hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "MC2ProgressWnd";
    RegisterClassA(&wc);
    done = true;
}

static void ShowProgressDialog(HWND owner) {
    RegisterProgClass();
    int w = 360, h = 130;
    RECT rc = { 0, 0, w, h };
    DWORD style = WS_POPUP | WS_CAPTION | WS_VISIBLE;
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    RECT orc; GetWindowRect(owner, &orc);
    int ox = orc.left + ((orc.right - orc.left) - ww) / 2;
    int oy = orc.top  + ((orc.bottom - orc.top) - wh) / 2;
    s_hProgWnd = CreateWindowA("MC2ProgressWnd", "Importing",
        style, ox, oy, ww, wh, owner, NULL, s_hInst, NULL);
    UpdateWindow(s_hProgWnd);
}

// ---- Import: orchestration ------------------------------------------------

// SHBrowseForFolder pick. Returns true + fills out[] (MAX_PATH).
static bool PickFolder(HWND owner, char* out, int outSz) {
    BROWSEINFOA bi = {};
    bi.hwndOwner = owner;
    bi.lpszTitle = "Pick the install ROOT (where the .fst files are), not a data/ or subfolder.";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return false;
    char path[MAX_PATH] = "";
    bool ok = SHGetPathFromIDListA(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok || !path[0]) return false;
    _snprintf(out, outSz, "%s", path);
    out[outSz-1] = '\0';
    return true;
}

// Enable/disable the main-window interactive controls during import.
static void SetMainControlsEnabled(BOOL en) {
    EnableWindow(s_hListBox, en);
    EnableWindow(s_hImport, en);
    EnableWindow(GetDlgItem(s_hMainWnd, IDC_LAUNCH), en);
    EnableWindow(GetDlgItem(s_hMainWnd, IDC_CANCEL), en);
    for (int i = 0; i < BASE_SLOTS; i++)
        if (s_hBase[i]) EnableWindow(s_hBase[i], en);
    for (int i = 0; i < ADDON_SLOTS; i++)
        if (s_hAddon[i]) EnableWindow(s_hAddon[i], en);
}

// Start the worker thread + show the modal progress dialog. s_importForce must
// be set before calling. Assumes s_importing already true + button disabled.
static void StartImportRun(HWND hwnd) {
    SetMainControlsEnabled(FALSE);
    EnableWindow(hwnd, FALSE);              // disable main window (re-entrancy guard, M1)
    s_importCancelled = false;
    s_importChildProc = NULL;
    ShowProgressDialog(hwnd);

    unsigned tid = 0;
    s_importThread = (HANDLE)_beginthreadex(NULL, 0, ImportWorker, NULL, 0, &tid);
    if (!s_importThread) {
        // Could not spawn — recover synchronously.
        if (s_hProgWnd) { DestroyWindow(s_hProgWnd); s_hProgWnd = NULL; }
        EnableWindow(hwnd, TRUE);
        SetMainControlsEnabled(TRUE);
        s_importing = false;
        MessageBoxA(hwnd, "Could not start the import worker thread.", "Import Error",
                    MB_ICONERROR | MB_OK);
    }
}

static void OnImportButton(HWND hwnd) {
    // Re-entrancy guard (M1): disable SYNCHRONOUSLY before any async work.
    if (s_importing) return;

    if (!ResolveImporter()) {
        MessageBoxA(hwnd,
            "Importer not found.\n\nExpected at:\n"
            "  <launcher>\\importer\\import_tool\\import_tool.exe   (frozen)\n"
            "  <launcher>\\importer\\mc2_import\\import_tool.py      (dev)\n\n"
            "Or set the MC2_IMPORTER_DIR environment variable to its folder.",
            "Importer not found", MB_ICONERROR | MB_OK);
        return;
    }
    // Python is only needed for the dev (.py) fallback. The frozen exe is self-contained.
    if (!s_importerFrozen && !ResolvePython()) {
        MessageBoxA(hwnd, "Python required for import (dev build).\n\n"
            "Neither 'py' nor 'python' was found on PATH.",
            "Python required", MB_ICONERROR | MB_OK);
        return;
    }

    char folder[MAX_PATH];
    if (!PickFolder(hwnd, folder, sizeof(folder))) return;   // user cancelled

    // Probe (read-only classify).
    char probeCmd[2 * MAX_PATH + 64];
    BuildCmd(probeCmd, sizeof(probeCmd), "probe", folder, NULL);
    char json[16384]; DWORD ex = 1; HANDLE dummy = NULL;
    if (!RunCaptured(probeCmd, json, sizeof(json), &ex, &dummy)) {
        MessageBoxA(hwnd, "Failed to run the importer (probe).", "Import Error",
                    MB_ICONERROR | MB_OK);
        return;
    }

    char kind[64] = "", action[64] = "", reject[1024] = "";
    JsonGetString(json, "kind", kind, sizeof(kind));
    JsonGetString(json, "action", action, sizeof(action));
    JsonGetString(json, "reject_reason", reject, sizeof(reject));

    bool rejected = (_stricmp(kind, "UNKNOWN") == 0) || (_stricmp(kind, "STOCK") == 0) ||
                    (_stricmp(action, "reject") == 0) || !kind[0];
    if (rejected) {
        char msg[1400];
        _snprintf(msg, sizeof(msg),
            "Cannot import this folder.\n\n%s\n\nHint: pick the install ROOT (where the .fst files are), "
            "not a data/ or subfolder.",
            reject[0] ? reject : "The folder was not recognized as an importable install or pack.");
        msg[sizeof(msg)-1] = '\0';
        MessageBoxA(hwnd, msg, "Cannot import", MB_ICONWARNING | MB_OK);
        return;
    }

    // Confirm — human sentence per kind.
    char confirm[1024];
    if (_stricmp(kind, "MCO") == 0)
        strcpy(confirm, "Detected a MechCommander Omnitech install. Import it as a playable campaign "
                        "(this can take several minutes)?");
    else if (ContainsCI(kind, "MC2X"))
        strcpy(confirm, "Detected a MechCommander 2 X-pack install. Import it as a playable campaign "
                        "(this can take several minutes)?");
    else if (_stricmp(kind, "PACK") == 0)
        strcpy(confirm, "Detected a content pack. Import it as a playable campaign "
                        "(this can take a few minutes)?");
    else
        _snprintf(confirm, sizeof(confirm),
            "Detected: %s. Import it as a playable campaign (this can take several minutes)?", kind);
    if (MessageBoxA(hwnd, confirm, "Import", MB_ICONQUESTION | MB_YESNO) != IDYES)
        return;

    // Commit to importing — synchronous guard BEFORE the async worker.
    s_importing = true;
    EnableWindow(s_hImport, FALSE);
    _snprintf(s_importFolder, sizeof(s_importFolder), "%s", folder);
    s_importFolder[sizeof(s_importFolder)-1] = '\0';
    s_importForce = false;

    StartImportRun(hwnd);
}

// WM_APP_IMPORT_DONE handler: worker finished. Parse auto JSON, handle
// guard-refusal -> --force re-run, then refresh UI + re-enable.
static void OnImportDone(HWND hwnd) {
    // Close progress dialog + reap worker thread.
    if (s_hProgWnd) { DestroyWindow(s_hProgWnd); s_hProgWnd = NULL; }
    if (s_importThread) {
        WaitForSingleObject(s_importThread, INFINITE);
        CloseHandle(s_importThread);
        s_importThread = NULL;
    }

    char status[64] = "", message[2048] = "";
    JsonGetString(s_autoJson, "status", status, sizeof(status));
    JsonGetString(s_autoJson, "message", message, sizeof(message));

    bool cancelled = s_importCancelled;
    bool guardRefusal = (s_autoExit != 0) || (_stricmp(status, "error") == 0);

    if (cancelled) {
        // Re-enable; staging means the real mod is untouched.
        EnableWindow(hwnd, TRUE);
        SetMainControlsEnabled(TRUE);
        EnableWindow(s_hImport, TRUE);
        s_importing = false;
        MessageBoxA(hwnd, "Import cancelled. No changes were applied.", "Import cancelled",
                    MB_ICONINFORMATION | MB_OK);
        return;
    }

    if (guardRefusal && !s_importForce) {
        // Offer overwrite via --force.
        int r = MessageBoxA(hwnd,
            "This looks already-imported or modified.\n\nRe-import and overwrite?",
            "Re-import?", MB_ICONWARNING | MB_YESNO);
        if (r == IDYES) {
            s_importForce = true;
            StartImportRun(hwnd);   // re-run with --force; another WM_APP_IMPORT_DONE follows
            return;
        }
        // User declined — surface the message + bail.
        EnableWindow(hwnd, TRUE);
        SetMainControlsEnabled(TRUE);
        EnableWindow(s_hImport, TRUE);
        s_importing = false;
        char msg[2200];
        _snprintf(msg, sizeof(msg), "Import did not complete.\n\n%s",
                  message[0] ? message : "The importer refused the operation.");
        msg[sizeof(msg)-1] = '\0';
        MessageBoxA(hwnd, msg, "Import not applied", MB_ICONERROR | MB_OK);
        return;
    }

    if (guardRefusal) {
        // Already tried --force and still failed -> human error, no traceback.
        EnableWindow(hwnd, TRUE);
        SetMainControlsEnabled(TRUE);
        EnableWindow(s_hImport, TRUE);
        s_importing = false;
        char msg[2200];
        _snprintf(msg, sizeof(msg), "Import failed.\n\n%s",
                  message[0] ? message : "The importer reported an error.");
        msg[sizeof(msg)-1] = '\0';
        MessageBoxA(hwnd, msg, "Import failed", MB_ICONERROR | MB_OK);
        return;
    }

    // Success (ok / skipped / warn). Identify the newly imported campaign so we
    // can re-select it: first entry of mods_written if present.
    char written[256] = "";
    JsonGetString(s_autoJson, "mods_written", written, sizeof(written));
    // mods_written is a JSON array; JsonGetString won't grab it (no leading '"').
    // Fall back to parsing the first string element of the array.
    char firstMod[256] = "";
    {
        const char* p = strstr(s_autoJson, "\"mods_written\"");
        if (p) {
            const char* q = strchr(p, '[');
            if (q) {
                const char* s = strchr(q, '"');
                const char* lb = strchr(q, ']');
                if (s && (!lb || s < lb)) {
                    const char* e = strchr(s + 1, '"');
                    if (e) {
                        int len = (int)(e - (s + 1));
                        if (len > 0 && len < (int)sizeof(firstMod)) {
                            memcpy(firstMod, s + 1, len);
                            firstMod[len] = '\0';
                        }
                    }
                }
            }
        }
    }

    // Re-enable + repopulate in place (fixed slots — no destroy/recreate).
    EnableWindow(hwnd, TRUE);
    SetMainControlsEnabled(TRUE);
    EnableWindow(s_hImport, TRUE);
    s_importing = false;

    RescanAndRepopulate(firstMod[0] ? firstMod : NULL);

    char msg[2300];
    if (_stricmp(status, "warn") == 0) {
        char warnings[1024] = "";
        // Surface warnings array's first string if present.
        const char* p = strstr(s_autoJson, "\"warnings\"");
        if (p) {
            const char* q = strchr(p, '[');
            if (q) { const char* s = strchr(q, '"'); if (s) {
                const char* e = strchr(s + 1, '"');
                if (e) { int len = (int)(e - (s+1));
                    if (len > 0 && len < (int)sizeof(warnings)) { memcpy(warnings, s+1, len); warnings[len]='\0'; } } } }
        }
        _snprintf(msg, sizeof(msg), "Import completed with warnings.\n\n%s%s%s",
                  message[0] ? message : "Imported.",
                  warnings[0] ? "\n\nWarning: " : "", warnings);
    } else if (_stricmp(status, "skipped") == 0) {
        _snprintf(msg, sizeof(msg), "%s",
                  message[0] ? message : "Already imported and unchanged — nothing to do.");
    } else {
        _snprintf(msg, sizeof(msg), "%s", message[0] ? message : "Import complete.");
    }
    msg[sizeof(msg)-1] = '\0';
    MessageBoxA(hwnd, msg, "Import", MB_ICONINFORMATION | MB_OK);
}

// ---- Window procedure -----------------------------------------------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        s_hMainWnd = hwnd;
        HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

        // ---- LEFT COLUMN: campaign label + Import button + campaign listbox ----
        // Top row: label (top-left of left column) + Import button (top-right of
        // left column, above the listbox, as before relative to the campaign section).
        HWND hLabel = CreateWindowA("STATIC", "Campaign (pick one):",
            WS_CHILD | WS_VISIBLE, LP_LIST_X, 14, 220, 18, hwnd, (HMENU)IDC_LABEL, s_hInst, NULL);
        SendMessageA(hLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        s_hImport = CreateWindowA("BUTTON", "Import...",
            WS_CHILD | WS_VISIBLE, LP_LIST_X + LP_LIST_W - 90, 8, 90, 26,
            hwnd, (HMENU)IDC_IMPORT, s_hInst, NULL);
        SendMessageA(s_hImport, WM_SETFONT, (WPARAM)hFont, TRUE);

        s_hListBox = CreateWindowA("LISTBOX", NULL,
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            LP_LIST_X, LP_LIST_Y, LP_LIST_W, LP_LIST_H, hwnd, (HMENU)IDC_MODLIST, s_hInst, NULL);
        SendMessageA(s_hListBox, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)"Stock (base game)");
        for (int i = 0; i < s_campCount; i++)
            SendMessageA(s_hListBox, LB_ADDSTRING, 0, (LPARAM)s_campaigns[i].name);
        SendMessageA(s_hListBox, LB_SETCURSEL, 0, 0);

        // ---- RIGHT COLUMN: base-radio group (top) + add-on checkbox group ----
        // Sub-group 1: "Compatibility Base (pick one):" label + radios.
        HWND hBaseLbl = CreateWindowA("STATIC", "Compatibility Base (pick one):",
            WS_CHILD | WS_VISIBLE, LP_RCOL_X, LP_BASE_LBL_Y, LP_RCOL_W, LP_LBL_H,
            hwnd, (HMENU)IDC_BASELBL, s_hInst, NULL);
        SendMessageA(hBaseLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        // FIXED base-radio slots: ONE exclusive group (first radio carries
        // WS_GROUP; BS_AUTORADIOBUTTON makes them mutually exclusive). Slot 0 is
        // always "None". Geometry never changes — refresh = show/hide + relabel.
        for (int i = 0; i < BASE_SLOTS; i++) {
            int by = LP_BASE_Y + i * LP_ROW_STEP;
            DWORD st = WS_CHILD | BS_AUTORADIOBUTTON;
            if (i == 0) st |= WS_GROUP;   // start of the exclusive radio group
            s_hBase[i] = CreateWindowA("BUTTON", "", st,
                LP_RCOL_X, by, LP_RCOL_W, LP_ROW_H,
                hwnd, (HMENU)(INT_PTR)(IDC_BASE0 + i), s_hInst, NULL);
            SendMessageA(s_hBase[i], WM_SETFONT, (WPARAM)hFont, TRUE);
        }

        // Sub-group 2: "Add-ons (stack any):" label + stackable checkboxes.
        // A WS_GROUP STATIC after the radios terminates the radio group so the
        // add-on checkboxes are not pulled into it.
        HWND hAddonLbl = CreateWindowA("STATIC", "Add-ons (stack any):",
            WS_CHILD | WS_VISIBLE | WS_GROUP, LP_RCOL_X, LP_ADDON_LBL_Y, LP_RCOL_W, LP_LBL_H,
            hwnd, (HMENU)IDC_ADDONLBL, s_hInst, NULL);
        SendMessageA(hAddonLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        // FIXED add-on checkbox slots: stacked vertically below the add-on label.
        // Geometry never changes — refresh = show/hide + relabel only. With no
        // add-ons deployed, all hide and just the label shows (that's fine).
        for (int i = 0; i < ADDON_SLOTS; i++) {
            int cy = LP_ADDON_Y + i * LP_ROW_STEP;
            s_hAddon[i] = CreateWindowA("BUTTON", "",
                WS_CHILD | BS_AUTOCHECKBOX,
                LP_RCOL_X, cy, LP_RCOL_W, LP_ROW_H,
                hwnd, (HMENU)(INT_PTR)(IDC_ADDON0 + i), s_hInst, NULL);
            SendMessageA(s_hAddon[i], WM_SETFONT, (WPARAM)hFont, TRUE);
        }
        RefreshBaseSlots();    // show/hide + label per current scan; default None
        RefreshAddonSlots();   // show/hide + label per current scan

        // Sub-group 3: gameplay "Options:" label + Faster-weapons toggle. This is
        // NOT a mod -- the checkbox drives engine env vars on Launch, separate from
        // the MC2_ACTIVE_MOD/MC2_MOD_DEPS mod logic. A WS_GROUP STATIC terminates
        // any preceding group; the checkbox defaults UNCHECKED.
        HWND hOptLbl = CreateWindowA("STATIC", "Options:",
            WS_CHILD | WS_VISIBLE | WS_GROUP, LP_RCOL_X, LP_OPT_LBL_Y, LP_RCOL_W, LP_LBL_H,
            hwnd, (HMENU)(INT_PTR)-1, s_hInst, NULL);
        SendMessageA(hOptLbl, WM_SETFONT, (WPARAM)hFont, TRUE);

        s_hFasterWeapons = CreateWindowA("BUTTON", "Faster weapons (experimental)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            LP_RCOL_X, LP_OPT_Y, LP_RCOL_W, LP_ROW_H,
            hwnd, (HMENU)(INT_PTR)IDC_FASTERWEAPONS, s_hInst, NULL);
        SendMessageA(s_hFasterWeapons, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageA(s_hFasterWeapons, BM_SETCHECK, BST_UNCHECKED, 0);  // default OFF

        // ---- BOTTOM (spans full width below BOTH columns): status + buttons ----
        s_hDesc = CreateWindowA("STATIC", "", WS_CHILD | WS_VISIBLE | SS_LEFT,
            LP_MARGIN, LP_BOTTOM_Y, LP_CLIENT_W - 2 * LP_MARGIN, LP_DESC_H,
            hwnd, (HMENU)IDC_DESC, s_hInst, NULL);
        SendMessageA(s_hDesc, WM_SETFONT, (WPARAM)hFont, TRUE);

        int btnW = (LP_CLIENT_W - 2 * LP_MARGIN - 10) / 2;   // two buttons + 10px gap
        HWND hLaunch = CreateWindowA("BUTTON", "Launch",
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, LP_MARGIN, LP_BTN_Y, btnW, LP_BTN_H,
            hwnd, (HMENU)IDC_LAUNCH, s_hInst, NULL);
        SendMessageA(hLaunch, WM_SETFONT, (WPARAM)hFont, TRUE);
        HWND hCancel = CreateWindowA("BUTTON", "Cancel",
            WS_CHILD | WS_VISIBLE, LP_MARGIN + btnW + 10, LP_BTN_Y, btnW, LP_BTN_H,
            hwnd, (HMENU)IDC_CANCEL, s_hInst, NULL);
        SendMessageA(hCancel, WM_SETFONT, (WPARAM)hFont, TRUE);

        UpdateForSelection();
        break;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_MODLIST && HIWORD(wParam) == LBN_SELCHANGE)
            UpdateForSelection();
        if (LOWORD(wParam) == IDC_IMPORT && HIWORD(wParam) == BN_CLICKED)
            OnImportButton(hwnd);
        if (LOWORD(wParam) == IDC_LAUNCH ||
            (LOWORD(wParam) == IDC_MODLIST && HIWORD(wParam) == LBN_DBLCLK)) {
            if (!s_importing) DoLaunch(hwnd);
        }
        if (LOWORD(wParam) == IDC_CANCEL) {
            if (!s_importing) PostQuitMessage(0);
        }
        break;

    case WM_APP_IMPORT_DONE:
        OnImportDone(hwnd);
        break;

    case WM_CLOSE:
        if (s_importing) return 0;   // block close during import
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
    fprintf(f, "bases (%d) [radio, mutually exclusive]:\n", s_baseCount);
    for (int i = 0; i < s_baseCount; i++)
        fprintf(f, "  %s\n", s_bases[i].folderName);
    fprintf(f, "add-ons (%d) [checkbox, stackable]:\n", s_addonCount);
    for (int i = 0; i < s_addonCount; i++)
        fprintf(f, "  %s\n", s_addons[i].folderName);
    fclose(f);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmd, int) {
    s_hInst = hInst;

    GetModuleFileNameA(NULL, s_launcherDir, sizeof(s_launcherDir));
    char* ls = strrchr(s_launcherDir, '\\');
    if (!ls) ls = strrchr(s_launcherDir, '/');
    if (ls) *(ls+1) = '\0'; else strcpy(s_launcherDir, ".\\");

    _snprintf(s_modsPath, sizeof(s_modsPath), "%smods\\", s_launcherDir);
    s_modsPath[sizeof(s_modsPath)-1] = '\0';

    ScanMods(s_modsPath);

    if (lpCmd && strstr(lpCmd, "--list")) { PrintListing(); return 0; }

    // No campaigns discovered: launch base game directly.
    if (s_campCount == 0) {
        SetEnvironmentVariableA("MC2_ACTIVE_MOD", NULL);
        SetEnvironmentVariableA("MC2_MOD_DEPS", NULL);
        char mc2Path[MAX_PATH];
        _snprintf(mc2Path, sizeof(mc2Path), "%smc2.exe", s_launcherDir);
        SetCurrentDirectoryA(s_launcherDir);
        STARTUPINFOA si = {}; si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        CreateProcessA(mc2Path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        if (pi.hProcess) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        return 0;
    }

    // Init common controls (marquee progress bar) + COM (SHBrowseForFolder).
    INITCOMMONCONTROLSEX icc = {}; icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    CoInitialize(NULL);

    WNDCLASSA wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = "MC2LauncherWnd";
    RegisterClassA(&wc);

    // Size the window to fit the two-column layout (must match WM_CREATE geometry):
    //   LEFT col: listbox at (10,40) 320x200; RIGHT col: base label + BASE_SLOTS
    //   radios, then add-on label + ADDON_SLOTS checkboxes; BOTTOM (full width):
    //   status STATIC then Launch/Cancel. Client height clears the taller column.
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    int clientW = LP_CLIENT_W;                           // 10+320+15+230+10 = 585
    int clientH = LP_BTN_Y + LP_BTN_H + LP_MARGIN;       // bottom button row + margin
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
    CoUninitialize();
    return 0;
}
