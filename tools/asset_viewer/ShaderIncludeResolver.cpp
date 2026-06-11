// tools/asset_viewer/ShaderIncludeResolver.cpp
#include "ShaderIncludeResolver.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>

namespace fs = std::filesystem;

static bool readFile(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}
static std::string includePath(const std::string& line) {
    size_t h = line.find('#');
    if (h == std::string::npos) return "";
    // Ignore if a // comment precedes the # on this line.
    size_t cmt = line.find("//");
    if (cmt != std::string::npos && cmt < h) return "";
    size_t inc = line.find("include", h);
    if (inc == std::string::npos) return "";
    size_t lt = line.find_first_of("<\"", inc);
    if (lt == std::string::npos) return "";
    char close = (line[lt] == '<') ? '>' : '"';
    size_t end = line.find(close, lt + 1);
    if (end == std::string::npos) return "";
    return line.substr(lt + 1, end - lt - 1);
}
static bool inlineFile(const fs::path& root, const fs::path& file,
                       std::string& out, std::set<std::string>& stack,
                       ShaderResolveResult& res) {
    std::string key = fs::weakly_canonical(file).string();
    if (stack.count(key)) { res.error = "include cycle at " + file.string(); return false; }
    std::string text;
    if (!readFile(file, text)) { res.unresolved.push_back(file.string()); return false; }
    res.includedFiles.push_back(file.string());
    stack.insert(key);
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::string inc = includePath(line);
        if (!inc.empty()) {
            // Primary path: root/inc (e.g. shaders/include/lighting.hglsl)
            fs::path target = root / inc;
            // Fallback: root/include/bare-name (mirrors the engine's GL_ARB_shading_language_include
            // search path which has shaders/include/ mounted as a secondary root).
            if (!fs::exists(target)) {
                fs::path fallback = root / "include" / fs::path(inc).filename();
                if (fs::exists(fallback)) target = fallback;
            }
            if (!inlineFile(root, target, out, stack, res)) {
                if (!res.error.empty()) return false;
                out += "// [unresolved include: " + inc + "]\n";
            }
            continue;
        }
        out += line; out += '\n';
    }
    stack.erase(key);
    return true;
}
ShaderResolveResult ResolveShaderIncludes(const std::string& shaderRoot,
                                          const std::string& entryFile) {
    ShaderResolveResult res;
    fs::path root = shaderRoot;
    fs::path entry = fs::path(entryFile).is_absolute() ? fs::path(entryFile) : root / entryFile;
    std::set<std::string> stack;
    std::string out;
    bool ok = inlineFile(root, entry, out, stack, res);
    if (!res.error.empty()) { res.ok = false; return res; }
    res.source = out;
    res.ok = ok && res.unresolved.empty();
    return res;
}
