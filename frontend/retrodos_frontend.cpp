/*
 * retro-dosbox frontend — SDL3 + Dear ImGui.
 *
 * This owns the window. The engine runs HEADLESS behind it through the Game
 * Link output and hands us finished frames via the retrodos_host framebuffer
 * tap. That arrangement buys three things at once:
 *
 *   - DOSBox-X's own menu bar and config GUI never appear, because there is no
 *     DOSBox-X window for them to be drawn into.
 *   - The DOS picture is SCALED. With --disable-opengl the engine's only
 *     on-screen backend is `surface`, which blits 1:1 and leaves a 720x400
 *     picture in the corner of a 1920x1080 handheld.
 *   - There is somewhere to put a wizard, a library and a keyboard, which an
 *     emulator that boots straight into DOS has nowhere to host.
 *
 * Two threads: the engine thread runs the blocking DOSBox-X mainloop; this one
 * does SDL, ImGui and the frame blit. Everything sent to the engine goes
 * through retrodos_host_*, which queues rather than touching emulator state.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "retrodos_host.h"
#include "retrodos_config.h"
#include "retrodos_osk.h"
#include "retrodos_saf.h"
#include "retrodos_demo.h"
#include "retrodos_media.h"

#include <algorithm>
#include <map>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
#include <dirent.h>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "retrodos", __VA_ARGS__)
#else
#define LOGI(...) do { SDL_Log(__VA_ARGS__); } while (0)
#endif

using retrodos::AppConfig;
using retrodos::Settings;

namespace {

/* ------------------------------------------------------------------ */
/* Library                                                             */
/* ------------------------------------------------------------------ */

struct Game {
    std::string name;
    std::string dir;          /* real path; empty for a SAF game until staged */
    std::string run;          /* DOS command, may be empty */
    char        initial = '#';
    bool        from_saf = false;
    bool        run_raw  = false;  /* emit `run` unquoted (a DOSBox-X command) */
    bool        is_demo  = false;  /* bundled content: never staged, never scanned */
    std::string slug;              /* RetroMedia catalogue slug, when matched */
};

bool ends_with_ci(const std::string &s, const char *suffix)
{
    const size_t n = SDL_strlen(suffix);
    if (s.size() < n) return false;
    return SDL_strncasecmp(s.c_str() + s.size() - n, suffix, n) == 0;
}

/* Reduce a title to something two sources can agree on.
 *
 * A folder on the card is called "DOOM2 (1994) [v1.9]" and the catalogue calls
 * it "Doom II (1994)". Nothing matches on raw strings, so both sides are
 * lowercased, stripped of bracketed qualifiers, and reduced to letters and
 * digits before they are compared. */
std::string canon(const std::string &s)
{
    std::string out;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth) --depth; continue; }
        if (depth) continue;
        /* Drop an archive extension: the folder and the .zip of the same game
         * must reduce to the same key. */
        if (c == '.' && (ends_with_ci(s, ".zip") || ends_with_ci(s, ".exe")) &&
            i + 4 == s.size())
            break;
        if (SDL_isalnum((unsigned char)c))
            out += (char)SDL_tolower((unsigned char)c);
    }
    /* "The Secret of Monkey Island" vs "Secret of Monkey Island, The". */
    if (out.compare(0, 3, "the") == 0 && out.size() > 3) out = out.substr(3);
    return out;
}

/* .BAT first: installers habitually leave a one-line batch file that sets up
 * the environment the .EXE expects, and running the .EXE directly then fails
 * in ways that look like emulation bugs. */
void find_runnable(const std::string &dir, Game &g)
{
    int n = 0;
    char **files = SDL_GlobDirectory(dir.c_str(), NULL, SDL_GLOB_CASEINSENSITIVE, &n);
    if (!files) return;
    std::string bat, com, exe;
    for (int i = 0; i < n; ++i) {
        const std::string f = files[i];
        if      (bat.empty() && ends_with_ci(f, ".bat")) bat = f;
        else if (com.empty() && ends_with_ci(f, ".com")) com = f;
        else if (exe.empty() && ends_with_ci(f, ".exe")) exe = f;
    }
    SDL_free(files);
    g.run = !bat.empty() ? bat : (!com.empty() ? com : exe);
}

std::vector<Game> scan_library(const std::string &root)
{
    std::vector<Game> games;
    if (root.empty()) return games;

    int n = 0;
    char **entries = SDL_GlobDirectory(root.c_str(), NULL, 0, &n);
    if (!entries) return games;

    for (int i = 0; i < n; ++i) {
        const std::string name = entries[i];
        if (name == "." || name == "..") continue;
        /* Skip dotfiles and the stray '~' trees DOSBox-X leaves behind when
         * handed a HOME it cannot resolve -- they are not games. */
        if (name[0] == '.' || name[0] == '~') continue;

        const std::string full = root + "/" + name;
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(full.c_str(), &info)) continue;
        if (info.type != SDL_PATHTYPE_DIRECTORY) continue;

        Game g;
        g.name = name;
        g.dir  = full;
        find_runnable(full, g);
        const char c = (char)SDL_toupper((unsigned char)name[0]);
        g.initial = (c >= 'A' && c <= 'Z') ? c : '#';
        games.push_back(g);
    }
    SDL_free(entries);

    std::sort(games.begin(), games.end(), [](const Game &a, const Game &b) {
        return SDL_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return games;
}

/* Games that live in a SAF-granted tree. Only names are known here: the
 * contents are not reachable by path until the title is staged, so the
 * runnable is discovered after staging rather than now. */
std::vector<Game> scan_saf_library()
{
    std::vector<Game> games;
    for (const std::string &name : retrodos::saf_list_games()) {
        if (name.empty() || name[0] == '.') continue;
        Game g;
        g.name     = name;
        g.from_saf = true;
        const char c = (char)SDL_toupper((unsigned char)name[0]);
        g.initial = (c >= 'A' && c <= 'Z') ? c : '#';
        games.push_back(g);
    }
    return games;
}

/* ------------------------------------------------------------------ */
/* Candidate library roots                                             */
/* ------------------------------------------------------------------ */

/* App-specific directories on EVERY volume need no permission and are real
 * filesystem paths -- which is what matters, because DOSBox-X mounts a
 * directory BY PATH and cannot be handed a SAF content:// URI. Anything
 * outside these needs a granted document tree, and then the game has to be
 * staged into one of these before it can be mounted at all. */
std::vector<std::string> candidate_roots()
{
    std::vector<std::string> out;
#if defined(__ANDROID__)
    if (const char *ext = SDL_GetAndroidExternalStoragePath())
        out.push_back(std::string(ext) + "/dos");

    /* The same app-private directory on removable storage: where a large
     * collection realistically lives on a handheld. */
    /* readdir, NOT SDL_GlobDirectory: the latter stats every entry, and
     * /storage/self is a self-referential symlink that makes it hang outright
     * -- the app starts, SDL comes up, and the first frame never arrives.
     * readdir only needs the names, which is all this wants. */
    if (DIR *d = opendir("/storage")) {
        while (struct dirent *e = readdir(d)) {
            const std::string v = e->d_name;
            if (v == "emulated" || v == "self" || v == "." || v == "..") continue;
            out.push_back("/storage/" + v +
                          "/Android/data/com.crownparkcomputing.retrodos/files/dos");
        }
        closedir(d);
    }
#else
    if (char *pref = SDL_GetPrefPath("CrownParkComputing", "Retro-DOS")) {
        out.push_back(std::string(pref) + "dos");
        SDL_free(pref);
    }
#endif
    return out;
}

/* What the user should see as "the library". Once a folder is granted, the
 * app-private path is only a staging area and showing it is actively
 * misleading -- it is not where their games are. */
std::string library_label(const std::string &root)
{
    const std::string uri = retrodos::saf_tree_uri();
    if (uri.empty()) return root;

    /* content://...tree/FEDD-B1FF%3ADOS%20Games...%2FGames -- decode enough of
     * the tail to be recognisable rather than showing a raw URI. */
    std::string t = uri.substr(uri.find_last_of('/') + 1);
    std::string out;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '%' && i + 2 < t.size()) {
            const int hi = SDL_isdigit(t[i+1]) ? t[i+1]-'0' : (SDL_toupper(t[i+1])-'A'+10);
            const int lo = SDL_isdigit(t[i+2]) ? t[i+2]-'0' : (SDL_toupper(t[i+2])-'A'+10);
            const char c = (char)(hi * 16 + lo);
            out += (c == ':') ? '/' : c;
            i += 2;
        } else out += t[i];
    }
    return out;
}

bool path_is_dir(const std::string &p)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(p.c_str(), &info) && info.type == SDL_PATHTYPE_DIRECTORY;
}

/* ------------------------------------------------------------------ */
/* Engine thread                                                       */
/* ------------------------------------------------------------------ */

std::atomic<bool> g_engine_done{false};

void engine_thread(std::string conf, std::string workdir)
{
    /* Deliberately NOT lending the engine our window: a borrowed window is one
     * it will reconfigure, present to and destroy, and any of those silently
     * kills the host's renderer. */
    retrodos_host_set_window(nullptr);

    char a0[] = "dosbox-x";
    char a1[] = "-conf";
    char a3[] = "-defaultdir";
    std::vector<char> c(conf.begin(), conf.end());       c.push_back('\0');
    std::vector<char> d(workdir.begin(), workdir.end()); d.push_back('\0');
    char *argv[] = { a0, a1, c.data(), a3, d.data(), nullptr };

    retrodos_host_set_framebuffer_output(true);
    retrodos_host_run(5, argv);
    g_engine_done.store(true);
}

/* ------------------------------------------------------------------ */
/* Presentation                                                        */
/* ------------------------------------------------------------------ */

SDL_FRect fit(int fb_w, int fb_h, int aspect_x1000, int win_w, int win_h,
              const Settings &s)
{
    /* DOS modes are frequently non-square-pixel: 320x200 is a 4:3 picture, not
     * 16:10. Ignoring the ratio the engine reports is the classic
     * DOS-emulator tell. */
    double ratio = (fb_h > 0) ? ((double)fb_w / (double)fb_h) : (4.0 / 3.0);
    if (s.aspect_correct && aspect_x1000 > 0) ratio = aspect_x1000 / 1000.0;
    if (ratio <= 0.0) ratio = 4.0 / 3.0;

    double w = (double)win_w, h = w / ratio;
    if (h > (double)win_h) { h = (double)win_h; w = h * ratio; }

    if (s.integer_scale && fb_w > 0 && fb_h > 0) {
        const int k = std::max(1, std::min(win_w / fb_w, win_h / fb_h));
        w = (double)(fb_w * k);
        h = (double)(fb_h * k);
    }

    SDL_FRect r;
    r.w = (float)w; r.h = (float)h;
    r.x = (float)((win_w - w) * 0.5);
    r.y = (float)((win_h - h) * 0.5);
    return r;
}

/* ------------------------------------------------------------------ */
/* Settings UI (shared by global defaults and per-game overrides)      */
/* ------------------------------------------------------------------ */

void settings_widgets(Settings &s)
{
    ImGui::TextUnformatted("CPU");
    ImGui::Checkbox("Dynamic core (faster; turn off if a game misbehaves)", &s.core_dynamic);
    ImGui::Checkbox("Cycles: max", &s.cycles_max);
    if (!s.cycles_max) {
        ImGui::SliderInt("Fixed cycles", &s.cycles_fixed, 300, 100000);
        ImGui::TextDisabled("Early titles busy-wait for timing and run absurdly\n"
                            "fast on 'max'. A fixed count is what fixes them.");
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextUnformatted("Memory");
    ImGui::SliderInt("Memory (MB)", &s.memsize, 1, 64);
    ImGui::TextDisabled("32 MB is the safe default: DOS/4GW 1.97 miscalculates with\n"
                        "more, and several early-90s titles then refuse to start.");

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextUnformatted("Sound");
    static const char *sb[] = { "sbpro2", "sb16", "sbpro1", "sb2", "sb1", "none" };
    for (int i = 0; i < (int)SDL_arraysize(sb); ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(sb[i], s.sbtype == sb[i])) s.sbtype = sb[i];
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextUnformatted("Display");
    ImGui::Checkbox("Correct aspect ratio", &s.aspect_correct);
    ImGui::Checkbox("Integer scaling (sharper, bigger borders)", &s.integer_scale);
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

#if defined(__ANDROID__)
    /* The engine narrates its startup on stdout, which on Android goes
     * nowhere -- a boot that fails early is otherwise completely silent.
     * Unbuffered, because a buffered log is lost exactly when it is the only
     * evidence left. */
    if (const char *ext = SDL_GetAndroidExternalStoragePath()) {
        char p[1024];
        SDL_snprintf(p, sizeof(p), "%s/retrodos-stdout.log", ext);
        if (freopen(p, "w", stdout)) setvbuf(stdout, NULL, _IONBF, 0);
        if (freopen(p, "a", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    }
#endif

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        LOGI("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    SDL_Window   *win = nullptr;
    SDL_Renderer *ren = nullptr;
    if (!SDL_CreateWindowAndRenderer("Retro-DOS", 1280, 720,
                                     SDL_WINDOW_FULLSCREEN | SDL_WINDOW_RESIZABLE,
                                     &win, &ren)) {
        LOGI("window/renderer failed: %s", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(ren, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   /* our own config owns durable state */
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    {   /* A handheld is held at arm's length, so the whole UI is scaled rather
         * than shipping one designed for a mouse on a desk.
         *
         * The font is RASTERISED at the target size instead of being stretched
         * with FontGlobalScale: that only magnifies the built-in 13px bitmap,
         * which stays soft and reads small however far it is pushed. Asking
         * for the size up front gives crisp glyphs. */
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(win, &ww, &wh);
        const float scale = std::max(1.0f, (float)wh / 540.0f);
        ImGui::GetStyle().ScaleAllSizes(scale);

        ImFontConfig fc;
        fc.SizePixels = std::max(20.0f, (float)wh / 26.0f);   /* ~41px at 1080p */
        ImGui::GetIO().Fonts->Clear();
        ImGui::GetIO().Fonts->AddFontDefault(&fc);
        ImGui::GetIO().Fonts->Build();
        /* The backend rebuilds its font texture lazily on the next frame; in
         * this ImGui version the explicit texture calls are not public. */
        ImGui_ImplSDLRenderer3_DestroyDeviceObjects();
    }

    /* Config lives in internal storage: it is ours, small, and must survive
     * the SD card being absent. */
    std::string cfg_dir;
#if defined(__ANDROID__)
    if (const char *in = SDL_GetAndroidInternalStoragePath()) cfg_dir = in;
#else
    if (char *pref = SDL_GetPrefPath("CrownParkComputing", "Retro-DOS")) {
        cfg_dir = pref; SDL_free(pref);
    }
#endif
    const std::string cfg_path  = cfg_dir + "/retrodos.cfg";
    const std::string games_dir = cfg_dir + "/games";
    const std::string conf_path = cfg_dir + "/dosbox-x.conf";

    AppConfig cfg;
    retrodos::load_app_config(cfg_path, cfg);

    std::vector<std::string> roots = candidate_roots();
    for (const auto &r : roots) SDL_CreateDirectory(r.c_str());
    if (cfg.library_root.empty() && !roots.empty()) cfg.library_root = roots.front();

    /* Staged SAF games land here: inside the library root, hidden from the
     * scanner by the leading dot so a staged copy never shows up as a second
     * entry in the list. */
    const std::string stage_dir = cfg.library_root + "/.staged";

    /* Bundled content, unpacked out of the package to a real path because
     * DOSBox-X mounts a directory and cannot read from an APK or app bundle. */
    const std::string demo_dir = cfg_dir + "/demo";

    std::vector<Game> games;
    auto refresh = [&]() {
        /* A granted SAF tree replaces the folder scan: it is what the user
         * actually chose, and the app-private folder is then only a staging
         * area. */
        games = retrodos::saf_has_grant() ? scan_saf_library()
                                          : scan_library(cfg.library_root);

        /* Nothing found -- a fresh install, a revoked grant, or an absent SD
         * card. Offer the bundled content rather than an empty list, which
         * reads as a broken app and, for a store reviewer, IS one: there would
         * otherwise be nothing to press. Only extract when it is needed, so a
         * device with a real library never pays for it. */
        if (games.empty() && retrodos::demo_prepare(demo_dir)) {
            const retrodos::DemoKind kinds[] = { retrodos::DemoKind::Demo,
                                                 retrodos::DemoKind::FreeDos };
            for (retrodos::DemoKind k : kinds) {
                Game g;
                g.name    = retrodos::demo_title(k);
                g.dir     = demo_dir;
                g.run     = retrodos::demo_command(k, g.run_raw);
                g.is_demo = true;
                g.initial = (char)SDL_toupper((unsigned char)g.name[0]);
                games.push_back(g);
            }
        }
    };
    refresh();

    enum class View { Wizard, Library, Settings, Media, Emulator };
    View view = cfg.wizard_done ? View::Library : View::Wizard;

    std::thread engine;
    SDL_Texture *fb_tex = nullptr;
    int fb_tex_w = 0, fb_tex_h = 0;
    uint64_t last_serial = 0;
    std::vector<uint32_t> fb_copy;

    char  search[128] = {0};
    char  filter_letter = 0;      /* 0 = no A-Z filter */

    /* ---- RetroMedia ---- */
    retrodos::MediaAccount account;
    std::string  media_msg;
    bool         media_busy = false;
    std::vector<retrodos::MediaGame> catalogue;   /* admin download browser */
    std::map<std::string, SDL_Texture *> art;     /* game name -> box art */
    std::vector<std::string> art_queue;           /* slugs still to fetch */
    std::map<std::string, std::string> art_owner; /* slug -> local game name */
    int  art_done = 0, art_total = 0;
    bool art_mode = false;        /* this catalogue fetch is for artwork */
    char m_email[128] = {0}, m_pass[128] = {0}, m_key[160] = {0};
    char cat_search[128] = {0};
    char cat_letter = 0;

    SDL_strlcpy(m_email, retrodos::media_last_email().c_str(), sizeof(m_email));
    if (retrodos::media_available()) retrodos::media_begin_status();

    /* Turn a cached RGBA file into a texture. Uploading happens here, on the
     * render thread, because that is the only thread allowed to touch the
     * renderer -- the bridge only ever hands back a path. */
    auto load_art = [&](const std::string &name, const std::string &path) {
        int w = 0, h = 0;
        std::vector<unsigned char> rgba;
        if (!retrodos::media_read_art(path, w, h, rgba)) return;
        SDL_Texture *t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC, w, h);
        if (!t) return;
        SDL_UpdateTexture(t, nullptr, rgba.data(), w * 4);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        auto it = art.find(name);
        if (it != art.end() && it->second) SDL_DestroyTexture(it->second);
        art[name] = t;
    };
    int   selected = -1;          /* game index for per-game settings */
    Settings active = cfg.defaults;
    bool  show_overlay = false, show_osk = false;
    bool  running = true;

    auto launch = [&](const Game &in) {
        Game g = in;

        /* A SAF game has no path yet. Copy it into our own directory, which IS
         * a real path -- DOSBox-X mounts a directory by path and cannot be
         * given a content:// URI. Only the title being launched is copied, and
         * only the first time. */
        if (g.from_saf && !g.is_demo) {
            SDL_CreateDirectory(stage_dir.c_str());
            const std::string dest = stage_dir + "/" + g.name;
            if (!retrodos::saf_stage_game(g.name, dest)) {
                LOGI("stage failed for %s", g.name.c_str());
                return;
            }
            g.dir = dest;
            find_runnable(dest, g);   /* only now can we look inside */
        }

        Settings s = cfg.defaults;
        retrodos::load_game_settings(games_dir, g.name, s);  /* overrides win */
        active = s;
        const std::string conf = retrodos::build_conf(s, g.name, g.dir, g.run,
                                                      g.run_raw);
        if (SDL_IOStream *io = SDL_IOFromFile(conf_path.c_str(), "w")) {
            SDL_WriteIO(io, conf.data(), conf.size());
            SDL_CloseIO(io);
        }
        last_serial  = 0;
        show_overlay = show_osk = false;
        engine = std::thread(engine_thread, conf_path, g.dir);
        view = View::Emulator;
    };

    while (running) {
        /* True when the UI is covering the game and DOS should see nothing. */
        const bool ui_modal = (view != View::Emulator) || show_overlay || show_osk;

        /* ImGui is fed events even while the game is in front, because the
         * emulator view carries a small always-visible control strip. Without
         * this it could be drawn but never clicked, which is how the on-screen
         * keyboard ended up reachable only by a key no handheld has.
         *
         * What stops the game from losing its input is WantCaptureMouse: it is
         * only true while the pointer is actually over one of those controls. */
        const ImGuiIO &io = ImGui::GetIO();
        const bool ui_wants_mouse = ui_modal || io.WantCaptureMouse;
        const bool ui_wants_keys  = ui_modal || io.WantCaptureKeyboard;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            ImGui_ImplSDL3_ProcessEvent(&ev);

            switch (ev.type) {
            case SDL_EVENT_QUIT: running = false; break;

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                if (view != View::Emulator) break;
                const bool down = (ev.type == SDL_EVENT_KEY_DOWN);
                /* Back/Escape always reaches the overlay, never DOS: on a
                 * handheld with no keyboard it is the only way out. */
                if (ev.key.scancode == SDL_SCANCODE_AC_BACK ||
                    ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                    if (down) show_overlay = !show_overlay;
                    break;
                }
                if (!ui_wants_keys) retrodos_host_send_key((int)ev.key.scancode, down);
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                if (!ui_wants_mouse && view == View::Emulator)
                    retrodos_host_mouse_move((int)ev.motion.xrel, (int)ev.motion.yrel);
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (!ui_wants_mouse && view == View::Emulator) {
                    const int b = (ev.button.button == SDL_BUTTON_RIGHT)  ? 1 :
                                  (ev.button.button == SDL_BUTTON_MIDDLE) ? 2 : 0;
                    retrodos_host_mouse_button(b, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                }
                break;

            default: break;
            }
        }

        /* Drain finished network work. Everything the bridge does is async, so
         * this is where results become UI state -- never on the caller's side
         * of a begin_*(), which returns long before the server answers. */
        {
            retrodos::MediaResult mr;
            while (retrodos::media_poll(mr)) {
                if (mr.op != retrodos::MediaOp::Artwork) media_busy = false;
                if (!mr.message.empty()) media_msg = mr.message;

                switch (mr.op) {
                case retrodos::MediaOp::Status:
                case retrodos::MediaOp::Login:
                    account = mr.account;
                    if (mr.ok && account.signed_in) {
                        /* The password has served its purpose; the session is
                         * what persists. Do not leave it sitting in a buffer. */
                        SDL_memset(m_pass, 0, sizeof(m_pass));
                        SDL_memset(m_key,  0, sizeof(m_key));
                        if (media_msg.empty()) media_msg = "Signed in as " + account.email;
                    }
                    break;

                case retrodos::MediaOp::Logout:
                    account = retrodos::MediaAccount();
                    catalogue.clear();
                    break;

                case retrodos::MediaOp::Catalogue:
                    if (!mr.ok) break;
                    catalogue = mr.games;
                    if (art_mode) {
                        art_mode = false;
                        /* This catalogue was fetched to find artwork, not to
                         * browse: match it against the library and queue only
                         * the titles we actually have. */
                        std::map<std::string, const retrodos::MediaGame *> by_key;
                        for (const auto &cg : mr.games) by_key[canon(cg.title)] = &cg;
                        art_queue.clear();
                        art_owner.clear();
                        for (auto &g : games) {
                            if (g.is_demo) continue;
                            auto it = by_key.find(canon(g.name));
                            if (it == by_key.end() || it->second->preview.empty()) continue;
                            g.slug = it->second->slug;
                            art_owner[g.slug] = g.name;
                            art_queue.push_back(g.slug);
                        }
                        art_total = (int)art_queue.size();
                        art_done  = 0;
                        media_msg = art_total ? ("Matched " + std::to_string(art_total) +
                                                 " of " + std::to_string(games.size()))
                                              : "No titles matched the catalogue";
                        /* Kick the first fetch; the rest follow one at a time as
                         * each result lands, so a big library does not open
                         * hundreds of sockets at once. */
                        if (!art_queue.empty()) {
                            const std::string slug = art_queue.back();
                            art_queue.pop_back();
                            for (const auto &cg : mr.games)
                                if (cg.slug == slug) {
                                    retrodos::media_begin_artwork(slug, cg.preview);
                                    break;
                                }
                        }
                    }
                    break;

                case retrodos::MediaOp::Artwork:
                    if (mr.ok && !mr.art.path.empty()) {
                        auto own = art_owner.find(mr.art.slug);
                        if (own != art_owner.end()) load_art(own->second, mr.art.path);
                    }
                    ++art_done;
                    if (!art_queue.empty()) {
                        const std::string slug = art_queue.back();
                        art_queue.pop_back();
                        for (const auto &cg : catalogue)
                            if (cg.slug == slug) {
                                retrodos::media_begin_artwork(slug, cg.preview);
                                break;
                            }
                    } else {
                        media_busy = false;
                        if (art_total) media_msg = "Artwork: " + std::to_string(art_done) +
                                                   " of " + std::to_string(art_total);
                        art_total = 0;
                    }
                    break;

                case retrodos::MediaOp::Download:
                    /* The game is on disk now, so the library is stale. */
                    if (mr.ok) refresh();
                    break;

                default: break;
                }
            }
        }

        int win_w = 0, win_h = 0;
        SDL_GetWindowSizeInPixels(win, &win_w, &win_h);

        /* ImGui's SDL_Renderer backend sets a clip rect per draw command;
         * reset the render state so each frame starts from a known one. */
        SDL_SetRenderClipRect(ren, NULL);
        SDL_SetRenderViewport(ren, NULL);
        SDL_SetRenderDrawColor(ren, 12, 12, 16, 255);
        SDL_RenderClear(ren);

        if (view == View::Emulator) {
            /* Poll the serial, not the pixels: DOS text mode can sit on an
             * identical picture for seconds, and re-uploading it every frame
             * would burn a handheld's battery for nothing. */
            const uint64_t serial = retrodos_host_framebuffer_serial();
            if (serial != last_serial) {
                int w = 0, h = 0;
                retrodos_host_framebuffer_size(&w, &h);
                if (w > 0 && h > 0) {
                    fb_copy.resize((size_t)w * (size_t)h);
                    uint64_t got = 0;
                    if (retrodos_host_copy_framebuffer(fb_copy.data(),
                                                       (int)fb_copy.size(),
                                                       &w, &h, &got)) {
                        if (!fb_tex || fb_tex_w != w || fb_tex_h != h) {
                            if (fb_tex) SDL_DestroyTexture(fb_tex);
                            /* XRGB, not ARGB: the engine's framebuffer carries
                             * no meaningful alpha, so treating the top byte as
                             * one draws a fully transparent picture -- the draw
                             * still succeeds and the screen stays black. */
                            fb_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888,
                                                       SDL_TEXTUREACCESS_STREAMING, w, h);
                            SDL_SetTextureScaleMode(fb_tex, SDL_SCALEMODE_NEAREST);
                            SDL_SetTextureBlendMode(fb_tex, SDL_BLENDMODE_NONE);
                            fb_tex_w = w; fb_tex_h = h;
                        }
                        SDL_UpdateTexture(fb_tex, nullptr, fb_copy.data(), w * 4);
                        last_serial = serial;
                    }
                }
            }

            if (fb_tex) {
                const SDL_FRect dst = fit(fb_tex_w, fb_tex_h,
                                          retrodos_host_pixel_aspect_x1000(),
                                          win_w, win_h, active);
                SDL_RenderTexture(ren, fb_tex, nullptr, &dst);
            }

            if (g_engine_done.load()) {
                if (engine.joinable()) engine.join();
                g_engine_done.store(false);
                view = View::Library;
                show_overlay = show_osk = false;
                refresh();
            }
        }

        /* The frame is built every time, not only when the UI is in front: the
         * emulator view carries a small control strip, and a widget that is not
         * submitted cannot be hovered, clicked, or reported by
         * WantCaptureMouse. */
        {
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            const ImVec2 full((float)win_w, (float)win_h);

            /* ---------------- Wizard ---------------- */
            if (view == View::Wizard) {
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(full);
                ImGui::Begin("##wizard", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

                ImGui::TextUnformatted("Retro-DOS - first run");
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextWrapped("Choose where your DOS games live. Each game should "
                                   "be its own folder.");
                ImGui::Spacing();
                ImGui::TextDisabled(
                    "These folders belong to the app, so they need no permission and\n"
                    "work on removable storage. Android only grants access elsewhere\n"
                    "as a document tree, which the emulator cannot mount directly --\n"
                    "games kept outside have to be copied in first.");
                ImGui::Spacing();

                for (size_t i = 0; i < roots.size(); ++i) {
                    ImGui::PushID((int)i);
                    if (ImGui::RadioButton("##root", cfg.library_root == roots[i]))
                        cfg.library_root = roots[i];
                    ImGui::SameLine();
                    ImGui::TextWrapped("%s%s", roots[i].c_str(),
                                       path_is_dir(roots[i]) ? "" : "   (will be created)");
                    ImGui::PopID();
                }

                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::TextWrapped("Or choose any folder on the device:");
                if (retrodos::saf_has_grant()) {
                    ImGui::TextWrapped("Using: %s", library_label(cfg.library_root).c_str());
                    ImGui::TextDisabled("Each game is copied to the folder above the first\n"
                                        "time you play it, then mounted from there.");
                } else {
                    ImGui::TextDisabled("Android grants only the exact folder you pick,\n"
                                        "never all files. The emulator mounts real paths,\n"
                                        "so the game you launch is copied in first.");
                }
                if (ImGui::Button(retrodos::saf_has_grant() ? "Choose a different folder"
                                                            : "Choose folder...",
                                  ImVec2(0, 0)))
                    retrodos::saf_pick_folder();

                ImGui::Spacing();
                if (ImGui::Button("Rescan volumes", ImVec2(0, 0))) {
                    roots = candidate_roots();
                    for (const auto &r : roots) SDL_CreateDirectory(r.c_str());
                }
                ImGui::SameLine();
                if (ImGui::Button("Continue", ImVec2(0, 0))) {
                    SDL_CreateDirectory(cfg.library_root.c_str());
                    cfg.wizard_done = true;
                    retrodos::save_app_config(cfg_path, cfg);
                    refresh();
                    view = View::Library;
                }
                ImGui::End();
            }

            /* ---------------- Library ---------------- */
            else if (view == View::Library) {
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(full);
                ImGui::Begin("##library", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

                ImGui::TextUnformatted("Retro-DOS");
                ImGui::SameLine(full.x - 640);
                if (retrodos::media_available()) {
                    if (ImGui::Button(account.signed_in ? "RetroMedia *" : "RetroMedia",
                                      ImVec2(0, 0)))
                        view = View::Media;
                    ImGui::SameLine();
                }
                if (ImGui::Button("Settings", ImVec2(0, 0))) { selected = -1; view = View::Settings; }
                ImGui::SameLine();
                if (ImGui::Button("Rescan", ImVec2(0, 0))) refresh();
                ImGui::Separator();

                /* A-Z strip: with thousands of titles, scrolling is not a
                 * navigation method. */
                if (ImGui::Button("All")) filter_letter = 0;
                for (char c = 'A'; c <= 'Z'; ++c) {
                    ImGui::SameLine(0.0f, 2.0f);
                    ImGui::PushID((int)c);
                    const bool on = (filter_letter == c);
                    if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    const char lbl[2] = { c, 0 };
                    if (ImGui::Button(lbl)) filter_letter = on ? 0 : c;
                    if (on) ImGui::PopStyleColor();
                    ImGui::PopID();
                }
                ImGui::SameLine(0.0f, 2.0f);
                if (ImGui::Button("#")) filter_letter = (filter_letter == '#') ? 0 : '#';

                ImGui::SetNextItemWidth(full.x * 0.5f);
                ImGui::InputTextWithHint("##search", "Search...", search, sizeof(search));

                std::vector<int> shown;
                shown.reserve(games.size());
                for (int i = 0; i < (int)games.size(); ++i) {
                    if (filter_letter && games[i].initial != filter_letter) continue;
                    if (search[0] && !SDL_strcasestr(games[i].name.c_str(), search)) continue;
                    shown.push_back(i);
                }
                ImGui::Text("%zu of %zu", shown.size(), games.size());
                ImGui::SameLine();
                ImGui::TextDisabled("  %s", library_label(cfg.library_root).c_str());

                if (games.empty()) {
                    ImGui::Spacing();
                    ImGui::TextWrapped("No games found in:");
                    ImGui::TextWrapped("%s", library_label(cfg.library_root).c_str());
                    ImGui::Spacing();
                    ImGui::TextWrapped("Put each game in its own folder there, then Rescan.");
                } else {
                    ImGui::BeginChild("##list");
                    /* Clip: building a widget per title would cost thousands of
                     * draw calls a frame on a real collection. */
                    ImGuiListClipper clipper;
                    clipper.Begin((int)shown.size());
                    while (clipper.Step()) {
                        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                            const int gi = shown[r];
                            ImGui::PushID(gi);

                            /* Box art where we have it. Sized from the row
                             * height so the list keeps a single rhythm whether
                             * or not a title matched the catalogue. */
                            const float row_h = ImGui::GetFontSize() * 1.9f;
                            auto ai = art.find(games[gi].name);
                            if (ai != art.end() && ai->second) {
                                SDL_Texture *t = ai->second;
                                const float tw = (t->h > 0)
                                    ? row_h * ((float)t->w / (float)t->h) : row_h;
                                ImGui::Image((ImTextureID)(intptr_t)t, ImVec2(tw, row_h));
                                ImGui::SameLine();
                            }

                            if (ImGui::Selectable(games[gi].name.c_str(), false, 0,
                                                  ImVec2(0, ImGui::GetFontSize() * 1.9f)))
                                launch(games[gi]);
                            ImGui::SameLine(full.x * 0.55f);
                            ImGui::TextDisabled("%s", games[gi].run.empty()
                                                ? "(no runnable found)" : games[gi].run.c_str());
                            ImGui::SameLine(full.x * 0.86f);
                            if (ImGui::SmallButton("Setup")) { selected = gi; view = View::Settings; }
                            ImGui::PopID();
                        }
                    }
                    ImGui::EndChild();
                }
                ImGui::End();
            }

            /* ---------------- Settings ---------------- */
            else if (view == View::Settings) {
                static Settings edit;
                static int edit_for = -2;

                const bool per_game = (selected >= 0 && selected < (int)games.size());
                if (edit_for != selected) {
                    edit = cfg.defaults;
                    if (per_game) retrodos::load_game_settings(games_dir, games[selected].name, edit);
                    edit_for = selected;
                }

                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(full);
                ImGui::Begin("##settings", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

                if (per_game) ImGui::Text("Settings - %s", games[selected].name.c_str());
                else          ImGui::TextUnformatted("Settings - defaults for all games");
                ImGui::Separator();

                ImGui::BeginChild("##sset", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f));
                settings_widgets(edit);
                if (per_game) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Saved for this game only; others keep the defaults.");
                }
                ImGui::EndChild();

                if (ImGui::Button("Save", ImVec2(0, 0))) {
                    if (per_game) retrodos::save_game_settings(games_dir, games[selected].name, edit);
                    else { cfg.defaults = edit; retrodos::save_app_config(cfg_path, cfg); }
                    edit_for = -2; selected = -1; view = View::Library;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(0, 0))) {
                    edit_for = -2; selected = -1; view = View::Library;
                }
                ImGui::SameLine();
                if (ImGui::Button("Change games folder", ImVec2(0, 0))) {
                    edit_for = -2; selected = -1;
                    roots = candidate_roots();
                    view = View::Wizard;
                }
                ImGui::End();
            }

            /* ---------------- RetroMedia ---------------- */
            else if (view == View::Media) {
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(full);
                ImGui::Begin("##media", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);

                ImGui::TextUnformatted("RetroMedia");
                ImGui::SameLine(full.x - 200);
                if (ImGui::Button("Back", ImVec2(0, 0))) view = View::Library;
                ImGui::Separator();

                if (!media_msg.empty()) {
                    ImGui::TextWrapped("%s", media_msg.c_str());
                    ImGui::Separator();
                }

                if (ImGui::BeginTabBar("##mediatabs")) {

                    /* ---- Account ---- */
                    if (ImGui::BeginTabItem("Account")) {
                        if (account.signed_in) {
                            ImGui::Text("Signed in as %s", account.email.c_str());
                            if (account.is_admin) ImGui::TextUnformatted("Administrator");
                            else ImGui::TextDisabled("Standard account - artwork only");
                            ImGui::Text("Credits: %d    Free today: %d",
                                        account.credits, account.free_remaining);
                            ImGui::Spacing();
                            if (ImGui::Button("Sign out", ImVec2(0, 0))) {
                                media_busy = true;
                                retrodos::media_begin_logout();
                            }
                        } else {
                            ImGui::TextWrapped("Sign in to media.crownparkcomputing.com "
                                               "for box art. Administrators can also "
                                               "download games.");
                            ImGui::Spacing();
                            ImGui::SetNextItemWidth(full.x * 0.6f);
                            ImGui::InputText("Email", m_email, sizeof(m_email));
                            ImGui::SetNextItemWidth(full.x * 0.6f);
                            ImGui::InputText("Password", m_pass, sizeof(m_pass),
                                             ImGuiInputTextFlags_Password);
                            ImGui::BeginDisabled(media_busy || !m_email[0] || !m_pass[0]);
                            if (ImGui::Button("Sign in", ImVec2(0, 0))) {
                                media_busy = true; media_msg = "Signing in...";
                                retrodos::media_begin_login(m_email, m_pass);
                            }
                            ImGui::EndDisabled();

                            ImGui::Spacing();
                            ImGui::Separator();
                            /* An API key is the better credential on a handheld
                             * that may be shared: it can be revoked from the
                             * website without changing a password, and nothing
                             * reusable elsewhere is stored on the device. It is
                             * also the only route for an account that signs in
                             * with Google and so has no password to type. */
                            ImGui::TextWrapped("Or paste an API key from your account "
                                               "page (starts with rmk_):");
                            ImGui::SetNextItemWidth(full.x * 0.6f);
                            ImGui::InputText("API key", m_key, sizeof(m_key),
                                             ImGuiInputTextFlags_Password);
                            ImGui::BeginDisabled(media_busy || !m_key[0]);
                            if (ImGui::Button("Use API key", ImVec2(0, 0))) {
                                media_busy = true; media_msg = "Checking key...";
                                retrodos::media_begin_login_key(m_key);
                            }
                            ImGui::EndDisabled();
                        }
                        ImGui::EndTabItem();
                    }

                    /* ---- Artwork ---- */
                    if (ImGui::BeginTabItem("Artwork")) {
                        if (!account.signed_in) {
                            ImGui::TextWrapped("Sign in on the Account tab first.");
                        } else {
                            ImGui::TextWrapped("Match your library against the DOS "
                                               "catalogue and fetch box art for every "
                                               "title that is found.");
                            ImGui::Spacing();
                            ImGui::BeginDisabled(media_busy || games.empty());
                            if (ImGui::Button("Get artwork for my library", ImVec2(0, 0))) {
                                media_busy = true;
                                art_mode   = true;
                                art_total  = 1;   /* non-zero until the real count lands */
                                art_done   = 0;
                                media_msg  = "Fetching catalogue...";
                                retrodos::media_begin_catalogue("", "", false);
                            }
                            ImGui::EndDisabled();

                            if (art_total > 0 && art_done <= art_total)
                                ImGui::Text("%d / %d", art_done, art_total);
                            ImGui::Text("Cached: %zu", art.size());
                        }
                        ImGui::EndTabItem();
                    }

                    /* ---- Downloads (admin) ---- */
                    if (ImGui::BeginTabItem("Downloads")) {
                        if (!account.signed_in) {
                            ImGui::TextWrapped("Sign in on the Account tab first.");
                        } else if (!account.is_admin) {
                            /* Stated plainly rather than hidden: the server
                             * enforces this too, so a disabled button here is
                             * describing a real rule, not inventing one. */
                            ImGui::TextWrapped("Downloading games requires a RetroMedia "
                                               "administrator account. Artwork is "
                                               "available on any account.");
                        } else {
                            ImGui::SetNextItemWidth(full.x * 0.45f);
                            ImGui::InputTextWithHint("##csearch", "Search catalogue...",
                                                     cat_search, sizeof(cat_search));
                            ImGui::SameLine();
                            ImGui::BeginDisabled(media_busy);
                            if (ImGui::Button("Browse", ImVec2(0, 0))) {
                                media_busy = true;
                                art_mode   = false;
                                media_msg  = "Loading catalogue...";
                                const char l[2] = { cat_letter, 0 };
                                retrodos::media_begin_catalogue(cat_search,
                                                                cat_letter ? l : "", true);
                            }
                            ImGui::EndDisabled();

                            if (ImGui::Button("All")) cat_letter = 0;
                            for (char c = 'A'; c <= 'Z'; ++c) {
                                ImGui::SameLine(0.0f, 2.0f);
                                ImGui::PushID(1000 + c);
                                const bool on = (cat_letter == c);
                                if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                            ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                                const char lbl[2] = { c, 0 };
                                if (ImGui::Button(lbl)) cat_letter = on ? 0 : c;
                                if (on) ImGui::PopStyleColor();
                                ImGui::PopID();
                            }

                            ImGui::Text("%zu titles", catalogue.size());
                            ImGui::BeginChild("##cat");
                            ImGuiListClipper clip;
                            clip.Begin((int)catalogue.size());
                            while (clip.Step()) {
                                for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                                    const retrodos::MediaGame &cg = catalogue[r];
                                    ImGui::PushID(r);
                                    ImGui::TextUnformatted(cg.title.c_str());
                                    ImGui::SameLine(full.x * 0.6f);
                                    ImGui::TextDisabled("%.1f MB",
                                                        (double)cg.bytes / (1024.0 * 1024.0));
                                    ImGui::SameLine(full.x * 0.8f);
                                    ImGui::BeginDisabled(media_busy || cg.rom_files == 0);
                                    if (ImGui::SmallButton(cg.rom_files ? "Download"
                                                                        : "No files")) {
                                        media_busy = true;
                                        media_msg  = "Downloading " + cg.title + "...";
                                        /* Straight into the library root, so the
                                         * title appears where the scanner already
                                         * looks. */
                                        retrodos::media_begin_download(cg.slug,
                                                                       cfg.library_root);
                                    }
                                    ImGui::EndDisabled();
                                    ImGui::PopID();
                                }
                            }
                            ImGui::EndChild();
                        }
                        ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::End();
            }

            /* ---------------- In-game overlay ---------------- */
            if (view == View::Emulator && show_overlay) {
                ImGui::SetNextWindowPos(ImVec2(full.x * 0.5f, full.y * 0.35f),
                                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::Begin("Paused", nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_AlwaysAutoResize);
                char prog[64] = {0};
                retrodos_host_running_program(prog, sizeof(prog));
                ImGui::Text("Running: %s", prog[0] ? prog : "DOS");
                ImGui::Separator();
                const ImVec2 bw(ImGui::GetFontSize() * 11.0f, 0);
                if (ImGui::Button("Resume", bw)) show_overlay = false;
                if (ImGui::Button(show_osk ? "Hide keyboard" : "Show keyboard", bw)) {
                    show_osk = !show_osk; show_overlay = false;
                }
                if (ImGui::Button("Ctrl+Alt+Del", bw)) {
                    retrodos::osk_send_ctrl_alt_del(); show_overlay = false;
                }
                if (ImGui::Button("Reset machine", bw)) {
                    retrodos_host_reset(true); show_overlay = false;
                }
                if (ImGui::Button("Quit to library", bw)) {
                    retrodos_host_quit(); show_overlay = false;
                }
                ImGui::End();
            }

            /* ---------------- Always-on emulator controls ---------------- */
            /* A handheld has no Escape key and often no usable Back button, so
             * without this the menu and the on-screen keyboard are unreachable
             * once a game is running -- the emulator becomes a one-way trip.
             * Kept small and translucent, and parked in the top-right where DOS
             * games put the least. */
            if (view == View::Emulator && !show_overlay) {
                const float pad = ImGui::GetStyle().WindowPadding.x;
                ImGui::SetNextWindowPos(ImVec2(full.x - pad, pad),
                                        ImGuiCond_Always, ImVec2(1.0f, 0.0f));
                ImGui::SetNextWindowBgAlpha(0.35f);
                ImGui::Begin("##emubar", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing);
                if (ImGui::Button("Menu")) show_overlay = true;
                ImGui::SameLine();
                if (ImGui::Button(show_osk ? "Hide keys" : "Keyboard")) show_osk = !show_osk;
                ImGui::End();
            }

            if (view == View::Emulator && show_osk)
                retrodos::osk_draw(full.x, full.y);

            ImGui::Render();
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        }

        SDL_RenderPresent(ren);
    }

    if (engine.joinable()) { retrodos_host_quit(); engine.join(); }
    retrodos::save_app_config(cfg_path, cfg);

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (fb_tex) SDL_DestroyTexture(fb_tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
