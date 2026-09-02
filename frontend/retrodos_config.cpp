#include "retrodos_config.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <map>

namespace retrodos {
namespace {

std::string read_file(const std::string &path)
{
    size_t len = 0;
    void *data = SDL_LoadFile(path.c_str(), &len);
    if (!data) return std::string();
    std::string s((const char *)data, len);
    SDL_free(data);
    return s;
}

bool write_file(const std::string &path, const std::string &body)
{
    SDL_IOStream *io = SDL_IOFromFile(path.c_str(), "w");
    if (!io) return false;
    const bool ok = SDL_WriteIO(io, body.data(), body.size()) == body.size();
    SDL_CloseIO(io);
    return ok;
}

std::map<std::string, std::string> parse_kv(const std::string &text)
{
    std::map<std::string, std::string> kv;
    size_t i = 0;
    while (i < text.size()) {
        size_t e = text.find('\n', i);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(i, e - i);
        i = e + 1;
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

bool as_bool(const std::map<std::string, std::string> &kv, const char *k, bool dflt)
{
    auto it = kv.find(k);
    return it == kv.end() ? dflt : (it->second == "1" || it->second == "true");
}

int as_int(const std::map<std::string, std::string> &kv, const char *k, int dflt)
{
    auto it = kv.find(k);
    return it == kv.end() ? dflt : SDL_atoi(it->second.c_str());
}

std::string as_str(const std::map<std::string, std::string> &kv, const char *k,
                   const std::string &dflt)
{
    auto it = kv.find(k);
    return it == kv.end() ? dflt : it->second;
}

void append_settings(std::string &s, const Settings &v)
{
    s += "cycles_max=";     s += v.cycles_max ? "1" : "0";     s += "\n";
    s += "cycles_fixed=";   s += std::to_string(v.cycles_fixed); s += "\n";
    s += "core_dynamic=";   s += v.core_dynamic ? "1" : "0";   s += "\n";
    s += "memsize=";        s += std::to_string(v.memsize);    s += "\n";
    s += "sbtype=";         s += v.sbtype;                     s += "\n";
    s += "aspect_correct="; s += v.aspect_correct ? "1" : "0"; s += "\n";
    s += "integer_scale=";  s += v.integer_scale ? "1" : "0";  s += "\n";
}

void read_settings(const std::map<std::string, std::string> &kv, Settings &v)
{
    v.cycles_max     = as_bool(kv, "cycles_max", v.cycles_max);
    v.cycles_fixed   = as_int (kv, "cycles_fixed", v.cycles_fixed);
    v.core_dynamic   = as_bool(kv, "core_dynamic", v.core_dynamic);
    v.memsize        = as_int (kv, "memsize", v.memsize);
    v.sbtype         = as_str (kv, "sbtype", v.sbtype);
    v.aspect_correct = as_bool(kv, "aspect_correct", v.aspect_correct);
    v.integer_scale  = as_bool(kv, "integer_scale", v.integer_scale);
}

/* A game name becomes a filename, so it must not carry separators. */
std::string sanitise(const std::string &name)
{
    std::string out;
    for (char c : name) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out += ok ? c : '_';
    }
    if (out.size() > 100) out.resize(100);
    return out;
}

} /* namespace */

bool load_app_config(const std::string &path, AppConfig &out)
{
    const std::string text = read_file(path);
    if (text.empty()) return false;
    const auto kv = parse_kv(text);
    out.library_root = as_str(kv, "library_root", out.library_root);
    out.wizard_done  = as_bool(kv, "wizard_done", false);
    read_settings(kv, out.defaults);
    return true;
}

bool save_app_config(const std::string &path, const AppConfig &cfg)
{
    std::string s = "# Retro-DOS settings\n";
    s += "library_root=" + cfg.library_root + "\n";
    s += "wizard_done=";  s += cfg.wizard_done ? "1" : "0"; s += "\n";
    append_settings(s, cfg.defaults);
    return write_file(path, s);
}

bool load_game_settings(const std::string &dir, const std::string &game, Settings &out)
{
    const std::string text = read_file(dir + "/" + sanitise(game) + ".cfg");
    if (text.empty()) return false;
    read_settings(parse_kv(text), out);
    return true;
}

bool save_game_settings(const std::string &dir, const std::string &game, const Settings &s)
{
    SDL_CreateDirectory(dir.c_str());
    std::string body = "# per-game overrides for: " + game + "\n";
    append_settings(body, s);
    return write_file(dir + "/" + sanitise(game) + ".cfg", body);
}

std::string build_conf(const Settings &s, const std::string &title,
                       const std::string &mount_dir, const std::string &run_cmd)
{
    std::string c;

    c += "[sdl]\n";
    /* The engine renders OFFSCREEN and the frontend owns the window. Both
     * lines are required: output=gamelink without 'gamelink master=true'
     * silently falls back to output=surface, and then the engine renders into
     * a window nobody reads -- a black screen with no error anywhere. */
    c += "output=gamelink\n";
    c += "gamelink master=true\n";
    c += "autolock=false\n";
    c += "waitonerror=false\n";
    c += "showmenu=false\n";

    c += "[dosbox]\n";
    /* Not optional. With a non-TTY stdin DOSBox-X decides to prompt for a
     * working directory and blocks in a folder picker; on Android there is
     * neither stdin nor a dialog, so it hangs with no diagnostic at all. */
    c += "working directory option=noprompt\n";
    c += "memsize=" + std::to_string(s.memsize) + "\n";
    c += "title=" + title + "\n";

    c += "[cpu]\n";
    c += std::string("core=") + (s.core_dynamic ? "dynamic" : "normal") + "\n";
    if (s.cycles_max) c += "cycles=max\n";
    else              c += "cycles=fixed " + std::to_string(s.cycles_fixed) + "\n";

    c += "[sblaster]\n";
    c += "sbtype=" + s.sbtype + "\n";

    c += "[autoexec]\n";
    c += "mount C \"" + mount_dir + "\"\n";
    c += "C:\n";
    if (!run_cmd.empty()) c += "\"" + run_cmd + "\"\n";

    return c;
}

} /* namespace retrodos */
