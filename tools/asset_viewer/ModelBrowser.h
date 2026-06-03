// tools/asset_viewer/ModelBrowser.h
// Simple list browser over the FST's .tgl entries for the Static Props panel.
// Wraps TglMeshLoader::ensureFastFile + listTgl; provides filter box + scrollable list.
#pragma once
#include <string>
#include <vector>

class ModelBrowser {
public:
    void draw();
    bool hasSelection() const { return hasSelection_; }
    // Returns the picked .tgl name (e.g. "data/tgl/2civliving.tgl") and clears the flag.
    std::string takeSelection();

private:
    void ensureList();          // calls TglMeshLoader once; idempotent
    void rebuildFiltered();     // rebuild filtered_ from all_ + filter_

    std::vector<std::string> all_;
    std::vector<std::string> filtered_;
    char filter_[128] = {0};
    int  selected_    = -1;
    bool listed_      = false;
    bool hasSelection_= false;
    std::string selectionPath_;
};
