#include "FileBrowser.h"
#include "TextureDecoderRegistry.h"
#include "TextureExtensions.h"
#include "imgui.h"
#include <algorithm>
#include <string>
#include <filesystem>
#include <system_error>
#include <cstdio>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <shobjidl.h>   // IFileOpenDialog
#endif
namespace fs = std::filesystem;

#ifdef _WIN32
static fs::path ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) return fs::current_path();
    return fs::path(buf).parent_path();
}
#else
static fs::path ExeDir() { return fs::current_path(); }
#endif

#ifdef _WIN32
static bool PickTextureFileWin32(std::string& outPath) {
    bool ok = false;
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        // Build "*.png;*.jpg;...;*.ktx2" from the decoder registry (single source of truth).
        std::wstring pattern;
        for (const auto& e : textureDecoderRegistry().supportedExtensions()) {
            if (!pattern.empty()) pattern += L";";
            pattern += L"*.";
            pattern += std::wstring(e.begin(), e.end());   // ASCII ext -> wide
        }
        COMDLG_FILTERSPEC filters[] = {
            { L"Textures", pattern.c_str() },
            { L"All files (*.*)", L"*.*" },
        };
        dlg->SetFileTypes(2, filters);
        dlg->SetTitle(L"Select a texture");
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR wpath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath)) && wpath) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, nullptr, 0, nullptr, nullptr);
                    if (len > 1) {
                        std::string s((size_t)(len - 1), '\0');
                        WideCharToMultiByte(CP_UTF8, 0, wpath, -1, s.data(), len, nullptr, nullptr);
                        outPath = s; ok = true;
                    }
                    CoTaskMemFree(wpath);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(hrInit)) CoUninitialize();
    return ok;
}
#else
static bool PickTextureFileWin32(std::string&) { return false; }
#endif

FileBrowser::FileBrowser() {
    const char* rels[] = { "data/tgl/128", "data/tgl", "data/art/gui/test", "data/art/gui", "data/art", "data" };
    fs::path roots[] = { ExeDir(), fs::current_path() };
    for (const auto& root : roots) {
        for (const char* rel : rels) {
            std::error_code ec;
            fs::path cand = root / rel;
            if (fs::is_directory(cand, ec)) {
                std::snprintf(folderPath_, sizeof(folderPath_), "%s", cand.string().c_str());
                refresh();
                return;
            }
        }
    }
}

void FileBrowser::selectFile(const std::string& fullPath) {
    fs::path p(fullPath);
    std::snprintf(folderPath_, sizeof(folderPath_), "%s", p.parent_path().string().c_str());
    refresh();
    selectionPath_ = fullPath;
    hasSelection_ = true;
    std::string name = p.filename().string();
    for (int i = 0; i < (int)entries_.size(); ++i)
        if (entries_[i] == name) { selectedIndex_ = i; break; }
}

void FileBrowser::refresh()
{
    entries_.clear();
    scanError_.clear();
    selectedIndex_ = -1;
    rescanTiers();                      // refresh tier list on folder change (NOT per draw frame)
    std::error_code ec;
    fs::path dir(folderPath_);
    if (!fs::is_directory(dir, ec)) { scanError_ = "Not a folder."; return; }
    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::string name = it->path().filename().string();
        if (IsSupportedTextureFile(name)) entries_.push_back(name);
    }
    if (ec) scanError_ = std::string("Scan error: ") + ec.message();
    else if (entries_.empty() && scanError_.empty()) {
        std::string extList;
        for (const auto& e : textureDecoderRegistry().supportedExtensions()) {
            if (!extList.empty()) extList += "/";
            extList += "." + e;
        }
        scanError_ = "No supported textures (" + extList + ") here.";
    }
}

void FileBrowser::draw()
{
    std::vector<std::string> tiers = SiblingTiers();
    if (tiers.size() > 1) {
        std::string cur = CurrentTier();
        ImGui::TextUnformatted("Resolution:");
        for (const auto& t : tiers) {
            ImGui::SameLine();
            bool active = (t == cur);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.50f, 0.80f, 1.0f));
            if (ImGui::Button(t.c_str())) SwitchTier(t);
            if (active) ImGui::PopStyleColor();
        }
        ImGui::Separator();
    }
    ImGui::TextUnformatted("Folder");
    ImGui::SetNextItemWidth(-150.0f);
    ImGui::InputText("##folder", folderPath_, sizeof(folderPath_));
    ImGui::SameLine();
    if (ImGui::Button("Load")) refresh();
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::string picked;
        if (PickTextureFileWin32(picked)) selectFile(picked);
    }
    if (!scanError_.empty())
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f), "%s", scanError_.c_str());
    ImGui::Separator();
    ImGui::BeginChild("file_list", ImVec2(0, 0), true);
    for (int i = 0; i < (int)entries_.size(); ++i) {
        bool selected = (i == selectedIndex_);
        if (ImGui::Selectable(entries_[i].c_str(), selected)) {
            selectedIndex_ = i;
            selectionPath_ = (fs::path(folderPath_) / entries_[i]).string();
            hasSelection_ = true;
        }
    }
    ImGui::EndChild();
}

std::string FileBrowser::takeSelection()
{
    hasSelection_ = false;
    return selectionPath_;
}

static bool fb_isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

// folderPath_ may carry a trailing separator (e.g. user types "…/128\" in the
// InputText box). Strip it so filename()/parent_path() behave (review fix:
// otherwise CurrentTier() returns "" and SwitchTier() targets the wrong dir).
static fs::path fb_normFolder(const char* raw) {
    std::string s = raw ? raw : "";
    while (!s.empty() && (s.back() == '/' || s.back() == '\\')) s.pop_back();
    return fs::path(s);
}

void FileBrowser::setFolder(const std::string& path) {
    std::snprintf(folderPath_, sizeof(folderPath_), "%s", path.c_str());
    refresh();
}

std::string FileBrowser::CurrentTier() const {
    std::string leaf = fb_normFolder(folderPath_).filename().string();
    return fb_isAllDigits(leaf) ? leaf : std::string();
}

void FileBrowser::rescanTiers() {
    tiers_.clear();
    fs::path parent = fb_normFolder(folderPath_).parent_path();
    std::error_code ec;
    if (parent.empty() || !fs::is_directory(parent, ec)) return;
    auto it = fs::directory_iterator(parent, ec);
    if (ec) return;                                 // open failed (perms etc.) -> no tiers (review m2)
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) break;                              // stop on iteration error (review m2)
        std::error_code dec;                        // separate ec so an entry error
        if (!it->is_directory(dec) || dec) continue; // doesn't abort the whole scan
        std::string name = it->path().filename().string();
        // <=7 digits keeps values within int range; avoids std::stoi overflow (review m1)
        if (fb_isAllDigits(name) && name.size() <= 7) tiers_.push_back(name);
    }
    // Ascending numeric order WITHOUT std::stoi: for all-digit strings, fewer digits
    // == smaller; same length == lexical order == numeric order.
    std::sort(tiers_.begin(), tiers_.end(),
              [](const std::string& a, const std::string& b){
                  if (a.size() != b.size()) return a.size() < b.size();
                  return a < b;
              });
}

void FileBrowser::SwitchTier(const std::string& tier) {
    fs::path dst = fb_normFolder(folderPath_).parent_path() / tier;
    std::error_code ec;
    if (!fs::is_directory(dst, ec)) return;   // missing tier -> no-op

    std::string keepName;                     // remember selected filename
    if (selectedIndex_ >= 0 && selectedIndex_ < (int)entries_.size())
        keepName = entries_[selectedIndex_];
    else if (hasSelection_ && !selectionPath_.empty())   // selection set via Browse w/o a list index (review final #2)
        keepName = fs::path(selectionPath_).filename().string();

    setFolder(dst.string());                  // repoint + rescan (clears entries_/selectedIndex_)
    selectedIndex_ = -1;
    hasSelection_  = false;
    if (!keepName.empty()) {                   // restore selection if same file exists here
        for (int i = 0; i < (int)entries_.size(); ++i) {
            if (entries_[i] == keepName) {
                selectedIndex_ = i;
                selectionPath_ = (fs::path(folderPath_) / keepName).string();
                hasSelection_  = true;
                break;
            }
        }
    }
}
