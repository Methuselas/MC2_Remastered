// tools/asset_viewer/AppearanceRoster.cpp
#include "AppearanceRoster.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static std::string trimmed(std::string s) {
    auto notspace = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notspace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notspace).base(), s.end());
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}
static std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void AppearanceRoster::load(const std::string& deployDir) {
    if (loaded_ && loadedDir_ == deployDir) return;
    names_.clear();
    scannedFiles_ = 0;
    loadedDir_ = deployDir;
    loaded_ = true;

    fs::path tglDir = fs::path(deployDir) / "data" / "tgl";
    std::error_code ec;
    if (!fs::is_directory(tglDir, ec)) return;

    std::vector<std::string> seenLower;
    for (auto& de : fs::directory_iterator(tglDir, ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        if (lower(de.path().extension().string()) != ".ini") continue;
        ++scannedFiles_;
        std::ifstream f(de.path());
        std::string line;
        while (std::getline(f, line)) {
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string key = lower(trimmed(line.substr(0, eq)));
            if (key != "filename") continue;
            std::string val = trimmed(line.substr(eq + 1));
            if (val.empty()) continue;
            std::string lv = lower(val);
            if (std::find(seenLower.begin(), seenLower.end(), lv) != seenLower.end()) break;
            seenLower.push_back(lv);
            names_.push_back(val);
            break;
        }
    }
    std::sort(names_.begin(), names_.end(),
              [](const std::string& a, const std::string& b){ return lower(a) < lower(b); });
}

void AppearanceRoster::refresh(const std::string& deployDir) {
    loaded_ = false;
    loadedDir_.clear();
    load(deployDir);
}

bool AppearanceRoster::contains(const std::string& name) const {
    std::string ln = lower(name);
    for (const auto& n : names_) if (lower(n) == ln) return true;
    return false;
}
