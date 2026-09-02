/*
 * retro-dosbox frontend — SDL3 + Dear ImGui.
 *
 * This owns the window. The engine runs HEADLESS behind it, through the Game
 * Link output, and hands us finished frames via the retrodos_host framebuffer
 * tap. That arrangement is what buys us all three things at once:
 *
 *   - DOSBox-X's own menu bar and config GUI never appear, because there is no
 *     DOSBox-X window for them to be drawn into. We are not hiding the old UI,
 *     we are not creating it.
 *   - The DOS picture is SCALED. With --disable-opengl the engine's only
 *     on-screen backend is `surface`, which blits 1:1 and leaves a 720x540
 *     picture sitting in the corner of a 1920x1080 handheld. Here the frame is
 *     a texture and we choose the rectangle.
 *   - There is somewhere to put a launcher, which an emulator that boots
 *     straight into DOS has nowhere to host.
 *
 * Two threads:
 *   engine thread — retrodos_host_run(), the blocking DOSBox-X mainloop
 *   main thread   — SDL events, ImGui, and the frame blit
 * Everything the main thread sends the engine goes through retrodos_host_*,
 * which queues rather than touching emulator state. See retrodos_host.h.
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "retrodos_host.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(__ANDROID__)
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "retrodos", __VA_ARGS__)
#else
#define LOGI(...) do { SDL_Log(__VA_ARGS__); } while (0)
#endif

namespace {

/* ------------------------------------------------------------------ */
/* Game discovery                                                      */
/* ------------------------------------------------------------------ */

struct Game {
    std::string name;      /* shown in the launcher            */
    std::string dir;       /* host folder mounted as C:        */
    std::string exe;       /* DOS command to run, may be empty */
};

bool ends_with_ci(const std::string &s, const char *suffix)
{
    const size_t n = SDL_strlen(suffix);
    if (s.size() < n) return false;
    return SDL_strncasecmp(s.c_str() + s.size() - n, suffix, n) == 0;
}

/* A game is a folder under the library root. Its runnable is the first
 * .BAT/.COM/.EXE we find -- .BAT first, because installers habitually leave a
 * one-line batch file that sets up the environment the .EXE expects. */
void find_runnable(const std::string &dir, Game &g)
{
    int count = 0;
    char **files = SDL_GlobDirectory(dir.c_str(), NULL, SDL_GLOB_CASEINSENSITIVE, &count);
    if (!files) return;

    std::string bat, com, exe;
    for (int i = 0; i < count; ++i) {
        const std::string f = files[i];
        if      (bat.empty() && ends_with_ci(f, ".bat")) bat = f;
        else if (com.empty() && ends_with_ci(f, ".com")) com = f;
        else if (exe.empty() && ends_with_ci(f, ".exe")) exe = f;
    }
    SDL_free(files);

    if      (!bat.empty()) g.exe = bat;
    else if (!com.empty()) g.exe = com;
    else if (!exe.empty()) g.exe = exe;
}

std::vector<Game> scan_library(const std::string &root)
{
    std::vector<Game> games;
    int count = 0;
    char **entries = SDL_GlobDirectory(root.c_str(), NULL, 0, &count);
    if (!entries) return games;

    for (int i = 0; i < count; ++i) {
        const std::string name = entries[i];
        if (name == "." || name == "..") continue;

        const std::string full = root + "/" + name;
        SDL_PathInfo info;
        if (!SDL_GetPathInfo(full.c_str(), &info)) continue;
        if (info.type != SDL_PATHTYPE_DIRECTORY) continue;

        Game g;
        g.name = name;
        g.dir  = full;
        find_runnable(full, g);
        games.push_back(g);
    }
    SDL_free(entries);

    std::sort(games.begin(), games.end(),
              [](const Game &a, const Game &b) {
                  return SDL_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
              });
    return games;
}

/* ------------------------------------------------------------------ */
/* Config generation                                                   */
/* ------------------------------------------------------------------ */

std::string write_conf(const std::string &conf_path, const std::string &pref_dir,
                       const Game &g)
{
    std::string s;
    s += "[sdl]\n";
    /* The engine renders offscreen; this frontend owns the window. Both lines
     * are required -- output=gamelink alone silently falls back to
     * output=surface, and then the engine renders into a window nobody reads
     * and the screen stays black with no error anywhere. */
    s += "output=gamelink\n";
    s += "gamelink master=true\n";
    s += "autolock=false\n";
    s += "waitonerror=false\n";
    s += "showmenu=false\n";

    s += "[dosbox]\n";
    /* Without this a non-TTY stdin makes DOSBox-X prompt for a working
     * directory and block forever in a folder picker. On Android there is no
     * stdin and no dialog: it hangs with no diagnostic at all. */
    s += "working directory option=noprompt\n";
    /* 32, not more: DOS/4GW 1.97 miscalculates with large memory and a pile of
     * early-90s titles refuse to start. */
    s += "memsize=32\n";
    s += "title=" + g.name + "\n";

    s += "[cpu]\n";
    s += "core=dynamic\n";
    s += "cycles=max\n";

    s += "[sblaster]\n";
    /* sbpro2 is the safest broad default for DOS-era titles. */
    s += "sbtype=sbpro2\n";

    s += "[autoexec]\n";
    s += "mount C \"" + g.dir + "\"\n";
    s += "C:\n";
    if (!g.exe.empty()) s += "\"" + g.exe + "\"\n";

    (void)pref_dir;

    SDL_IOStream *io = SDL_IOFromFile(conf_path.c_str(), "w");
    if (io) {
        SDL_WriteIO(io, s.data(), s.size());
        SDL_CloseIO(io);
    }
    return conf_path;
}

/* ------------------------------------------------------------------ */
/* Engine thread                                                       */
/* ------------------------------------------------------------------ */

std::atomic<bool> g_engine_done{false};

SDL_Window *g_host_window = nullptr;

void engine_thread(std::string conf, std::string defaultdir)
{
    /* The engine adopts this rather than creating a second window -- see
     * retrodos_host_set_window() for why that matters on Android. */
    retrodos_host_set_window(g_host_window);

    char a0[] = "dosbox-x";
    char a1[] = "-conf";
    char a3[] = "-defaultdir";
    std::vector<char> c(conf.begin(), conf.end()); c.push_back('\0');
    std::vector<char> d(defaultdir.begin(), defaultdir.end()); d.push_back('\0');
    char *argv[] = { a0, a1, c.data(), a3, d.data(), nullptr };

    retrodos_host_set_framebuffer_output(true);
    retrodos_host_run(5, argv);
    g_engine_done.store(true);
}

/* ------------------------------------------------------------------ */
/* Aspect-correct destination rect                                     */
/* ------------------------------------------------------------------ */

SDL_FRect fit(int fb_w, int fb_h, int aspect_x1000, int win_w, int win_h)
{
    /* DOS modes are frequently non-square-pixel: 320x200 is a 4:3 picture, not
     * 16:10. The engine tells us the ratio; ignoring it makes everything look
     * subtly squashed and is the classic DOS-emulator tell. */
    double ratio = (aspect_x1000 > 0) ? (aspect_x1000 / 1000.0)
                                      : ((double)fb_w / (double)fb_h);
    if (ratio <= 0.0) ratio = 4.0 / 3.0;

    double w = (double)win_w;
    double h = w / ratio;
    if (h > (double)win_h) { h = (double)win_h; w = h * ratio; }

    SDL_FRect r;
    r.w = (float)w;
    r.h = (float)h;
    r.x = (float)((win_w - w) * 0.5);
    r.y = (float)((win_h - h) * 0.5);
    return r;
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
    g_host_window = win;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;   /* no layout persistence; our own prefs own state */
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer3_Init(ren);

    /* A handheld is held at arm's length, not on a desk: scale the whole UI
     * rather than shipping a UI designed for a mouse. */
    {
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(win, &ww, &wh);
        const float scale = std::max(1.0f, (float)wh / 480.0f);
        ImGui::GetStyle().ScaleAllSizes(scale);
        io.FontGlobalScale = scale;
    }

    /* Library root: the app's external files dir, which needs no permission
     * and is reachable over MTP/adb so a user can actually put games there. */
    std::string root;
#if defined(__ANDROID__)
    if (const char *ext = SDL_GetAndroidExternalStoragePath()) root = std::string(ext) + "/dos";
#else
    if (const char *pref = SDL_GetPrefPath("CrownParkComputing", "Retro-DOS")) {
        root = std::string(pref) + "dos";
        SDL_free((void *)pref);
    }
#endif
    SDL_CreateDirectory(root.c_str());

    std::vector<Game> games = scan_library(root);
    LOGI("library %s: %zu game(s)", root.c_str(), games.size());

    enum class View { Launcher, Emulator };
    View view = View::Launcher;

    std::thread engine;
    SDL_Texture *fb_tex = nullptr;
    int fb_tex_w = 0, fb_tex_h = 0;
    uint64_t last_serial = 0;
    std::vector<uint32_t> fb_copy;
    bool running = true;
    bool show_overlay = false;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (view == View::Launcher) ImGui_ImplSDL3_ProcessEvent(&ev);

            switch (ev.type) {
            case SDL_EVENT_QUIT:
                running = false;
                break;

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                const bool down = (ev.type == SDL_EVENT_KEY_DOWN);
                if (view == View::Emulator) {
                    /* Back/Escape opens the in-game overlay rather than
                     * reaching DOS, so there is always a way out on a handheld
                     * with no keyboard. */
                    if (ev.key.scancode == SDL_SCANCODE_AC_BACK ||
                        ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                        if (down) show_overlay = !show_overlay;
                        break;
                    }
                    if (!show_overlay)
                        retrodos_host_send_key((int)ev.key.scancode, down);
                }
                break;
            }

            case SDL_EVENT_MOUSE_MOTION:
                if (view == View::Emulator && !show_overlay)
                    retrodos_host_mouse_move((int)ev.motion.xrel, (int)ev.motion.yrel);
                break;

            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (view == View::Emulator && !show_overlay) {
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

        /* ImGui's SDL_Renderer backend sets a clip rect per draw command. The
         * launcher renders ImGui; the emulator view does not, so whatever clip
         * was left standing from the last ImGui frame silently survives and
         * can discard the DOS picture entirely -- frames arrive, the texture
         * updates, nothing appears. Reset the render state each frame so this
         * view starts from a known one. */
        SDL_SetRenderClipRect(ren, NULL);
        SDL_SetRenderViewport(ren, NULL);
        SDL_SetRenderDrawColor(ren, 12, 12, 16, 255);
        SDL_RenderClear(ren);

        if (view == View::Emulator) {
            /* Poll the serial rather than the pixels: DOS text mode can sit on
             * an identical picture for seconds, and re-uploading it every
             * frame would burn a handheld's battery for nothing. */
            /* one line a second, not one a frame */
            static Uint64 last_log = 0;
            const Uint64 now_ms = SDL_GetTicks();
            const uint64_t serial = retrodos_host_framebuffer_serial();
            if (now_ms - last_log > 1000) {
                last_log = now_ms;
                int dw = 0, dh = 0;
                retrodos_host_framebuffer_size(&dw, &dh);
                LOGI("tap: serial=%llu size=%dx%d running=%d",
                     (unsigned long long)serial, dw, dh,
                     (int)retrodos_host_is_running());
            }
            if (serial != last_serial) {
                int w = 0, h = 0;
                retrodos_host_framebuffer_size(&w, &h);
                if (w > 0 && h > 0) {
                    fb_copy.resize((size_t)w * (size_t)h);
                    uint64_t got = 0;
                    const int copied = retrodos_host_copy_framebuffer(
                        fb_copy.data(), (int)fb_copy.size(), &w, &h, &got);
                    if (now_ms - last_log <= 20) {
                        size_t nz = 0;
                        for (size_t i = 0; i < fb_copy.size(); ++i)
                            if (fb_copy[i] & 0x00FFFFFFu) nz++;
                        LOGI("copy=%d %dx%d nonblack=%zu/%zu tex=%p",
                             copied, w, h, nz, fb_copy.size(), (void *)fb_tex);
                    }
                    if (copied) {
                        if (!fb_tex || fb_tex_w != w || fb_tex_h != h) {
                            if (fb_tex) SDL_DestroyTexture(fb_tex);
                            /* The engine publishes 0xAARRGGBB little-endian,
                             * which is exactly ARGB8888 -- no conversion. */
                            fb_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                                       SDL_TEXTUREACCESS_STREAMING, w, h);
                            SDL_SetTextureScaleMode(fb_tex, SDL_SCALEMODE_NEAREST);
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
                                          win_w, win_h);
                SDL_RenderTexture(ren, fb_tex, nullptr, &dst);
            }

            if (g_engine_done.load()) {
                if (engine.joinable()) engine.join();
                g_engine_done.store(false);
                view = View::Launcher;
                show_overlay = false;
                games = scan_library(root);
            }
        }

        if (view == View::Launcher || show_overlay) {
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            if (view == View::Launcher) {
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2((float)win_w, (float)win_h));
                ImGui::Begin("Retro-DOS", nullptr,
                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoBringToFrontOnFocus);

                ImGui::TextUnformatted("Retro-DOS");
                ImGui::Separator();

                if (games.empty()) {
                    ImGui::TextWrapped("No games found.");
                    ImGui::Spacing();
                    ImGui::TextWrapped("Put each game in its own folder under:");
                    ImGui::TextWrapped("%s", root.c_str());
                    ImGui::Spacing();
                    if (ImGui::Button("Rescan")) games = scan_library(root);
                } else {
                    ImGui::Text("%zu game(s)", games.size());
                    ImGui::Spacing();
                    ImGui::BeginChild("##list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()));
                    for (size_t i = 0; i < games.size(); ++i) {
                        const Game &g = games[i];
                        ImGui::PushID((int)i);
                        if (ImGui::Selectable(g.name.c_str(), false, 0,
                                              ImVec2(0, ImGui::GetFontSize() * 2.0f))) {
                            std::string conf;
#if defined(__ANDROID__)
                            if (const char *ext = SDL_GetAndroidInternalStoragePath())
                                conf = std::string(ext) + "/dosbox-x.conf";
#else
                            conf = root + "/../dosbox-x.conf";
#endif
                            write_conf(conf, root, g);
                            last_serial = 0;
                            engine = std::thread(engine_thread, conf, root);
                            view = View::Emulator;
                        }
                        if (!g.exe.empty()) {
                            ImGui::SameLine();
                            ImGui::TextDisabled("  %s", g.exe.c_str());
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndChild();
                    if (ImGui::Button("Rescan")) games = scan_library(root);
                }
                ImGui::End();
            } else {
                /* In-game overlay: the replacement for DOSBox-X's menu bar. */
                ImGui::SetNextWindowPos(ImVec2(win_w * 0.5f, win_h * 0.5f),
                                        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
                ImGui::Begin("Paused", nullptr,
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_AlwaysAutoResize);
                char prog[64] = {0};
                retrodos_host_running_program(prog, sizeof(prog));
                ImGui::Text("Running: %s", prog[0] ? prog : "DOS");
                ImGui::Separator();
                if (ImGui::Button("Resume", ImVec2(220, 0))) show_overlay = false;
                if (ImGui::Button("Reset", ImVec2(220, 0))) {
                    retrodos_host_reset(true);
                    show_overlay = false;
                }
                if (ImGui::Button("Quit to launcher", ImVec2(220, 0))) {
                    retrodos_host_quit();
                    show_overlay = false;
                }
                ImGui::End();
            }

            ImGui::Render();
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), ren);
        }

        SDL_RenderPresent(ren);
    }

    if (engine.joinable()) {
        retrodos_host_quit();
        engine.join();
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    if (fb_tex) SDL_DestroyTexture(fb_tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
