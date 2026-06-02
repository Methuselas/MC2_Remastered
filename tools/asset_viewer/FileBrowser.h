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
private:
    void refresh();                    // re-scan folderPath_ into entries_
    void selectFile(const std::string& fullPath);  // set folder=parent, refresh, select that file
    char folderPath_[1024] = {0};
    std::vector<std::string> entries_;
    std::string scanError_;
    int selectedIndex_ = -1;
    bool hasSelection_ = false;
    std::string selectionPath_;
};
