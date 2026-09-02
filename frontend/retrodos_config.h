/*
 * retro-dosbox — persisted settings, and the DOSBox-X conf they generate.
 *
 * Two layers, because DOS titles disagree with each other constantly:
 *   global defaults  — what a game gets unless it says otherwise
 *   per-game override — one file per title, only the keys that differ
 *
 * Kept as plain key=value text rather than anything structured: it is small,
 * a human can fix it with a text editor when a game will not start, and it
 * survives an app update with no migration code.
 */
#ifndef RETRODOS_CONFIG_H
#define RETRODOS_CONFIG_H

#include <string>
#include <vector>

namespace retrodos {

/* The knobs that actually decide whether a DOS game runs. Every one of these
 * is here because getting it wrong breaks real titles, not for completeness. */
struct Settings {
    /* cycles: "max" suits most later titles; a FIXED count is what saves the
     * early ones, which busy-wait for timing and run absurdly fast otherwise. */
    bool        cycles_max   = true;
    int         cycles_fixed = 20000;

    /* dynamic is much faster; normal is the compatibility fallback. */
    bool        core_dynamic = true;

    /* 32 MB, not more: DOS/4GW 1.97 miscalculates with large memory and a pile
     * of early-90s titles refuse to start above it. */
    int         memsize      = 32;

    /* sbpro2 is the safest broad default for the DOS era. */
    std::string sbtype       = "sbpro2";

    /* DOS modes are frequently non-square-pixel: 320x200 is a 4:3 picture. */
    bool        aspect_correct = true;
    /* Integer scaling is sharper but leaves bigger borders on a handheld. */
    bool        integer_scale  = false;

    bool operator==(const Settings &o) const {
        return cycles_max == o.cycles_max && cycles_fixed == o.cycles_fixed &&
               core_dynamic == o.core_dynamic && memsize == o.memsize &&
               sbtype == o.sbtype && aspect_correct == o.aspect_correct &&
               integer_scale == o.integer_scale;
    }
};

/* App-level state that is not per-game. */
struct AppConfig {
    std::string library_root;      /* where games live                    */
    bool        wizard_done = false;
    Settings    defaults;
};

bool load_app_config(const std::string &path, AppConfig &out);
bool save_app_config(const std::string &path, const AppConfig &cfg);

/* Per-game overrides live beside the app config, one file per title. */
bool load_game_settings(const std::string &dir, const std::string &game, Settings &out);
bool save_game_settings(const std::string &dir, const std::string &game, const Settings &s);

/* Build the DOSBox-X conf for one title. mount_dir is a REAL filesystem path:
 * DOSBox-X mounts a directory by path and cannot be handed a content:// URI. */
/* run_raw emits run_cmd into autoexec unquoted. Program names are quoted
 * because DOS titles routinely contain spaces, but a built-in DOSBox-X command
 * that takes an argument -- "boot FREEDOS.IMG" -- would then be read as the
 * name of a program to find, and fail. */
std::string build_conf(const Settings &s, const std::string &title,
                       const std::string &mount_dir, const std::string &run_cmd,
                       bool run_raw = false);

} /* namespace retrodos */

#endif /* RETRODOS_CONFIG_H */
