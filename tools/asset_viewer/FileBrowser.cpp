#include "FileBrowser.h"
#include "TextureExtensions.h"
#include "imgui.h"
#include <filesystem>
#include <system_error>
namespace fs = std::filesystem;

void FileBrowser::refresh()
{
    entries_.clear();
    scanError_.clear();
    selectedIndex_ = -1;
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
    else if (entries_.empty() && scanError_.empty())
        scanError_ = "No supported textures (.png/.jpg/.jpeg/.bmp/.tga) here.";
}

void FileBrowser::draw()
{
    ImGui::TextUnformatted("Folder");
    ImGui::SetNextItemWidth(-70.0f);
    ImGui::InputText("##folder", folderPath_, sizeof(folderPath_));
    ImGui::SameLine();
    if (ImGui::Button("Load")) refresh();
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
