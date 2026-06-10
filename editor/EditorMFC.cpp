/***************************************************************
* FILENAME: EditorMFC.cpp
* DESCRIPTION: Implements the MFC application entry points for the Editor.
* AUTHOR: Microsoft Corporation
* COPYRIGHT: Copyright (C) Microsoft Corporation. All rights reserved.
* DATE: 04/28/2026
* MODIFICATION: by Methuselas
* CHANGES: Gate MFC startup trace logging behind MC2_EDITOR_TRACE.
****************************************************************/

#include <cstdio>
#include <stdlib.h>
#include <string>
#include <cstring>
#include <vector>
#include "../GameOS/gameos/gos_crashbundle.h"

// ---------------------------------------------------------------------------
// S-CLI: command-line flags for autonomous editor launches.
//
//   -mission PATH.pak       auto-load .pak on first idle after InitGameOS
//   -frames N               PostMessage WM_CLOSE after N rendered frames
//   -exit-on-load-fail      exit nonzero on mission load failure (no modal)
//
// Globals are file-scope here; EditorCamera.h reads g_cliFramesLimit /
// g_cliFrameCounter via extern declarations to drive the WM_CLOSE post.
// ---------------------------------------------------------------------------
std::string g_cliMissionPath;
long        g_cliFramesLimit     = 0;     // 0 = no auto-close
bool        g_cliExitOnLoadFail  = false;
long        g_cliFrameCounter    = 0;     // bumped by EditorCamera::render
bool        g_cliClosePosted     = false; // one-shot guard for WM_CLOSE
bool        g_cliAutoLoadFired   = false; // one-shot guard for auto-load

// --new-map[=mapSize,terrain]: headless repro of the File->New (from-scratch
// generator) path. Mirrors -mission auto-load but calls initTerrainFromTGA
// instead of initTerrainFromPCV. mapSize/terrain are the MapSizeDlg/TerrainDlg
// indices (default 0,0). Diagnostic harness for debugging the generator; not
// surfaced in the UI.
bool        g_cliNewMap          = false;
bool        g_cliGenMap          = false;
int         g_cliNewMapSize      = 0;
int         g_cliNewMapTerrain   = 0;

// -smoke-foliage[=PATH]: after the map auto-loads, exercise the Phase 5 foliage
// preview (FoliageRender::Load + Toggle) under the live frame loop. A missing or
// malformed JSON / missing sprite texture must load to 0 instances WITHOUT
// crashing -- that is the whole point of the foliage smoke. Default path is the
// generator's genmap sidecar. Reported as facts in the [ESMOKE v1] summary; the
// Python runner (run_editor_smoke.py) applies the pass/fail verdict.
bool        g_cliSmokeFoliage    = false;
std::string g_cliSmokeFoliagePath;
bool        g_cliSmokeFoliageFired = false;
int         g_cliSmokeFoliageCount = -1;   // -1 = not attempted, >=0 = Load() ran

// -smoke-exit-sec N: time-based auto-exit. The -frames path (EditorCamera::render
// counter) is unreliable -- that render does not tick every frame and a WM_CLOSE
// can stall on a "save changes?" modal in a headless run. Instead, N seconds after
// the engine is up we emit the [ESMOKE v1] summary and hard exit(0): guaranteed
// termination + rc 0 + a parseable summary, which is all a smoke needs.
int          g_cliExitAfterSec    = 0;     // 0 = disabled
unsigned int g_cliExitDeadlineMs  = 0;     // GetTickCount() target, set on first post-init idle
bool         g_cliEsmokeEmitted   = false;

// Smoke mode suppresses the auto-run-path AfxMessageBox modals (generate/load
// failures) that would otherwise hang a headless run with no one to click OK.
// Read by EditorData.cpp via extern. Derived from the smoke flags at end of parse.
bool         g_cliSuppressModals  = false;

// -smoke-save[=PATH]: after the map auto-generates, save it programmatically to a
// .pak so a second run can load it (the generate->save->load gate, driven through
// the editor). Default path lives under terrain_gen_out so it deploys with tools.
bool         g_cliSmokeSave       = false;
std::string  g_cliSmokeSavePath;
int          g_cliSmokeSaveOk     = -1;    // -1 = not attempted, 0/1 = save() result

// -smoke-outliner: after the map auto-loads, exercise the read-only Scene
// Outliner Lite: open the panel, enumerate placed objects (counts), and select
// the first selectable object via the existing EditorObjectMgr path. Read-only
// (no save mutation); reported in the [ESMOKE v1] summary. Pairs with a -gen-map
// (empty map -> counts all zero, selected=0, which is still a valid PASS).
bool         g_cliSmokeOutliner       = false;
int          g_cliSmokeOutlinerCount  = -1;   // -1 = not attempted, >=0 = total objects
int          g_cliSmokeOutlinerSel    = -1;   // -1 = not attempted, 0/1 = first-object selected

// Set true after the auto-load block finishes ALL its work (generate + optional
// foliage + optional save). The smoke exit countdown starts from THIS, not from
// g_cliAutoLoadFired (which is set at block ENTRY) -- otherwise a slow generation
// or save can be cut off mid-flight by the timed ExitProcess.
bool         g_cliSmokeWorkDone   = false;

// -smoke-foliage-menu: drive the foliage MENU commands (Toggle/Clear/Generate)
// via WM_COMMAND -- the actual command path, not just the load hook. Records the
// visibility/count at each step into the [ESMOKE v1] summary for the runner to
// verify. Implies -smoke-foliage (so there is foliage loaded to toggle first).
bool         g_cliSmokeFoliageMenu = false;
void*        g_smokeMainHwnd       = NULL;   // HWND (typed void* -- declared before windows.h)
int          g_menuVis0 = -1, g_menuVis1 = -1, g_menuVis2 = -1;   // visibility across two toggles
int          g_menuClearCount = -1;          // Count() after Clear (expect 0)
int          g_menuReloadCount = -1;         // Count() after Generate/reload (expect >0)

static void editor_set_default_env_vars()
{
    // Bake editor-preferred defaults so RenderDoc and other tools can launch
    // the exe directly without a .bat. Bat overrides still work because we
    // only set when unset.
    if (!getenv("MC2_EDITOR_TRACE"))            _putenv_s("MC2_EDITOR_TRACE",              "1");
    // S3 (2026-05-25): MC2_GPU_DRIVEN, MC2_EDITOR_BYPASS_BLDG_CULL, MC2_STATIC_PROP_REGISTRY
    // sidesteps retired -- editor now runs canonical default-ON chain.
}

static void EarlyTrace(const char* msg)
{
	// Always feed crash ring — visible in last_trace.txt on CTD.
	crashbundle_append(msg);

	if (getenv("MC2_EDITOR_TRACE") == NULL)
		return;

    FILE* f = fopen("editor-startup.log", "a");
    if (f)
    {
        fprintf(f, "%s\n", msg);
        fclose(f);
    }
}

static void EarlyTraceBegin()
{
	if (getenv("MC2_EDITOR_TRACE") == NULL)
		return;

    // Open with "w" exactly once at true process start to clear any stale log,
    // then write the sentinel.  All subsequent writes use "a" (append).
    FILE* f = fopen("editor-startup.log", "w");
    if (f)
    {
        fprintf(f, "editor-startup.log begin\n");
        fclose(f);
    }
}

// EditorMFC.cpp : Defines the class behaviors for the application.
//

#include "stdafx.h"
#include "EditorMFC.h"

#include "MainFrm.h"
#include "editorinterface.h"
#include "EditorVersion.h"
#include "EditorData.h"
#include "FoliageRender.h"
#include "SceneOutliner.h"
#include "resource.h"   // ID_FOLIAGE_* for the -smoke-foliage-menu WM_COMMAND drive

// -- S-CLI parser ------------------------------------------------------------
// Accepts: -mission/--mission PATH (or =PATH), -frames/--frames N (or =N),
// -exit-on-load-fail / --exit-on-load-fail. Tokenizes a Windows-style cmdline
// honoring double-quoted spans (no escape processing beyond quote toggling —
// matches how MFC's CFileDialog hands paths back to itself).
static bool s_cli_flag_match(const char* tok, const char* shortName, const char* longName)
{
	return (strcmp(tok, shortName) == 0) || (strcmp(tok, longName) == 0);
}
static bool s_cli_starts_with(const char* tok, const char* prefix, const char** outRest)
{
	size_t n = strlen(prefix);
	if (strncmp(tok, prefix, n) == 0) { *outRest = tok + n; return true; }
	return false;
}
static void s_cli_parse(const char* cmd)
{
	if (!cmd || !*cmd) return;
	std::vector<std::string> argv;
	{
		std::string cur;
		bool inQuote = false;
		for (const char* p = cmd; *p; ++p)
		{
			char c = *p;
			if (c == '"') { inQuote = !inQuote; continue; }
			if (!inQuote && (c == ' ' || c == '\t'))
			{
				if (!cur.empty()) { argv.push_back(cur); cur.clear(); }
				continue;
			}
			cur.push_back(c);
		}
		if (!cur.empty()) argv.push_back(cur);
	}

	for (size_t i = 0; i < argv.size(); ++i)
	{
		const char* tok = argv[i].c_str();
		const char* rest = NULL;

		if (s_cli_flag_match(tok, "-mission", "--mission"))
		{
			if (i + 1 < argv.size()) { g_cliMissionPath = argv[++i]; }
		}
		else if (s_cli_starts_with(tok, "-mission=", &rest) ||
		         s_cli_starts_with(tok, "--mission=", &rest))
		{
			g_cliMissionPath = rest;
		}
		else if (s_cli_flag_match(tok, "-frames", "--frames"))
		{
			if (i + 1 < argv.size()) { g_cliFramesLimit = atol(argv[++i].c_str()); }
		}
		else if (s_cli_starts_with(tok, "-frames=", &rest) ||
		         s_cli_starts_with(tok, "--frames=", &rest))
		{
			g_cliFramesLimit = atol(rest);
		}
		else if (s_cli_flag_match(tok, "-exit-on-load-fail", "--exit-on-load-fail"))
		{
			g_cliExitOnLoadFail = true;
		}
		else if (s_cli_flag_match(tok, "-new-map", "--new-map"))
		{
			g_cliNewMap = true;
		}
		else if (s_cli_starts_with(tok, "-new-map=", &rest) ||
		         s_cli_starts_with(tok, "--new-map=", &rest))
		{
			// rest = "size" or "size,terrain"
			g_cliNewMap = true;
			g_cliNewMapSize = atol(rest);
			const char* comma = strchr(rest, ',');
			if (comma) g_cliNewMapTerrain = atol(comma + 1);
		}
		else if (s_cli_flag_match(tok, "-gen-map", "--gen-map"))
		{
			g_cliGenMap = true;
		}
		else if (s_cli_starts_with(tok, "-gen-map=", &rest) ||
		         s_cli_starts_with(tok, "--gen-map=", &rest))
		{
			g_cliGenMap = true;
			g_cliNewMapSize = atol(rest);
			const char* comma = strchr(rest, ',');
			if (comma) g_cliNewMapTerrain = atol(comma + 1);
		}
		else if (s_cli_flag_match(tok, "-smoke-foliage", "--smoke-foliage"))
		{
			g_cliSmokeFoliage = true;
			g_cliSmokeFoliagePath = "terrain_gen_out\\genmap.foliage.json";
		}
		else if (s_cli_starts_with(tok, "-smoke-foliage=", &rest) ||
		         s_cli_starts_with(tok, "--smoke-foliage=", &rest))
		{
			g_cliSmokeFoliage = true;
			g_cliSmokeFoliagePath = rest;
		}
		else if (s_cli_flag_match(tok, "-smoke-exit-sec", "--smoke-exit-sec"))
		{
			if (i + 1 < argv.size()) { g_cliExitAfterSec = atoi(argv[++i].c_str()); }
		}
		else if (s_cli_starts_with(tok, "-smoke-exit-sec=", &rest) ||
		         s_cli_starts_with(tok, "--smoke-exit-sec=", &rest))
		{
			g_cliExitAfterSec = atoi(rest);
		}
		else if (s_cli_flag_match(tok, "-smoke-foliage-menu", "--smoke-foliage-menu"))
		{
			g_cliSmokeFoliageMenu = true;
			g_cliSmokeFoliage = true;   // need foliage loaded before driving the menu
			if (g_cliSmokeFoliagePath.empty())
				g_cliSmokeFoliagePath = "terrain_gen_out\\genmap.foliage.json";
		}
		else if (s_cli_flag_match(tok, "-smoke-save", "--smoke-save"))
		{
			g_cliSmokeSave = true;
			g_cliSmokeSavePath = "terrain_gen_out\\smoke_saved.pak";
		}
		else if (s_cli_starts_with(tok, "-smoke-save=", &rest) ||
		         s_cli_starts_with(tok, "--smoke-save=", &rest))
		{
			g_cliSmokeSave = true;
			g_cliSmokeSavePath = rest;
		}
		else if (s_cli_flag_match(tok, "-smoke-outliner", "--smoke-outliner"))
		{
			g_cliSmokeOutliner = true;
		}
	}

	// Any smoke flag implies headless -> suppress the auto-run-path failure modals.
	g_cliSuppressModals = (g_cliExitAfterSec > 0) || g_cliSmokeFoliage || g_cliSmokeSave
	                      || g_cliSmokeOutliner;
}

// Forward declaration — defined in EditorGameOS.cpp.
// MFCPlatform.hpp is a legacy header that may not be present in all build
// configurations; declare the symbol directly to avoid the dependency.
void __stdcall InitGameOS(HINSTANCE hInstance, HWND hWindow, char* commandLine);

static EditorInterface* g_editorInterfaceWindow = NULL;

/////////////////////////////////////////////////////////////////////////////
// EditorMFCApp

BEGIN_MESSAGE_MAP(EditorMFCApp, CWinApp)
	//{{AFX_MSG_MAP(EditorMFCApp)
	ON_COMMAND(ID_APP_ABOUT, OnAppAbout)
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// EditorMFCApp construction

EditorMFCApp::EditorMFCApp()
	: m_bPendingInitGameOS(false)
	, m_hEditorWnd(NULL)
{
}

/////////////////////////////////////////////////////////////////////////////
// The one and only EditorMFCApp object

EditorMFCApp theApp;

/////////////////////////////////////////////////////////////////////////////
// EditorMFCApp initialization

static DWORD WINAPI s_smoke_thread(LPVOID);   // smoke driver: defined below

BOOL EditorMFCApp::InitInstance()
{
	editor_set_default_env_vars();
	crashbundle_init();    // install SEH filter; writes crashes/<timestamp>/ on CTD
	EarlyTraceBegin();
	EarlyTrace("InitInstance: enter");
	EarlyTrace(EditorVersion_GetStartupLine());

	// S-CLI: parse before any MFC chrome so flags are stable for the rest of
	// startup. MFC's default arg handling still receives m_lpCmdLine — we
	// don't consume it — but we use only explicit -mission so MFC's implicit
	// "first non-flag = file to shell-open" path stays untouched.
	s_cli_parse(m_lpCmdLine ? (const char*)m_lpCmdLine : "");
	fprintf(stderr, "[EDITOR_CLI v1] event=parsed mission=\"%s\" frames=%ld exit_on_load_fail=%d\n",
		g_cliMissionPath.c_str(), g_cliFramesLimit, g_cliExitOnLoadFail ? 1 : 0);
	fflush(stderr);

	// Start the smoke driver thread NOW (before the blocking init/gen/save on the
	// main thread) so it can auto-dismiss modals throughout and enforce the timed
	// exit. Defined below; forward-declared above InitInstance.
	if (g_cliExitAfterSec > 0)
	{
		EarlyTrace("InitInstance: starting smoke driver thread");
		::CreateThread(NULL, 0, s_smoke_thread, NULL, 0, NULL);
	}

#ifdef _AFXDLL
	// Enable3dControls() is deprecated and no-op on modern MSVC -- removed per C4996
#else
	Enable3dControlsStatic();	// Call this when linking to MFC statically
#endif

	SetRegistryKey(_T("Local AppWizard-Generated Applications"));

	MainFrame* pFrame = new MainFrame;
	m_pMainWnd = pFrame;
	EarlyTrace("InitInstance: m_pMainWnd set");

	// Create the real MFC frame first.  The EditorInterface view is normally
	// constructed from MainFrame::OnCreate; if that route fails, create one here.
	if (!pFrame->LoadFrame(IDR_EDITOR_MENU, WS_OVERLAPPEDWINDOW | FWS_ADDTOTITLE, NULL, NULL))
	{
		EarlyTrace("InitInstance: LoadFrame FAILED");
		delete pFrame;
		m_pMainWnd = NULL;
		return FALSE;
	}
	EarlyTrace("InitInstance: LoadFrame OK");

	HICON editorIco = LoadIcon(IDI_ICON1);
	if (editorIco)
	{
		pFrame->SetIcon(editorIco, TRUE);
		pFrame->SetIcon(editorIco, FALSE);
	}

	// Make sure the frame has a native MFC menu.  Some remastered startup paths
	// leave the frame alive but without a menu attached, which hides the editor UI.
	if (pFrame->GetMenu() == NULL)
	{
		CMenu* pMenu = new CMenu;
		if (pMenu->LoadMenu(IDR_EDITOR_MENU))
		{
			pFrame->SetMenu(pMenu);
			pMenu->Detach(); // ownership transferred to the frame HWND
			EarlyTrace("InitInstance: menu loaded and attached");
		}
		else
		{
			EarlyTrace("InitInstance: menu LoadMenu FAILED");
		}
		delete pMenu;
	}
	else
	{
		EarlyTrace("InitInstance: frame already has menu");
	}

	EditorInterface* editorInterface = EditorInterface::instance();
	if (!editorInterface)
	{
		EarlyTrace("InitInstance: EditorInterface missing after LoadFrame; creating fallback child");

		CRect clientRect;
		pFrame->GetClientRect(&clientRect);

		g_editorInterfaceWindow = new EditorInterface;
		if (!g_editorInterfaceWindow->Create(
			NULL,
			_T("EditorInterface"),
			WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
			clientRect,
			pFrame,
			AFX_IDW_PANE_FIRST,
			NULL))
		{
			EarlyTrace("InitInstance: fallback EditorInterface Create FAILED");
			delete g_editorInterfaceWindow;
			g_editorInterfaceWindow = NULL;
		}
		else
		{
			EarlyTrace("InitInstance: fallback EditorInterface Create OK");
			g_editorInterfaceWindow->ShowWindow(SW_MAXIMIZE);
			g_editorInterfaceWindow->UpdateWindow();
		}
	}
	else
	{
		EarlyTrace("InitInstance: EditorInterface exists after LoadFrame");
	}

	pFrame->ShowWindow(SW_MAXIMIZE);
	pFrame->UpdateWindow();
	pFrame->DrawMenuBar();

	HWND editorWnd = NULL;
	editorInterface = EditorInterface::instance();
	if (editorInterface && editorInterface->GetSafeHwnd())
	{
		editorWnd = editorInterface->GetSafeHwnd();
		EarlyTrace("InitInstance: EditorInterface hwnd valid");
	}
	else
	{
		EarlyTrace("InitInstance: EditorInterface hwnd NULL before InitGameOS");
	}

	// SDL_CreateWindow on Windows requires an active Win32 message pump on the
	// calling thread: WGL calls CreateWindowEx internally, which sends WM_CREATE
	// synchronously, and DWM/driver setup needs the pump to dispatch those messages.
	// Calling SDL_CreateWindow here (before CWinApp::Run() enters the pump) causes
	// a deadlock or driver crash that kills the process a few seconds in.
	//
	// Defer InitGameOS to the first OnIdle tick, at which point Run() has entered
	// the pump and the thread is fully message-capable.
	m_hEditorWnd          = editorWnd;
	m_sCmdLine            = m_lpCmdLine ? m_lpCmdLine : _T("");
	m_bPendingInitGameOS  = true;
	EarlyTrace("InitInstance: InitGameOS deferred to first OnIdle");

	if (editorInterface && editorInterface->GetSafeHwnd())
	{
		editorInterface->SetFocus();
	}
	else
	{
		pFrame->SetFocus();
	}

	EarlyTrace("InitInstance: return TRUE");
	return TRUE;
}

/////////////////////////////////////////////////////////////////////////////
// EditorMFCApp message handlers





/////////////////////////////////////////////////////////////////////////////
// CAboutDlg dialog used for App About

class CAboutDlg : public CDialog
{
public:
	CAboutDlg();

// Dialog Data
	//{{AFX_DATA(CAboutDlg)
	enum { IDD = IDD_FOG };
	//}}AFX_DATA

	// ClassWizard generated virtual function overrides
	//{{AFX_VIRTUAL(CAboutDlg)
	protected:
	virtual void DoDataExchange(CDataExchange* pDX);    // DDX/DDV support
	//}}AFX_VIRTUAL

// Implementation
protected:
	//{{AFX_MSG(CAboutDlg)
		// No message handlers
	//}}AFX_MSG
	DECLARE_MESSAGE_MAP()
};

CAboutDlg::CAboutDlg() : CDialog(CAboutDlg::IDD)
{
	//{{AFX_DATA_INIT(CAboutDlg)
	//}}AFX_DATA_INIT
}

void CAboutDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	//{{AFX_DATA_MAP(CAboutDlg)
	//}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CAboutDlg, CDialog)
	//{{AFX_MSG_MAP(CAboutDlg)
		// No message handlers
	//}}AFX_MSG_MAP
END_MESSAGE_MAP()

// App command to run the dialog
void EditorMFCApp::OnAppAbout()
{
	CAboutDlg aboutDlg;
	aboutDlg.DoModal();
}

/////////////////////////////////////////////////////////////////////////////
// EditorMFCApp message handlers


// Emit the machine-readable smoke summary exactly once (idempotent). Written to
// both the inherited stderr pipe and editor-startup.log (the reliable IPC channel
// for a WIN32-subsystem app). The Python runner turns these facts into a verdict.
static void s_emit_esmoke(const char* how)
{
	if (g_cliEsmokeEmitted) return;
	g_cliEsmokeEmitted = true;
	char esmoke[640];
	_snprintf(esmoke, sizeof(esmoke),
		"[ESMOKE v1] event=summary how=%s clean_exit=1 frames=%ld autoload=%d gen_map=%d "
		"foliage_smoke=%d foliage_count=%d saved=%d "
		"outliner_count=%d outliner_selected=%d "
		"menu_vis0=%d menu_vis1=%d menu_vis2=%d menu_clear=%d menu_reload=%d",
		how, g_cliFrameCounter, g_cliAutoLoadFired ? 1 : 0, g_cliGenMap ? 1 : 0,
		g_cliSmokeFoliageFired ? 1 : 0, g_cliSmokeFoliageCount, g_cliSmokeSaveOk,
		g_cliSmokeOutlinerCount, g_cliSmokeOutlinerSel,
		g_menuVis0, g_menuVis1, g_menuVis2, g_menuClearCount, g_menuReloadCount);
	esmoke[sizeof(esmoke) - 1] = '\0';
	fprintf(stderr, "%s\n", esmoke);
	fflush(stderr);
	EarlyTrace(esmoke);
}

// Auto-dismiss any modal dialog (#32770 = standard dialog/MessageBox class) owned
// by this process. Headless smoke runs have no one to click OK, and modals appear
// from many scattered places (startup, generate, and especially SAVE validation
// warnings that are built from resource keys and impossible to gate individually).
// We post IDOK + WM_CLOSE so both OK-only and OK/Cancel boxes go away.
// Single smoke driver thread, started at InitInstance. Once the auto-load work
// (generate + optional foliage + optional save) is done, it counts down
// g_cliExitAfterSec, emits the summary, and hard ExitProcess(0)s. A dedicated
// thread is immune to the editor's WM_TIMER render pump starving CWinApp::OnIdle.
// Modals are suppressed AT SOURCE (EMessageBox / EditorData AfxMessageBox gates on
// g_cliSuppressModals), so no fragile external dialog-dismissal is needed -- that
// approach (PostMessage WM_CLOSE) crashed the editor by hitting non-modal panels.
// An absolute backstop fires if the load step never completes.
static DWORD WINAPI s_smoke_thread(LPVOID)
{
	DWORD start = ::GetTickCount();
	DWORD loadTick = 0;
	DWORD sec = (DWORD)(g_cliExitAfterSec > 0 ? g_cliExitAfterSec : 12);
	// -smoke-foliage-menu: once foliage is loaded, drive the real menu commands via
	// WM_COMMAND (the same path a click takes) with renders in between, recording
	// visibility/count at each step. PostMessage from this thread queues onto the
	// main thread, which runs the handler between render frames.
	if (g_cliSmokeFoliageMenu)
	{
		while (!(g_cliSmokeWorkDone && g_smokeMainHwnd) && (::GetTickCount() - start) < 120000u)
			::Sleep(100);
		HWND w = (HWND)g_smokeMainHwnd;
		if (w)
		{
			::Sleep(700);
			g_menuVis0 = FoliageRender::Visible() ? 1 : 0;
			::PostMessageA(w, WM_COMMAND, MAKEWPARAM(ID_FOLIAGE_TOGGLE, 0), 0);  // off
			::Sleep(900);
			g_menuVis1 = FoliageRender::Visible() ? 1 : 0;
			::PostMessageA(w, WM_COMMAND, MAKEWPARAM(ID_FOLIAGE_TOGGLE, 0), 0);  // on
			::Sleep(900);
			g_menuVis2 = FoliageRender::Visible() ? 1 : 0;
			::PostMessageA(w, WM_COMMAND, MAKEWPARAM(ID_FOLIAGE_CLEAR, 0), 0);   // clear
			::Sleep(900);
			g_menuClearCount = FoliageRender::Count();
			::PostMessageA(w, WM_COMMAND, MAKEWPARAM(ID_FOLIAGE_GENERATE, 0), 0); // reload (shells generator)
			// Generate re-runs the generator synchronously on the main thread; poll
			// for the reloaded instance count.
			for (int i = 0; i < 60 && FoliageRender::Count() <= 0; ++i)
				::Sleep(500);
			g_menuReloadCount = FoliageRender::Count();
		}
		s_emit_esmoke("foliage_menu");
		::ExitProcess(0);
	}

	for (;;)
	{
		DWORD now = ::GetTickCount();
		if (g_cliSmokeWorkDone && loadTick == 0)
			loadTick = now;
		bool loadedDone = (loadTick != 0 && (now - loadTick) >= sec * 1000u);
		bool absBackstop = (now - start) >= 180000u;   // never-loaded safety exit
		if (loadedDone || absBackstop)
		{
			s_emit_esmoke(loadedDone ? "smoke_thread" : "abs_backstop");
			::ExitProcess(0);
		}
		::Sleep(200);
	}
	return 0;
}

BOOL EditorMFCApp::OnIdle(LONG lCount)
{
	CWinApp::OnIdle(lCount);

	// First-tick deferred initialisation: the MFC message pump is now live so
	// SDL_CreateWindow (which sends WM_CREATE synchronously via CreateWindowEx
	// and needs the pump to dispatch DWM/WGL messages) is safe to call.
	if (m_bPendingInitGameOS)
	{
		m_bPendingInitGameOS = false;
		EarlyTrace("OnIdle: deferred InitGameOS: enter");
		InitGameOS(m_hInstance, m_hEditorWnd, (char*)(LPCSTR)m_sCmdLine);
		EarlyTrace("OnIdle: deferred InitGameOS: complete");
	}

	// S-CLI: auto-load a mission on the FIRST idle after InitGameOS so the GL
	// context, asset system, and texture manager are fully up. One-shot.
	if (!g_cliAutoLoadFired && (!g_cliMissionPath.empty() || g_cliNewMap || g_cliGenMap) && !m_bPendingInitGameOS)
	{
		g_cliAutoLoadFired = true;
		bool ok = false;
		if (g_cliGenMap)
		{
			fprintf(stderr, "[EDITOR_CLI v1] event=gen_map_begin size=%d terrain=%d\n",
				g_cliNewMapSize, g_cliNewMapTerrain);
			fflush(stderr);
			EarlyTrace("OnIdle: S-CLI auto-gen-map: enter");
			ok = EditorData::generateMission(g_cliNewMapSize, g_cliNewMapTerrain, 12345u);
			EarlyTrace(ok ? "OnIdle: S-CLI auto-gen-map: OK" : "OnIdle: S-CLI auto-gen-map: FAILED");
			fprintf(stderr, "[EDITOR_CLI v1] event=gen_map_end ok=%d\n", ok ? 1 : 0);
			fflush(stderr);
		}
		else if (g_cliNewMap)
		{
			fprintf(stderr, "[EDITOR_CLI v1] event=new_map_begin size=%d terrain=%d\n",
				g_cliNewMapSize, g_cliNewMapTerrain);
			fflush(stderr);
			EarlyTrace("OnIdle: S-CLI auto-new-map: enter");
			ok = EditorData::initTerrainFromTGA(g_cliNewMapSize, 0, 0, g_cliNewMapTerrain);
			EarlyTrace(ok ? "OnIdle: S-CLI auto-new-map: OK" : "OnIdle: S-CLI auto-new-map: FAILED");
			fprintf(stderr, "[EDITOR_CLI v1] event=new_map_end ok=%d\n", ok ? 1 : 0);
			fflush(stderr);
		}
		else
		{
			EarlyTrace("OnIdle: S-CLI auto-load: enter");
			ok = EditorData::initTerrainFromPCV(g_cliMissionPath.c_str());
			EarlyTrace(ok ? "OnIdle: S-CLI auto-load: OK" : "OnIdle: S-CLI auto-load: FAILED");
		}
		if (!ok && g_cliExitOnLoadFail)
		{
			fprintf(stderr, "[EDITOR_CLI v1] event=load_fail mission=\"%s\" newmap=%d\n",
				g_cliMissionPath.c_str(), g_cliNewMap ? 1 : 0);
			fflush(stderr);
			exit(1);
		}

		// -smoke-foliage: exercise the foliage preview once the map is up. Load()
		// is failure-tolerant (missing/garbage JSON -> false, 0 instances), so this
		// must never crash; Toggle() flips visibility so the render path runs both
		// states over the remaining frames. Facts only -> [ESMOKE v1] at exit.
		if (g_cliSmokeFoliage)
		{
			bool floaded = FoliageRender::Load(g_cliSmokeFoliagePath.c_str());
			g_cliSmokeFoliageCount = FoliageRender::Count();
			FoliageRender::Toggle();   // exercise hidden state too
			FoliageRender::Toggle();   // ...and back to visible for the frame loop
			g_cliSmokeFoliageFired = true;
			fprintf(stderr, "[EDITOR_CLI v1] event=foliage loaded=%d count=%d path=\"%s\"\n",
				floaded ? 1 : 0, g_cliSmokeFoliageCount, g_cliSmokeFoliagePath.c_str());
			fflush(stderr);
			EarlyTrace(floaded ? "OnIdle: smoke-foliage Load OK" : "OnIdle: smoke-foliage Load returned false (tolerated)");
		}

		// -smoke-save: persist the generated map to a .pak so a follow-up -mission
		// run can load it (generate->save->load gate through the editor itself).
		// EditorData::instance is the live document singleton; save() bypasses the
		// interactive Save-As file dialog because we pass an explicit path.
		if (g_cliSmokeSave && ok && EditorData::instance)
		{
			// quickSave=true skips the heavy non-quickSave tail (updateTacMap +
			// MOVE rebuild + tac-map packet) that stalls headless on large maps. The
			// .pak still carries the terrain + object packets, so it reloads fine in
			// the editor (the load-back phase of the smoke).
			bool saved = EditorData::instance->save((char*)g_cliSmokeSavePath.c_str(), true);
			g_cliSmokeSaveOk = saved ? 1 : 0;
			fprintf(stderr, "[EDITOR_CLI v1] event=save ok=%d path=\"%s\"\n",
				saved ? 1 : 0, g_cliSmokeSavePath.c_str());
			fflush(stderr);
			EarlyTrace(saved ? "OnIdle: smoke-save OK" : "OnIdle: smoke-save FAILED");
		}

		// -smoke-outliner: exercise the read-only Scene Outliner. Open the panel,
		// enumerate placed objects (counts), and select the first selectable one
		// through the existing EditorObjectMgr path. All read-only except selection;
		// safe on an empty map (count 0, selected 0). Facts -> [ESMOKE v1] at exit.
		if (g_cliSmokeOutliner)
		{
			SceneOutliner::Open();
			OutlinerCounts oc = SceneOutliner::ComputeCounts();
			g_cliSmokeOutlinerCount = oc.total();
			g_cliSmokeOutlinerSel = SceneOutliner::SelectFirstObject() ? 1 : 0;
			fprintf(stderr, "[EDITOR_CLI v1] event=outliner count=%d selected=%d "
				"units=%d buildings=%d navmarkers=%d dropzones=%d forests=%d\n",
				g_cliSmokeOutlinerCount, g_cliSmokeOutlinerSel,
				oc.units, oc.buildings, oc.navMarkers, oc.dropZones, oc.forests);
			fflush(stderr);
			EarlyTrace("OnIdle: smoke-outliner done");
		}

		// All auto-load work (generate + foliage + save) is complete -> arm the
		// smoke exit countdown from here.
		g_cliSmokeWorkDone = true;
		EarlyTrace("OnIdle: smoke work done");
	}

	EditorInterface* pEditor = EditorInterface::instance();
	if (pEditor && ::IsWindow(pEditor->m_hWnd))
	{
		// The 3D viewport is an embedded child window.  The old foreground-window
		// check only repainted when the MFC frame itself was foreground, which
		// starves the viewport when focus is inside a child/control.
		::InvalidateRect(pEditor->m_hWnd, NULL, FALSE);
	}

	// Capture the main frame HWND on the main thread so the smoke driver thread can
	// PostMessage WM_COMMAND foliage-menu commands to it (-smoke-foliage-menu).
	if (g_cliSmokeFoliageMenu && g_smokeMainHwnd == NULL && AfxGetMainWnd())
		g_smokeMainHwnd = AfxGetMainWnd()->GetSafeHwnd();

	Sleep(2/*milliseconds*/); /* limits the framerate to 500fps */
	return 1;
}


int EditorMFCApp::ExitInstance()
{
	// Machine-readable smoke summary for the non-smoke-exit path (e.g. -frames or a
	// real user close). Idempotent with the OnIdle deadline emit.
	s_emit_esmoke("exit_instance");

	{
		Environment.TerminateGameEngine();
		gos_PushCurrentHeap(0); // TerminateGameEngine() forgets to do this
	}
	if (false) {
		ExitGameOS();
		if (!EditorInterface::instance()->m_hWnd)
		{
			/* ExitGameOS() shuts down directX which has the side effect of killing the
			EditorInterface window, so we recreate the window here. The editor window
			may not be referenced after this function is executed, but this is not the correct
			place for the EditorInterface window to be destroyed. */
			EditorInterface::instance()->Create(NULL, NULL, AFX_WS_DEFAULT_VIEW | WS_VSCROLL | WS_HSCROLL,
				CRect(0, 0, 0, 0), m_pMainWnd, AFX_IDW_PANE_FIRST, NULL);
		}
	}

	delete m_pMainWnd;
	m_pMainWnd = 0;

	return CWinApp::ExitInstance();
}

