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

#include <algorithm>
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
    std::string dir;
    std::string run;          /* DOS command, may be empty */
    char        initial = '#';
};

bool ends_with_ci(const std::string &s, const char *suffix)
{
    const size_t n = SDL_strlen(suffix);
    if (s.size() < n) return false;
    return SDL_strncasecmp(s.c_str() + s.size() - n, suffix, n) == 0;
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

    {   /* A handheld is held at arm's length: scale the whole UI rather than
         * shipping one designed for a mouse. */
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(win, &ww, &wh);
        const float scale = std::max(1.0f, (float)wh / 540.0f);
        ImGui::GetStyle().ScaleAllSizes(scale);
        ImGui::GetIO().FontGlobalScale = scale;
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

    std::vector<Game> games = scan_library(cfg.library_root);

    enum class View { Wizard, Library, Settings, Emulator };
    View view = cfg.wizard_done ? View::Library : View::Wizard;

    std::thread engine;
    SDL_Texture *fb_tex = nullptr;
    int fb_tex_w = 0, fb_tex_h = 0;
    uint64_t last_serial = 0;
    std::vector<uint32_t> fb_copy;

    char  search[128] = {0};
    char  filter_letter = 0;      /* 0 = no A-Z filter */
    int   selected = -1;          /* game index for per-game settings */
    Settings active = cfg.defaults;
    bool  show_overlay = false, show_osk = false;
    bool  running = true;

    auto launch = [&](const Game &g) {
        Settings s = cfg.defaults;
        retrodos::load_game_settings(games_dir, g.name, s);  /* overrides win */
        active = s;
        const std::string conf = retrodos::build_conf(s, g.name, g.dir, g.run);
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
        const bool ui_has_input = (view != View::Emulator) || show_overlay || show_osk;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ui_has_input) ImGui_ImplSDL3_ProcessEvent(&ev);

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
                if (!ui_has_input) retrodos_host_send_key((int)ev.key.scancode, down);
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                if (!ui_has_input && view == View::Emulator)
                    retrodos_host_mouse_move((int)ev.motion.xrel, (int)ev.motion.yrel);
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (!ui_has_input && view == View::Emulator) {
                    const int b = (ev.button.button == SDL_BUTTON_RIGHT)  ? 1 :
                                  (ev.button.button == SDL_BUTTON_MIDDLE) ? 2 : 0;
                    retrodos_host_mouse_button(b, ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                }
                break;

            default: break;
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
                games = scan_library(cfg.library_root);
            }
        }

        if (ui_has_input) {
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
                if (ImGui::Button("Rescan volumes", ImVec2(260, 0))) {
                    roots = candidate_roots();
                    for (const auto &r : roots) SDL_CreateDirectory(r.c_str());
                }
                ImGui::SameLine();
                if (ImGui::Button("Continue", ImVec2(260, 0))) {
                    SDL_CreateDirectory(cfg.library_root.c_str());
                    cfg.wizard_done = true;
                    retrodos::save_app_config(cfg_path, cfg);
                    games = scan_library(cfg.library_root);
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
                ImGui::SameLine(full.x - 380);
                if (ImGui::Button("Settings", ImVec2(170, 0))) { selected = -1; view = View::Settings; }
                ImGui::SameLine();
                if (ImGui::Button("Rescan", ImVec2(170, 0))) games = scan_library(cfg.library_root);
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

                if (games.empty()) {
                    ImGui::Spacing();
                    ImGui::TextWrapped("No games found in:");
                    ImGui::TextWrapped("%s", cfg.library_root.c_str());
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

                if (ImGui::Button("Save", ImVec2(200, 0))) {
                    if (per_game) retrodos::save_game_settings(games_dir, games[selected].name, edit);
                    else { cfg.defaults = edit; retrodos::save_app_config(cfg_path, cfg); }
                    edit_for = -2; selected = -1; view = View::Library;
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(200, 0))) {
                    edit_for = -2; selected = -1; view = View::Library;
                }
                ImGui::SameLine();
                if (ImGui::Button("Change games folder", ImVec2(340, 0))) {
                    edit_for = -2; selected = -1;
                    roots = candidate_roots();
                    view = View::Wizard;
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
                const ImVec2 bw(360, 0);
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
