#pragma once
#include <string>
#include <vector>
// Minimal dependency-free browser: a folder path field. On load, lists supported
// texture files in that folder. Selecting one returns its full path via takeSelection().
class FileBrowser {
public:
    void draw();
    bool hasSelection() const { return hasSelection_; }
    std::string takeSelection();
private:
    void refresh();
    char folderPath_[1024] = {0};
    std::vector<std::string> entries_;
    std::string scanError_;
    int selectedIndex_ = -1;
    bool hasSelection_ = false;
    std::string selectionPath_;
};
