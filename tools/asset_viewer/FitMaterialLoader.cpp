// tools/asset_viewer/FitMaterialLoader.cpp
#include "FitMaterialLoader.h"
#include <fstream>
#include <sstream>
#include <cctype>

static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
}
static std::string unquote(std::string s) {
    s = trim(s);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') return s.substr(1, s.size() - 2);
    return s;
}

FitMaterial FitMaterialLoader_Parse(const std::string& fitPath, std::string* errorOut) {
    FitMaterial m;
    std::ifstream f(fitPath);
    if (!f) { if (errorOut) *errorOut = "cannot open " + fitPath; return m; }
    std::stringstream ss; ss << f.rdbuf();
    std::string text = ss.str();

    size_t mp = text.find("Material");
    if (mp == std::string::npos) { if (errorOut) *errorOut = "no Material keyword"; return m; }
    size_t ob = text.find('{', mp);
    size_t cb = (ob == std::string::npos) ? std::string::npos : text.find('}', ob);
    if (ob == std::string::npos || cb == std::string::npos) { if (errorOut) *errorOut = "no { } block"; return m; }

    std::string body = text.substr(ob + 1, cb - ob - 1);
    std::istringstream bs(body);
    std::string line;
    auto assign = [&](const std::string& key, const std::string& val) {
        if      (key == "baseColor")  m.baseColor  = unquote(val);
        else if (key == "normal")     m.normal     = unquote(val);
        else if (key == "orm")        m.orm        = unquote(val);
        else if (key == "emissive")   m.emissive   = unquote(val);
        else if (key == "shader")     m.shader     = unquote(val);
        else if (key == "ormPacking") m.ormPacking = unquote(val);
        else if (key == "alphaMode")  m.alphaMode  = unquote(val);
    };
    while (std::getline(bs, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        assign(trim(line.substr(0, eq)), line.substr(eq + 1));
    }
    m.found = true;
    return m;
}
