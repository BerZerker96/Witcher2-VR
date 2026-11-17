#include "config.h"
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

static std::map<std::string, std::string> g_kv;
static std::mutex g_mx;

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

void ConfigLoad(const wchar_t* path) {
    std::lock_guard<std::mutex> l(g_mx);
    g_kv.clear();
    FILE* f = _wfopen(path, L"r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        size_t h = s.find('#'); if (h != std::string::npos) s = s.substr(0, h);
        size_t e = s.find('='); if (e == std::string::npos) continue;
        std::string k = Trim(s.substr(0, e)), v = Trim(s.substr(e + 1));
        if (!k.empty()) g_kv[k] = v;
    }
    fclose(f);
}

static bool Get(const char* key, std::string& out) {
    std::lock_guard<std::mutex> l(g_mx);
    auto it = g_kv.find(key);
    if (it == g_kv.end()) return false;
    out = it->second; return true;
}
int ConfigInt(const char* key, int def) { std::string v; return Get(key, v) ? atoi(v.c_str()) : def; }
float ConfigFloat(const char* key, float def) { std::string v; return Get(key, v) ? (float)atof(v.c_str()) : def; }
std::string ConfigStr(const char* key, const char* def) { std::string v; return Get(key, v) ? v : std::string(def); }
bool ConfigBool(const char* key, bool def) {
    std::string v; if (!Get(key, v)) return def;
    return v == "1" || v == "true" || v == "on" || v == "yes";
}
