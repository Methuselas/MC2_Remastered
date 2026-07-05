// gos_dev_shell.cpp - DEV-SHELL-1: localhost dev command socket.
// See gos_dev_shell.h for the contract. Failure modes are boring by design:
// any socket-layer failure logs once and the game continues; a malformed
// request gets an error reply; an oversized request drops the connection.

#include "gos_dev_shell.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "utils/shader_builder.h"
#include "utils/graphics.h"
#include "gos_screenshot.h"
#include "gos_postprocess.h"

namespace gos_dev_shell {

#ifndef _WIN32
// Non-Windows builds: inert stubs (winsock-only v0; revisit at the
// mac/linux port together with the rest of the platform layer).
bool pollCommands(uint32_t) { return false; }
void capturePendingScreenshot(uint32_t) {}
#else

namespace {

constexpr int kProtocolVersion = 1;
constexpr size_t kMaxRequestBytes = 16 * 1024;  // oversized request -> drop connection

// --- gate -------------------------------------------------------------
bool shellEnabled()
{
    static const bool s_on = [] {
        const char* v = getenv("MC2_DEV_SHELL");
        return v && atoi(v) != 0;
    }();
    return s_on;
}

// --- tiny flat-JSON helpers -------------------------------------------
// Requests are small flat objects from our own client; a full JSON parser
// is not warranted for v0. Extracts `"key" : "value"` / bare tokens.
std::string jsonGetString(const std::string& src, const char* key)
{
    const std::string pat = std::string("\"") + key + "\"";
    size_t p = src.find(pat);
    if (p == std::string::npos) return "";
    p = src.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) ++p;
    if (p >= src.size()) return "";
    if (src[p] == '"') {
        std::string out;
        for (size_t i = p + 1; i < src.size(); ++i) {
            if (src[i] == '\\' && i + 1 < src.size()) { out += src[++i]; continue; }
            if (src[i] == '"') return out;
            out += src[i];
        }
        return "";  // unterminated string
    }
    // bare token (true/false/number)
    size_t e = src.find_first_of(",}\n\r ", p);
    return src.substr(p, (e == std::string::npos ? src.size() : e) - p);
}

bool jsonGetBool(const std::string& src, const char* key, bool def)
{
    const std::string v = jsonGetString(src, key);
    if (v.empty()) return def;
    return v == "true" || v == "1";
}

std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) { /* drop control */ }
            else out += c;
        }
    }
    return out;
}

// Reply is ALWAYS {"ok":..,"error":..,"version":1,"data":{..}} — versioned
// from day one so future MCP/editor clients do not rot.
std::string makeReply(bool ok, const std::string& error, const std::string& dataJson)
{
    std::string r = "{\"ok\":";
    r += ok ? "true" : "false";
    r += ",\"error\":";
    if (error.empty()) r += "null";
    else { r += '"'; r += jsonEscape(error); r += '"'; }
    r += ",\"version\":";
    r += std::to_string(kProtocolVersion);
    r += ",\"data\":";
    r += dataJson.empty() ? "{}" : dataJson;
    r += "}\n";
    return r;
}

// --- socket state -------------------------------------------------------
SOCKET s_listen = INVALID_SOCKET;
SOCKET s_client = INVALID_SOCKET;
std::string s_recvBuf;
bool s_initTried = false;
bool s_initOk = false;

// pending screenshot (set by command, consumed at the post-render hook)
std::string s_pendingShotPath;
std::string s_lastShotResult;  // "" until first capture; then "wrote <path>" / error

void closeClient()
{
    if (s_client != INVALID_SOCKET) { closesocket(s_client); s_client = INVALID_SOCKET; }
    s_recvBuf.clear();
}

bool initSocket()
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[DEV_SHELL] WSAStartup failed (%d); shell disabled, game continues\n", WSAGetLastError());
        return false;
    }
    int port = 9877;
    if (const char* p = getenv("MC2_DEV_SHELL_PORT")) {
        const int v = atoi(p);
        if (v > 0 && v < 65536) port = v;
    }
    s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen == INVALID_SOCKET) {
        fprintf(stderr, "[DEV_SHELL] socket() failed (%d); shell disabled, game continues\n", WSAGetLastError());
        return false;
    }
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    // localhost-only bind: the shell must never be reachable off-machine.
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR
        || listen(s_listen, 1) == SOCKET_ERROR) {
        fprintf(stderr, "[DEV_SHELL] bind/listen 127.0.0.1:%d failed (%d); shell disabled, game continues\n",
                port, WSAGetLastError());
        closesocket(s_listen);
        s_listen = INVALID_SOCKET;
        return false;
    }
    u_long nonblock = 1;
    ioctlsocket(s_listen, FIONBIO, &nonblock);
    fprintf(stderr, "[DEV_SHELL v1] listening on 127.0.0.1:%d\n", port);
    fflush(stderr);
    return true;
}

// --- commands -----------------------------------------------------------

std::string cmdPing(uint32_t frame)
{
    return makeReply(true, "", "{\"frame\":" + std::to_string(frame) + "}");
}

// Iterate the global program registry. Default: reload only programs whose
// source timestamps changed (needsReload). params: {"force":true} reloads
// everything, {"name":"..."} restricts to one program. Full compile/link
// error text is returned in the reply; on failure the previous program stays
// live (stated explicitly — no silent stale state).
std::string cmdReloadShaders(const std::string& req)
{
    const bool force = jsonGetBool(req, "force", false);
    const std::string only = jsonGetString(req, "name");

    int reloaded = 0, failed = 0, skipped = 0;
    std::string results = "[";
    std::string firstError;
    for (auto& kv : glsl_program::s_programs) {
        glsl_program* prog = kv.second;
        if (!prog) continue;
        if (!only.empty() && kv.first != only) continue;
        const char* action;
        std::string err;
        if (force || prog->needsReload()) {
            if (prog->reload()) { action = "reloaded"; ++reloaded; }
            else {
                action = "failed_stale_program_preserved";
                err = prog->reload_log_;
                if (firstError.empty())
                    firstError = kv.first + ": " + (err.empty() ? "see console" : err);
                ++failed;
            }
        } else { action = "unchanged"; ++skipped; }
        if (results.size() > 1) results += ',';
        results += "{\"program\":\"" + jsonEscape(kv.first) + "\",\"action\":\"" + action + "\"";
        if (!err.empty()) results += ",\"error\":\"" + jsonEscape(err) + "\"";
        results += "}";
    }
    results += "]";
    std::string data = "{\"reloaded\":" + std::to_string(reloaded)
        + ",\"failed\":" + std::to_string(failed)
        + ",\"unchanged\":" + std::to_string(skipped)
        + ",\"programs\":" + results + "}";
    return makeReply(failed == 0, firstError, data);
}

// Hard path safety: caller supplies a NAME, never a path. Anything that
// looks like traversal or a separator is stripped to a flat basename and
// the file always lands under dev_shell_out/ in the working directory.
std::string sanitizeShotName(std::string name, uint32_t frame)
{
    std::string base;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (ok) base += c;
    }
    if (base.empty()) base = "shot_frame" + std::to_string(frame);
    return "dev_shell_out/" + base + ".tga";
}

std::string cmdScreenshot(const std::string& req, uint32_t frame)
{
    if (!s_pendingShotPath.empty())
        return makeReply(false, "screenshot already pending: " + s_pendingShotPath, "");
    CreateDirectoryA("dev_shell_out", nullptr);  // ok if it already exists
    s_pendingShotPath = sanitizeShotName(jsonGetString(req, "name"), frame);
    return makeReply(true, "",
        "{\"scheduled\":true,\"path\":\"" + jsonEscape(s_pendingShotPath)
        + "\",\"note\":\"captured at end of current frame; poll last_screenshot to confirm\"}");
}

std::string cmdLastScreenshot()
{
    return makeReply(true, "",
        "{\"result\":\"" + jsonEscape(s_lastShotResult.empty() ? "none" : s_lastShotResult) + "\"}");
}

std::string cmdGetGate(const std::string& req)
{
    const std::string name = jsonGetString(req, "name");
    if (name.empty()) return makeReply(false, "get_gate: missing name", "");
    const char* v = getenv(name.c_str());
    return makeReply(true, "",
        "{\"name\":\"" + jsonEscape(name) + "\",\"value\":"
        + (v ? "\"" + jsonEscape(v) + "\"" : "null") + "}");
}

// Honesty contract: this updates the PROCESS environment. Gates that are
// read every frame pick it up on their next check; gates cached once at
// init (`static const` reads — the majority) need a restart. We cannot
// distinguish the two generically here, so the reply says exactly that
// instead of pretending the set took effect.
std::string cmdSetGate(const std::string& req)
{
    const std::string name = jsonGetString(req, "name");
    const std::string value = jsonGetString(req, "value");
    if (name.empty()) return makeReply(false, "set_gate: missing name", "");
    if (name.compare(0, 4, "MC2_") != 0)
        return makeReply(false, "set_gate: only MC2_* gates are settable", "");
    if (_putenv_s(name.c_str(), value.c_str()) != 0)
        return makeReply(false, "set_gate: _putenv_s failed", "");
    return makeReply(true, "",
        "{\"name\":\"" + jsonEscape(name) + "\",\"value\":\"" + jsonEscape(value)
        + "\",\"applied\":\"process_env_only\",\"note\":\"live-read gates apply on next check;"
          " init-read gates (static const reads) require restart\"}");
}

// Dispatch one complete request line -> reply string. `quitOut` set on "quit".
std::string dispatch(const std::string& req, uint32_t frame, bool* quitOut)
{
    const std::string type = jsonGetString(req, "type");
    if (type == "ping")            return cmdPing(frame);
    if (type == "reload_shaders")  return cmdReloadShaders(req);
    if (type == "screenshot")      return cmdScreenshot(req, frame);
    if (type == "last_screenshot") return cmdLastScreenshot();
    if (type == "get_gate")        return cmdGetGate(req);
    if (type == "set_gate")        return cmdSetGate(req);
    if (type == "quit") { *quitOut = true; return makeReply(true, "", "{\"quitting\":true}"); }
    if (type.empty())              return makeReply(false, "malformed request: no type", "");
    return makeReply(false, "unknown command: " + type, "");
}

}  // namespace

bool pollCommands(uint32_t frame)
{
    if (!shellEnabled()) return false;
    if (!s_initTried) { s_initTried = true; s_initOk = initSocket(); }
    if (!s_initOk) return false;

    // accept (single client at a time; a new connection replaces a dead one)
    if (s_client == INVALID_SOCKET) {
        SOCKET c = accept(s_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) return false;  // WSAEWOULDBLOCK = nobody there
        u_long nonblock = 1;
        ioctlsocket(c, FIONBIO, &nonblock);
        s_client = c;
    }

    // drain what's available; execute every complete line
    bool quit = false;
    char buf[4096];
    for (;;) {
        const int n = recv(s_client, buf, sizeof(buf), 0);
        if (n > 0) {
            s_recvBuf.append(buf, static_cast<size_t>(n));
            if (s_recvBuf.size() > kMaxRequestBytes) {
                fprintf(stderr, "[DEV_SHELL] oversized request (>%zu bytes); dropping connection\n", kMaxRequestBytes);
                closeClient();
                return false;
            }
            continue;
        }
        if (n == 0) { closeClient(); return false; }  // peer closed
        break;  // n < 0: WSAEWOULDBLOCK (no more data) or error — both stop reading
    }

    size_t nl;
    while (s_client != INVALID_SOCKET && (nl = s_recvBuf.find('\n')) != std::string::npos) {
        const std::string line = s_recvBuf.substr(0, nl);
        s_recvBuf.erase(0, nl + 1);
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        const std::string reply = dispatch(line, frame, &quit);
        if (send(s_client, reply.c_str(), static_cast<int>(reply.size()), 0) == SOCKET_ERROR)
            closeClient();
    }
    return quit;
}

void capturePendingScreenshot(uint32_t frame)
{
    if (!shellEnabled() || s_pendingShotPath.empty()) return;
    const std::string path = s_pendingShotPath;
    s_pendingShotPath.clear();

    // Same source as [SCREENSHOT v1]: the offscreen post-process scene FBO,
    // not backbuffer FBO 0 (which reads black when the window is minimized).
    gosPostProcess* pp = getGosPostProcess();
    const GLuint fbo = pp ? pp->getSceneFBO() : 0;
    const int w = pp ? pp->getWidth() : 0;
    const int h = pp ? pp->getHeight() : 0;
    if (!fbo || w <= 0 || h <= 0) {
        s_lastShotResult = "failed: scene FBO unavailable";
        return;
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    const bool ok = gos::screenshot::writeTGA(path.c_str(), w, h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    s_lastShotResult = ok
        ? "wrote " + path + " (" + std::to_string(w) + "x" + std::to_string(h) + ", frame " + std::to_string(frame) + ")"
        : "failed: writeTGA " + path;
    fprintf(stderr, "[DEV_SHELL] screenshot: %s\n", s_lastShotResult.c_str());
}

#endif  // _WIN32

}  // namespace gos_dev_shell
