//-------------------------------------------------------------------------------------------------
// EditorModProject.cpp -- see EditorModProject.h.
//
// Mod Project = an editor session binding to a `mods/<id>/` folder.  This file owns the
// project state, the minimal hand-rolled mod.json read/write (mirrors mclib/file.cpp's
// ReadModJson style), the scaffold for New Mod Project, the save-dir override that File
// dialogs consult, ModPicker mounting, and the last-project pointer persistence.
//
// MID-SESSION MOUNT NOTE: ModPicker::Activate() sets MC2_ACTIVE_MOD + re-runs
// InitModSearchPaths (clear + re-index) -- the same path the MFC startup dialog uses.  It
// is designed as a PRE-LOAD selector, so mounting takes effect for the NEXT mission load.
// We call it on Open/CreateNew and surface a status hint to load/reload the mission to apply.
//-------------------------------------------------------------------------------------------------
#include "stdafx.h"   // MFC / <windows.h>

#include "EditorModProject.h"
#include "ModPicker.h"

#ifdef MC2_IMGUI
#include "imgui.h"
#include <shlobj.h>   // SHBrowseForFolder
#endif

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace EditorModProject
{
namespace {

bool         s_active = false;
std::string  s_root;        // `...\mods\<id>` absolute dir
std::string  s_id;
std::string  s_name;
std::string  s_version;
std::vector<std::string> s_deps;
std::string  s_saveDir;     // cached `<root>\data\missions` for SaveDirOverride()
char         s_status[512] = "No mod project open.";

// Pointer file: where we remember the last opened project across launches.  Kept next to the
// editor exe so it is independent of any single mod root.
const char* kPointerFile = "editor-modproject.txt";

// --- path / file helpers --------------------------------------------------------------

bool FileExists(const char* p)
{
	DWORD a = GetFileAttributesA(p);
	return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const char* p)
{
	DWORD a = GetFileAttributesA(p);
	return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string AbsPath(const std::string& in)
{
	char buf[1024];
	DWORD n = GetFullPathNameA(in.c_str(), sizeof(buf), buf, NULL);
	if (n > 0 && n < sizeof(buf))
		return std::string(buf);
	return in;
}

// Strip a trailing path separator (so `...\<id>\` -> `...\<id>`).
std::string TrimTrailingSep(const std::string& in)
{
	std::string s = in;
	while (!s.empty() && (s.back() == '\\' || s.back() == '/'))
		s.pop_back();
	return s;
}

// Last path component (the id, given a `...\mods\<id>` root).
std::string LastComponent(const std::string& path)
{
	std::string p = TrimTrailingSep(path);
	size_t slash = p.find_last_of("/\\");
	return (slash != std::string::npos) ? p.substr(slash + 1) : p;
}

// mkdir -p for an (absolute or relative) path: create each level as we walk separators.
void MakeDirTree(const std::string& dir)
{
	std::string acc;
	for (size_t i = 0; i < dir.size(); ++i)
	{
		char c = dir[i];
		acc.push_back(c);
		if (c == '\\' || c == '/')
			CreateDirectoryA(acc.c_str(), NULL);
	}
	CreateDirectoryA(acc.c_str(), NULL);
}

std::string ReadFileText(const char* path)
{
	HANDLE fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (fh == INVALID_HANDLE_VALUE)
		return std::string();
	std::string out;
	char buf[4096];
	DWORD read = 0;
	while (ReadFile(fh, buf, sizeof(buf), &read, NULL) && read > 0)
		out.append(buf, read);
	CloseHandle(fh);
	return out;
}

bool WriteFileText(const char* path, const std::string& text)
{
	HANDLE fh = CreateFileA(path, GENERIC_WRITE, 0, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (fh == INVALID_HANDLE_VALUE)
		return false;
	DWORD written = 0;
	BOOL ok = WriteFile(fh, text.data(), (DWORD)text.size(), &written, NULL);
	CloseHandle(fh);
	return ok && written == text.size();
}

// --- minimal JSON (mirrors mclib/file.cpp ReadModJson; no full parser) -----------------

std::string JsonGetString(const std::string& json, const char* key)
{
	std::string needle = std::string("\"") + key + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string::npos) return std::string();
	const char* p = json.c_str() + pos + needle.size();
	while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	if (*p != '"') return std::string();
	p++;
	const char* end = strchr(p, '"');
	if (!end) return std::string();
	return std::string(p, (size_t)(end - p));
}

std::vector<std::string> JsonGetStringArray(const std::string& json, const char* key)
{
	std::vector<std::string> result;
	std::string needle = std::string("\"") + key + "\"";
	size_t pos = json.find(needle);
	if (pos == std::string::npos) return result;
	const char* p = json.c_str() + pos + needle.size();
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	if (*p != '[') return result;
	p++;
	const char* end = strchr(p, ']');
	if (!end) return result;
	while (p < end)
	{
		while (p < end && (*p == ' ' || *p == '\t' || *p == ',' || *p == '\r' || *p == '\n')) p++;
		if (*p == '"')
		{
			p++;
			const char* se = strchr(p, '"');
			if (!se || se > end) break;
			result.push_back(std::string(p, (size_t)(se - p)));
			p = se + 1;
		}
		else p++;
	}
	return result;
}

// Serialize a minimal mod.json from the given fields.
std::string BuildModJson(const std::string& id, const std::string& name,
	const std::string& version, const std::vector<std::string>& deps)
{
	std::string s;
	s += "{\n";
	s += "  \"id\": \"" + id + "\",\n";
	s += "  \"name\": \"" + name + "\",\n";
	s += "  \"version\": \"" + version + "\",\n";
	s += "  \"dependencies\": [";
	for (size_t i = 0; i < deps.size(); ++i)
	{
		if (i) s += ", ";
		s += "\"" + deps[i] + "\"";
	}
	s += "]\n";
	s += "}\n";
	return s;
}

// --- pointer-file persistence ----------------------------------------------------------

void WritePointer(const std::string& rootAbs)
{
	WriteFileText(kPointerFile, rootAbs);
}

std::string ReadPointer()
{
	std::string s = ReadFileText(kPointerFile);
	// Trim trailing whitespace/newlines.
	while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t'))
		s.pop_back();
	return s;
}

// Write/update the tool-private .modproject\editor.json (records the project's own root +
// version so a future editor can self-describe).  Non-fatal on failure.
void WriteEditorJson(const std::string& rootAbs, const std::string& id, const std::string& version)
{
	std::string dir = rootAbs + "\\.modproject";
	MakeDirTree(dir);
	std::string js;
	js += "{\n";
	js += "  \"projectRoot\": \"";
	// escape backslashes for valid JSON
	for (char c : rootAbs) { if (c == '\\') js += "\\\\"; else js += c; }
	js += "\",\n";
	js += "  \"id\": \"" + id + "\",\n";
	js += "  \"version\": \"" + version + "\"\n";
	js += "}\n";
	WriteFileText((dir + "\\editor.json").c_str(), js);
}

// Bind state to a validated root with parsed fields, mount the mod, persist the pointer.
void Bind(const std::string& rootAbs, const std::string& id, const std::string& name,
	const std::string& version, const std::vector<std::string>& deps)
{
	s_active  = true;
	s_root    = rootAbs;
	s_id      = id;
	s_name    = name.empty() ? id : name;
	s_version = version;
	s_deps    = deps;
	s_saveDir = rootAbs + "\\data\\missions";

	// Mount the mod for asset loading (pre-load selector; takes effect on next mission load).
	ModPicker::Activate(id.c_str());

	WritePointer(rootAbs);
	WriteEditorJson(rootAbs, id, version);
}

} // anonymous namespace

// --- accessors ------------------------------------------------------------------------

bool        IsActive()  { return s_active; }
const char* RootPath()  { return s_root.c_str(); }
const char* Id()        { return s_id.c_str(); }
const char* Name()      { return s_name.c_str(); }
const char* Version()   { return s_version.c_str(); }
const char* StatusLine(){ return s_status; }

const char* SaveDirOverride()
{
	if (!s_active || s_saveDir.empty())
		return NULL;
	return s_saveDir.c_str();
}

// --- lifecycle ------------------------------------------------------------------------

bool Open(const char* folder, bool offerCreate)
{
	if (!folder || !folder[0])
	{
		snprintf(s_status, sizeof(s_status), "Open Mod Project: empty folder.");
		return false;
	}

	std::string rootAbs = TrimTrailingSep(AbsPath(folder));
	if (!DirExists(rootAbs.c_str()))
	{
		snprintf(s_status, sizeof(s_status), "Open Mod Project: folder not found (%s).", rootAbs.c_str());
		return false;
	}

	std::string id = LastComponent(rootAbs);
	std::string jsonPath = rootAbs + "\\mod.json";

	std::string name, version;
	std::vector<std::string> deps;

	if (FileExists(jsonPath.c_str()))
	{
		std::string js = ReadFileText(jsonPath.c_str());
		std::string jid = JsonGetString(js, "id");
		if (!jid.empty()) id = jid;          // honor explicit id over folder name
		name    = JsonGetString(js, "name");
		version = JsonGetString(js, "version");
		deps    = JsonGetStringArray(js, "dependencies");
	}
	else if (offerCreate)
	{
		// No manifest -- scaffold a minimal one in place (id derived from folder).
		name    = id;
		version = "0.1.0";
		std::string js = BuildModJson(id, name, version, deps);
		if (!WriteFileText(jsonPath.c_str(), js))
		{
			snprintf(s_status, sizeof(s_status),
				"Open Mod Project: no mod.json and could not create one (%s).", jsonPath.c_str());
			return false;
		}
	}
	else
	{
		snprintf(s_status, sizeof(s_status), "Open Mod Project: no mod.json in %s.", rootAbs.c_str());
		return false;
	}

	// Ensure the standard data\missions tree exists so saves land cleanly.
	MakeDirTree(rootAbs + "\\data\\missions");

	Bind(rootAbs, id, name, version, deps);
	snprintf(s_status, sizeof(s_status),
		"Project '%s' open. Saves default to data\\missions; mod mounted -- load/reload the mission to apply assets.",
		s_id.c_str());
	return true;
}

bool CreateNew(const char* parentDir, const char* id)
{
	if (!id || !id[0])
	{
		snprintf(s_status, sizeof(s_status), "New Mod Project: empty id.");
		return false;
	}

	std::string parent = (parentDir && parentDir[0]) ? std::string(parentDir) : std::string("mods");
	std::string rootAbs = TrimTrailingSep(AbsPath(parent + "\\" + id));

	std::string jsonPath = rootAbs + "\\mod.json";
	if (FileExists(jsonPath.c_str()) && !ReadFileText(jsonPath.c_str()).empty())
	{
		snprintf(s_status, sizeof(s_status),
			"New Mod Project: '%s' already has a mod.json -- use Open instead.", rootAbs.c_str());
		return false;
	}

	// Scaffold the tree: <root>\data\missions and <root>\.modproject.
	MakeDirTree(rootAbs);
	MakeDirTree(rootAbs + "\\data\\missions");
	MakeDirTree(rootAbs + "\\.modproject");

	std::vector<std::string> deps;
	std::string name = id;
	std::string version = "0.1.0";
	if (!WriteFileText(jsonPath.c_str(), BuildModJson(id, name, version, deps)))
	{
		snprintf(s_status, sizeof(s_status),
			"New Mod Project: failed to write mod.json (%s).", jsonPath.c_str());
		return false;
	}

	Bind(rootAbs, id, name, version, deps);
	snprintf(s_status, sizeof(s_status),
		"Created project '%s' at %s. Saves default to data\\missions.", s_id.c_str(), rootAbs.c_str());
	return true;
}

void Close()
{
	s_active = false;
	s_root.clear();
	s_id.clear();
	s_name.clear();
	s_version.clear();
	s_deps.clear();
	s_saveDir.clear();
	snprintf(s_status, sizeof(s_status), "Mod project closed.");
}

void ReopenLastIfAny()
{
	std::string last = ReadPointer();
	if (last.empty())
		return;
	if (!DirExists(last.c_str()))
		return;
	// offerCreate=false: do not silently scaffold a manifest at startup; just bind if valid.
	if (!Open(last.c_str(), /*offerCreate=*/false))
	{
		// Leave a benign status; a missing manifest is not fatal at startup.
		snprintf(s_status, sizeof(s_status),
			"Last mod project (%s) could not be reopened.", last.c_str());
	}
}

// --- ImGui panel ----------------------------------------------------------------------
#ifdef MC2_IMGUI

namespace {
bool s_panelOpen = false;
char s_newId[128] = "my_mod";
char s_newParent[512] = "mods";
bool s_openNewPopup = false;

// Modal MFC folder picker (SHBrowseForFolder).  Returns selected path or "" on cancel.
std::string PickFolder(const char* title)
{
	std::string result;
	BROWSEINFOA bi;
	memset(&bi, 0, sizeof(bi));
	bi.lpszTitle = title;
	bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
	LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
	if (pidl)
	{
		char path[MAX_PATH] = "";
		if (SHGetPathFromIDListA(pidl, path))
			result = path;
		CoTaskMemFree(pidl);
	}
	return result;
}
} // anonymous namespace

void Toggle() { s_panelOpen = !s_panelOpen; }

void Draw()
{
	if (!s_panelOpen)
		return;

	ImGui::SetNextWindowSize(ImVec2(440.f, 0.f), ImGuiCond_FirstUseEver);
	if (!ImGui::Begin("Mod Project", &s_panelOpen))
	{
		ImGui::End();
		return;
	}

	if (s_active)
	{
		ImGui::Text("Active: %s", s_id.c_str());
		ImGui::Text("Name: %s", s_name.c_str());
		ImGui::Text("Version: %s", s_version.empty() ? "(none)" : s_version.c_str());
		ImGui::TextWrapped("Root: %s", s_root.c_str());
	}
	else
	{
		ImGui::TextDisabled("No project open.");
	}
	ImGui::Separator();

	if (ImGui::Button("Open Mod Project...", ImVec2(-1.f, 0.f)))
	{
		std::string folder = PickFolder("Select a mods\\<id> project folder");
		if (!folder.empty())
			Open(folder.c_str(), /*offerCreate=*/true);
	}

	if (ImGui::Button("New Mod Project...", ImVec2(-1.f, 0.f)))
		s_openNewPopup = true;

	if (s_active)
	{
		if (ImGui::Button("Close Project", ImVec2(-1.f, 0.f)))
			Close();
	}

	if (s_openNewPopup)
	{
		ImGui::OpenPopup("New Mod Project");
		s_openNewPopup = false;
	}
	if (ImGui::BeginPopupModal("New Mod Project", NULL, ImGuiWindowFlags_AlwaysAutoResize))
	{
		ImGui::TextUnformatted("Creates <parent>\\<id>\\{mod.json, data\\missions\\, .modproject\\}");
		ImGui::SetNextItemWidth(300.f);
		ImGui::InputText("Mod id", s_newId, sizeof(s_newId));
		ImGui::SetNextItemWidth(300.f);
		ImGui::InputText("Parent dir", s_newParent, sizeof(s_newParent));
		ImGui::SameLine();
		if (ImGui::SmallButton("..."))
		{
			std::string folder = PickFolder("Select the parent folder (usually mods)");
			if (!folder.empty())
			{
				strncpy(s_newParent, folder.c_str(), sizeof(s_newParent) - 1);
				s_newParent[sizeof(s_newParent) - 1] = '\0';
			}
		}
		ImGui::Separator();
		const bool canCreate = (s_newId[0] != '\0');
		if (!canCreate) ImGui::BeginDisabled();
		if (ImGui::Button("Create", ImVec2(120.f, 0.f)))
		{
			if (CreateNew(s_newParent, s_newId))
				ImGui::CloseCurrentPopup();
		}
		if (!canCreate) ImGui::EndDisabled();
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120.f, 0.f)))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}

	ImGui::Separator();
	ImGui::TextWrapped("%s", s_status);

	ImGui::End();
}

#endif // MC2_IMGUI

} // namespace EditorModProject
