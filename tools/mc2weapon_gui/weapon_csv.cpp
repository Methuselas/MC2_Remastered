// weapon_csv.cpp — see weapon_csv.h.
#include "weapon_csv.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>   // _mkdir
#endif

namespace mc2w {

const char* kWeaponTypes[3] = {"EnergyWeapon", "BallisticWeapon", "MissileWeapon"};
const char* kRanges[3] = {"short", "medium", "long"};

static std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) ++b;
    while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
    return s.substr(b, e - b);
}

// RFC-ish CSV line parse (handles "quoted, fields" and "" escapes).
static std::vector<std::string> parseCsvLine(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inq) {
            if (c == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
                else inq = false;
            } else cur += c;
        } else {
            if (c == '"') inq = true;
            else if (c == ',') { out.push_back(cur); cur.clear(); }
            else if (c == '\r') { /* skip */ }
            else cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

static std::string emitCsvField(const std::string& f) {
    bool need = f.find_first_of(",\"\n") != std::string::npos;
    if (!need) return f;
    std::string out = "\"";
    for (char c : f) { if (c == '"') out += "\"\""; else out += c; }
    out += "\"";
    return out;
}
static std::string emitCsvLine(const std::vector<std::string>& cells) {
    std::string out;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i) out += ",";
        out += emitCsvField(cells[i]);
    }
    return out;
}

static int colIndex(const std::vector<std::string>& header, const std::string& needle) {
    std::string nl = lower(needle);
    for (size_t i = 0; i < header.size(); ++i) {
        std::string hl = lower(trim(header[i]));
        if (hl.rfind(nl, 0) == 0 || hl.find(nl) != std::string::npos) return (int)i;
    }
    return -1;
}

bool isWeaponType(const std::string& type) {
    std::string t = lower(trim(type));
    return t == "energyweapon" || t == "ballisticweapon" || t == "missileweapon";
}

bool Compbas::load(const std::string& path, std::string& err) {
    std::ifstream in(path.c_str());
    if (!in.is_open()) { err = "cannot open " + path; return false; }
    header.clear(); rows.clear();
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        auto cells = parseCsvLine(line);
        if (first) { header = cells; first = false; continue; }
        if (cells.empty() || trim(cells[0]).empty()) continue;
        rows.push_back(cells);
    }
    if (header.empty()) { err = "empty compbas: " + path; return false; }
    srcPath = path;
    idx.masterID     = colIndex(header, "component table");
    idx.type         = colIndex(header, "type");
    idx.name         = colIndex(header, "name");
    idx.slots        = colIndex(header, "crit hits");
    idx.recycle      = colIndex(header, "recycle");
    idx.heat         = colIndex(header, "heat");
    idx.tons         = colIndex(header, "weight");
    idx.damage       = colIndex(header, "damage");
    idx.br           = colIndex(header, "br");
    idx.rp           = colIndex(header, "rp");
    idx.range        = colIndex(header, "range");
    idx.missileType  = colIndex(header, "missile type");
    idx.fields       = colIndex(header, "fields");
    idx.fxid         = colIndex(header, "special fx id");
    idx.ammoMasterId = colIndex(header, "ammo master id");
    return true;
}

bool Compbas::write(const std::string& path, std::string& err) const {
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out.is_open()) { err = "cannot write " + path; return false; }
    out << emitCsvLine(header) << "\n";
    for (const auto& r : rows) out << emitCsvLine(r) << "\n";
    return true;
}

int Compbas::findByMasterId(const std::string& mid) const {
    for (size_t i = 0; i < rows.size(); ++i)
        if (!rows[i].empty() && trim(rows[i][0]) == trim(mid)) return (int)i;
    return -1;
}

std::string Compbas::cell(int row, int colIdx) const {
    if (row < 0 || row >= (int)rows.size() || colIdx < 0 || colIdx >= (int)rows[row].size())
        return "";
    return rows[row][colIdx];
}
void Compbas::setCell(int row, int colIdx, const std::string& v) {
    if (row < 0 || row >= (int)rows.size() || colIdx < 0) return;
    auto& r = rows[row];
    while ((int)r.size() <= colIdx) r.push_back("");
    r[colIdx] = v;
}

bool loadEffects(const std::string& path, std::vector<FxEntry>& out, std::string& err) {
    std::ifstream in(path.c_str());
    if (!in.is_open()) { err = "cannot open " + path; return false; }
    out.clear();
    std::string line;
    bool first = true;
    int id = 0;
    while (std::getline(in, line)) {
        if (first) { first = false; continue; }  // header
        auto c = parseCsvLine(line);
        if (c.empty()) continue;
        FxEntry e;
        e.id = id++;
        auto at = [&](size_t i) { return i < c.size() ? trim(c[i]) : std::string(); };
        e.name = at(1); e.muzzle = at(2); e.hit = at(3);
        e.objNum = at(4); e.miss = at(5); e.weaponName = at(6);
        out.push_back(e);
    }
    return true;
}

std::string validateCell(const std::string& kind, const std::string& value,
                         const std::vector<FxEntry>& fx) {
    std::string v = trim(value);
    if (kind == "ufloat" || kind == "uint") {
        char* end = nullptr;
        double n = std::strtod(v.c_str(), &end);
        if (end == v.c_str() || *end != '\0') return value + " is not numeric";
        if (n < 0) return "must be >= 0";
        if (kind == "uint" && (n != (double)(long)n || (long)n <= 0))
            return "must be a positive integer";
    } else if (kind == "int") {
        char* end = nullptr;
        std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str() || *end != '\0') return value + " is not an integer";
    } else if (kind == "range") {
        std::string lv = lower(v);
        if (lv != "short" && lv != "medium" && lv != "long" && lv != "0")
            return "range must be short/medium/long";
    } else if (kind == "wtype") {
        if (!isWeaponType(v)) return "not a weapon type";
    } else if (kind == "fxid") {
        char* end = nullptr;
        long id = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str() || *end != '\0') return "FX id not an integer";
        bool found = false;
        for (const auto& e : fx) if (e.id == (int)id) { found = true; break; }
        if (!found) return "FX id has no effects.csv row";
    } else if (kind == "str") {
        if (v.empty()) return "must be non-empty";
    }
    return "";
}

std::vector<std::string> validateRow(const Compbas& cb, int row,
                                     const std::vector<FxEntry>& fx) {
    std::vector<std::string> probs;
    auto chk = [&](int ci, const char* kind, const char* label) {
        if (ci < 0) return;
        std::string m = validateCell(kind, cb.cell(row, ci), fx);
        if (!m.empty()) probs.push_back(std::string(label) + ": " + m);
    };
    chk(cb.idx.type, "wtype", "type");
    chk(cb.idx.name, "str", "name");
    chk(cb.idx.damage, "ufloat", "damage");
    chk(cb.idx.heat, "ufloat", "heat");
    chk(cb.idx.recycle, "ufloat", "recycle");
    chk(cb.idx.tons, "ufloat", "tons");
    chk(cb.idx.slots, "uint", "slots");
    chk(cb.idx.range, "range", "range");
    chk(cb.idx.fxid, "fxid", "fxid");
    chk(cb.idx.missileType, "int", "missileType");
    chk(cb.idx.fields, "int", "fields");
    chk(cb.idx.ammoMasterId, "int", "ammoMasterId");
    return probs;
}

static bool makeDirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        cur += c;
        if (c == '/' || c == '\\') {
#ifdef _WIN32
            _mkdir(cur.c_str());
#else
            mkdir(cur.c_str(), 0755);
#endif
        }
    }
#ifdef _WIN32
    _mkdir(path.c_str());
#else
    mkdir(path.c_str(), 0755);
#endif
    return true;
}

static bool fileExists(const std::string& p) {
    struct stat st;
    return ::stat(p.c_str(), &st) == 0;
}

bool writeOverlay(const std::string& modRoot, const std::string& modId,
                  const Compbas& cb, std::string& outPath, std::string& err) {
    std::string moddir = modRoot + "/" + modId;
    std::string objdir = moddir + "/data/objects";
    makeDirs(objdir);
    std::string modjson = moddir + "/mod.json";
    if (!fileExists(modjson)) {
        std::ofstream mj(modjson.c_str());
        if (mj.is_open())
            mj << "{\n  \"schema\": \"mc2-mod/1\",\n  \"id\": \"" << modId
               << "\",\n  \"name\": \"" << modId
               << "\",\n  \"version\": \"1.0.0\",\n  \"dependencies\": []\n}\n";
    }
    outPath = objdir + "/compbas.csv";
    return cb.write(outPath, err);
}

}  // namespace mc2w
