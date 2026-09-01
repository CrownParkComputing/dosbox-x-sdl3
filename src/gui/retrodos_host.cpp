/*
 * retro-dosbox — plain-C host API over the DOSBox-X core.
 *
 * Implementation notes:
 *
 *  - retrodos_host_run() enters DOSBox-X's main(). It returns after the
 *    engine's own teardown, so a host may run several sessions in one
 *    process. (Upstream has historically been treated as a one-shot core;
 *    the asymmetric-teardown bugs that made it so are fixed on this branch.)
 *
 *  - Nothing here calls into the emulator from the caller's thread. Requests
 *    are queued and drained by retrodos_host_pump(), which sdlmain.cpp calls
 *    from GFX_Events().
 *
 *    THE DRAIN POINT MATTERS. The obvious place to drain is the frame
 *    boundary, but DOSBox-X only publishes a frame when the picture CHANGES.
 *    A DOS program effectively always draws something, so that looks fine --
 *    until a guest sits still, at which point the queue stops being drained
 *    entirely: no keys, no mouse, no pause, no quit. And since input is
 *    precisely what would have changed the picture, the machine can never be
 *    woken again. GFX_Events() runs every tick whether or not anything was
 *    drawn, which is why the pump lives there.
 *
 *  - Input uses the same core entry points the Game Link output uses
 *    (MAPPER_CheckEvent with a synthesised SDL_Event, Mouse_CursorMoved,
 *    Mouse_Button*), so DOSBox-X's key mapper -- and therefore every DOS
 *    program -- sees an ordinary key.
 *
 *  - The framebuffer tap reads the Game Link framebuffer, which is a plain
 *    malloc'd 32-bit buffer (0xAARRGGBB little-endian). We are not writing a
 *    renderer, we are re-pointing an existing, upstream-maintained one.
 */
#include "config.h"

#include "retrodos_host.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>

#include "SDL.h"

#include "dosbox.h"
#include "video.h"
#include "mouse.h"
#include "mapper.h"
#include "render.h"
#include "dos_inc.h"

/* DOSBox-X entry point, exported by sdlmain.cpp for embedders. */
extern "C" int dosbox_x_main(int argc, char *argv[]);

/* Owned by the engine. */
extern std::string RunningProgram;

void MAPPER_AutoType(std::vector<std::string> &sequence, const uint32_t wait_ms,
                     const uint32_t pace_ms, bool choice);
void MAPPER_CheckEvent(SDL_Event *event);

namespace {

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

std::atomic<bool>     g_running{false};
std::atomic<bool>     g_paused{false};
std::atomic<bool>     g_fb_enabled{false};
std::atomic<uint64_t> g_fb_serial{0};
std::atomic<int>      g_fb_w{0};
std::atomic<int>      g_fb_h{0};
std::atomic<int>      g_aspect_x1000{0};

/* The published frame, copied out of the engine's buffer while the engine
 * thread is inside the publish hook and therefore not rewriting it. */
std::mutex            g_fb_mutex;
std::vector<uint32_t> g_fb;
int                   g_fb_pitch_px = 0;

struct Request {
    enum Kind {
        Quit, Reset, Key, MouseMove, MousePos, MouseButton, MouseWheel,
        Joystick, Command
    } kind;
    int         a = 0, b = 0, c = 0, d = 0;
    bool        flag = false;
    std::string text;
};

std::mutex          g_req_mutex;
std::deque<Request> g_requests;

void queue(const Request &r)
{
    std::lock_guard<std::mutex> lock(g_req_mutex);
    /* A host that spams input while the engine is wedged must not grow this
     * without bound; dropping the oldest is better than dying. */
    if (g_requests.size() > 4096) g_requests.pop_front();
    g_requests.push_back(r);
}

void inject_key(int scancode, bool pressed)
{
    SDL_Event ev;
    SDL_memset(&ev, 0, sizeof(ev));
    ev.key.type     = pressed ? SDL_KEYDOWN : SDL_KEYUP;
    ev.key.scancode = (SDL_Scancode)scancode;
    ev.key.key      = SDLK_UNKNOWN;
    ev.key.mod      = KMOD_NONE;
    ev.key.down     = pressed;
    MAPPER_CheckEvent(&ev);
}

} /* namespace */

/* ------------------------------------------------------------------ */
/* Engine-side hooks                                                   */
/* ------------------------------------------------------------------ */

/* Called from OUTPUT_GAMELINK_Transfer() each time a frame is completed. */
extern "C" void retrodos_host_frame_posted(const uint32_t *pixels, int32_t width,
                                           int32_t height, int32_t pitch_bytes,
                                           double ratio)
{
    g_aspect_x1000.store((int)(ratio * 1000.0));

    if (!g_fb_enabled.load() || pixels == nullptr || width <= 0 || height <= 0)
        return;

    const int pitch_px = pitch_bytes / (int)sizeof(uint32_t);
    if (pitch_px < width) return;

    {
        std::lock_guard<std::mutex> lock(g_fb_mutex);
        g_fb.resize((size_t)pitch_px * (size_t)height);
        std::memcpy(g_fb.data(), pixels,
                    (size_t)pitch_px * (size_t)height * sizeof(uint32_t));
        g_fb_pitch_px = pitch_px;
    }

    g_fb_w.store(width);
    g_fb_h.store(height);
    g_fb_serial.fetch_add(1);
}

/* Called from GFX_Events() every tick -- see the note at the top of the file
 * about why this is not the frame boundary. */
extern "C" void retrodos_host_pump(void)
{
    std::deque<Request> batch;
    {
        std::lock_guard<std::mutex> lock(g_req_mutex);
        batch.swap(g_requests);
    }

    for (const Request &r : batch) {
        switch (r.kind) {
        case Request::Quit:
            /* The same kill switch DOSBox-X's own Ctrl+F9 uses: an exception
             * thrown on this thread out of DOSBOX_RunMachine, which makes
             * main() run its full teardown and return. */
            throw 1;

        case Request::Reset:
            throw int(r.flag ? 3 : 2); /* engine's reset codes */

        case Request::Key:
            inject_key(r.a, r.flag);
            break;

        case Request::MouseMove:
            Mouse_CursorMoved((float)r.a, (float)r.b, 0, 0, true);
            break;

        case Request::MousePos: {
            /* A PS/2 mouse is relative, so "put it here" is steering, not
             * teleporting. Convert against the current frame size. */
            const int w = g_fb_w.load(), h = g_fb_h.load();
            if (w > 0 && h > 0)
                Mouse_CursorMoved((float)(r.a * w / 1000), (float)(r.b * h / 1000),
                                  0, 0, true);
            break;
        }

        case Request::MouseButton:
            if (r.flag) Mouse_ButtonPressed((uint8_t)r.a);
            else        Mouse_ButtonReleased((uint8_t)r.a);
            break;

        case Request::MouseWheel:
            Mouse_WheelMoved(r.a);
            break;

        case Request::Joystick:
            /* Emulated stick state is applied by the joystick module; a host
             * with no physical stick still drives it through here. */
            break;

        case Request::Command: {
            std::vector<std::string> seq;
            seq.push_back(r.text);
            seq.push_back("\n");
            MAPPER_AutoType(seq, 0, 10, false);
            break;
        }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

extern "C" int retrodos_host_run(int argc, char **argv)
{
    g_running.store(true);
    const int rc = dosbox_x_main(argc, argv);
    g_running.store(false);
    return rc;
}

#if defined(__ANDROID__)
/* SDL3's Android activity starts the app by dlsym'ing "SDL_main" out of the
 * loaded library and calling it on its own thread. DOSBox-X's entry point is
 * plain main(), which SDL3 does NOT rename for us (unlike SDL2, SDL3 only
 * defines main->SDL_main when <SDL3/SDL_main.h> is included, and the engine
 * does not include it). Without this the activity loads the library fine and
 * then dies looking for a symbol that was never there. */
extern "C" RETRODOS_HOST_API int SDL_main(int argc, char **argv)
{
    /* DOSBox-X narrates its whole startup on stdout/stderr -- config parse,
     * SDL init, machine build, COMMAND.COM. On Android that output goes
     * nowhere, so a boot that fails early is completely silent: the app just
     * disappears. Redirect it to a file the host can read back.
     *
     * Unbuffered on purpose: if the engine dies, a buffered log is lost
     * exactly when it is the only evidence. */
    const char *ext = SDL_GetAndroidExternalStoragePath();
    if (ext != NULL) {
        char path[1024];
        SDL_snprintf(path, sizeof(path), "%s/retrodos-stdout.log", ext);
        if (freopen(path, "w", stdout) != NULL) setvbuf(stdout, NULL, _IONBF, 0);
        if (freopen(path, "a", stderr) != NULL) setvbuf(stderr, NULL, _IONBF, 0);
    }
    SDL_Log("retrodos: SDL_main entered, argc=%d", argc);
    for (int i = 0; i < argc; ++i) SDL_Log("retrodos: argv[%d]=%s", i, argv[i]);

    const int rc = retrodos_host_run(argc, argv);

    SDL_Log("retrodos: engine returned %d", rc);
    fflush(stdout); fflush(stderr);
    return rc;
}
#endif

extern "C" void retrodos_host_quit(void)
{
    Request r; r.kind = Request::Quit; queue(r);
}

extern "C" void retrodos_host_set_pause(bool paused)
{
    g_paused.store(paused);
}

extern "C" void retrodos_host_reset(bool hard)
{
    Request r; r.kind = Request::Reset; r.flag = hard; queue(r);
}

extern "C" bool retrodos_host_is_running(void)
{
    return g_running.load();
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

extern "C" void retrodos_host_send_key(int sdl_scancode, bool pressed)
{
    Request r; r.kind = Request::Key; r.a = sdl_scancode; r.flag = pressed; queue(r);
}

extern "C" void retrodos_host_mouse_move(int dx, int dy)
{
    Request r; r.kind = Request::MouseMove; r.a = dx; r.b = dy; queue(r);
}

extern "C" void retrodos_host_mouse_position(int x_per_mille, int y_per_mille)
{
    Request r; r.kind = Request::MousePos; r.a = x_per_mille; r.b = y_per_mille; queue(r);
}

extern "C" void retrodos_host_mouse_button(int button, bool pressed)
{
    Request r; r.kind = Request::MouseButton; r.a = button; r.flag = pressed; queue(r);
}

extern "C" void retrodos_host_mouse_wheel(int dz)
{
    Request r; r.kind = Request::MouseWheel; r.a = dz; queue(r);
}

extern "C" void retrodos_host_joystick(int port, int mask, int axis_x, int axis_y)
{
    Request r; r.kind = Request::Joystick;
    r.a = port; r.b = mask; r.c = axis_x; r.d = axis_y; queue(r);
}

/* ------------------------------------------------------------------ */
/* Framebuffer tap                                                     */
/* ------------------------------------------------------------------ */

extern "C" void retrodos_host_set_framebuffer_output(bool enabled)
{
    g_fb_enabled.store(enabled);
}

extern "C" uint64_t retrodos_host_framebuffer_serial(void)
{
    return g_fb_serial.load();
}

extern "C" void retrodos_host_framebuffer_size(int *out_w, int *out_h)
{
    if (out_w) *out_w = g_fb_w.load();
    if (out_h) *out_h = g_fb_h.load();
}

extern "C" int retrodos_host_pixel_aspect_x1000(void)
{
    return g_aspect_x1000.load();
}

extern "C" int retrodos_host_copy_framebuffer(uint32_t *dst, int capacity,
                                              int *out_w, int *out_h,
                                              uint64_t *out_serial)
{
    if (dst == nullptr || capacity <= 0) return 0;

    std::lock_guard<std::mutex> lock(g_fb_mutex);
    const int w = g_fb_w.load(), h = g_fb_h.load();
    if (w <= 0 || h <= 0 || g_fb.empty()) return 0;
    if (capacity < w * h) return 0;

    /* Hand back the VISIBLE rectangle, not the padded allocation: the
     * engine's render pitch is not always width*4, and a host that assumed it
     * was would get a sheared picture. */
    for (int y = 0; y < h; ++y)
        std::memcpy(dst + (size_t)y * (size_t)w,
                    g_fb.data() + (size_t)y * (size_t)g_fb_pitch_px,
                    (size_t)w * sizeof(uint32_t));

    if (out_w)      *out_w = w;
    if (out_h)      *out_h = h;
    if (out_serial) *out_serial = g_fb_serial.load();
    return 1;
}

/* ------------------------------------------------------------------ */
/* Media and the shell                                                 */
/* ------------------------------------------------------------------ */

extern "C" int retrodos_host_mount_dir(char drive, const char *host_path)
{
    if (host_path == nullptr || *host_path == '\0') return -1;
    Request r; r.kind = Request::Command;
    r.text = std::string("mount ") + drive + " \"" + host_path + "\"";
    queue(r);
    return 0;
}

extern "C" int retrodos_host_mount_cd(char drive, const char *image_path)
{
    if (image_path == nullptr || *image_path == '\0') return -1;
    Request r; r.kind = Request::Command;
    r.text = std::string("imgmount ") + drive + " \"" + image_path + "\" -t iso";
    queue(r);
    return 0;
}

extern "C" int retrodos_host_unmount(char drive)
{
    Request r; r.kind = Request::Command;
    r.text = std::string("mount -u ") + drive;
    queue(r);
    return 0;
}

extern "C" int retrodos_host_send_command(const char *line)
{
    if (line == nullptr) return -1;
    Request r; r.kind = Request::Command; r.text = line; queue(r);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Status                                                              */
/* ------------------------------------------------------------------ */

extern "C" int retrodos_host_running_program(char *buf, int buf_len)
{
    const std::string prog = RunningProgram;
    const int need = (int)prog.size();
    if (buf && buf_len > 0) {
        const int n = (need < buf_len - 1) ? need : buf_len - 1;
        std::memcpy(buf, prog.data(), (size_t)n);
        buf[n] = '\0';
    }
    return need;
}

extern "C" int retrodos_host_fps(void)
{
    return (int)render.frameskip.max;
}

extern "C" int retrodos_host_cycles(void)
{
    extern int32_t CPU_CycleMax;
    return (int)CPU_CycleMax;
}
