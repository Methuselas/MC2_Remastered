#pragma once
#include <string>
#include <vector>
// Folder-path browser with a native Browse button (Win32). On load/browse it lists
// supported texture files; selecting one returns its full path via takeSelection().
class FileBrowser {
public:
    FileBrowser();                     // pre-fills folderPath_ with a default art dir + scans
    void draw();
    bool hasSelection() const { return hasSelection_; }
    std::string takeSelection();

    void selectFile(const std::string& fullPath);          // select a known file (sets folder=parent)
    const std::string& selectionPath() const { return selectionPath_; }
    void setFolder(const std::string& path);               // set folder + rescan
    // Resolution tiers = numeric-named sibling folders of the current folder.
    std::vector<std::string> SiblingTiers() const;         // ascending, e.g. {"64","128","256"}
    std::string CurrentTier() const;                        // current folder leaf if numeric, else ""
    void SwitchTier(const std::string& tier);               // repoint to <parent>/<tier>, keep same filename if present
private:
    void refresh();                    // re-scan folderPath_ into entries_
    char folderPath_[1024] = {0};
    std::vector<std::string> entries_;
    std::string scanError_;
    int selectedIndex_ = -1;
    bool hasSelection_ = false;
    std::string selectionPath_;
};
