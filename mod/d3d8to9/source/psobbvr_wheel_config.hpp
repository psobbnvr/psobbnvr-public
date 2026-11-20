#pragma once

// The spell wheel's settings: the [wheel] section of psobbvr.ini over
// psobbvr-defaults.ini (psobbvr_settings.hpp), the knobs below plus each
// character's pages (psobbvr_wheel_core.hpp). Loaded with the controller
// bindings and reloaded with them when psobbvr.ini changes
// (bindings::Load calls Load), so a save from the Options program reaches
// a running game within about a second. Every load bumps `generation`, so
// the wheel can tell a reload from a release of its button.

#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "psobbvr_log.hpp"
#include "psobbvr_settings.hpp"
#include "psobbvr_wheel_core.hpp"

namespace wheelconfig {

// Which hand's swing casts the wheel's choice.
enum SwingHand : int { SWING_BOTH = 0, SWING_LEFT = 1, SWING_RIGHT = 2 };
constexpr const char* kSwingHandKeys[3] = {"both", "left", "right"};

struct Config {
    bool enabled = true;
    int swing_hand = SWING_BOTH;
    float size_m = 0.35f;             // the ring's width
    float ring_raise_m = 0.10f;       // the ring's centre above the right grip
    float ring_toward_head_m = 0.05f; // ...and moved toward the head (negative: away)
    int open_delay_ms = 120;          // how long the button is held before it opens
    bool hand_icon = true;            // the choice's icon on the palm of the left hand
    bool hand_icon_depth_test = true; // hidden behind the hand and weapon (0: always on top)
    float hand_icon_size_m = 0.10f;
    float hand_icon_offset_x_m = 0.0f;  // along the drawn left hand's own axes
    float hand_icon_offset_y_m = 0.02f;
    float hand_icon_offset_z_m = 0.0f;
    float hand_icon_spin_deg = 0.0f;  // in its own plane, counter-clockwise as seen
    float hand_icon_tilt_deg = 0.0f;
    bool haptics = true;
    wheelcore::Layouts layouts;       // each character's pages
};

inline Config config;
inline unsigned generation = 0;              // loads so far; 0 = not loaded yet
inline std::vector<std::string> warnings;    // the last load's rejections
inline char last_why[64] = "";
inline char ini_path[MAX_PATH] = "";         // psobbvr.ini, as last loaded

// A whole-string decimal number.
inline bool ParseNumber(const std::string& s, double& out) {
    const std::string t = wheelcore::Trim(s);
    if (t.empty())
        return false;
    char* end = nullptr;
    const double v = strtod(t.c_str(), &end);
    if (end == nullptr || *end != '\0')
        return false;
    out = v;
    return true;
}

// One knob line into `c`. A bad value keeps the default, with a warning.
inline void ApplyKnob(Config& c, const wheelcore::KeyValue& kv, std::vector<std::string>& warn) {
    const std::string k = wheelcore::Lower(kv.key);
    double v = 0.0;
    const bool num = ParseNumber(kv.value, v);
    auto bad = [&](const std::string& why) {
        warn.push_back("'" + kv.key + " = " + kv.value + "': " + why + " - keeping the default");
    };
    auto flag = [&](bool& out) {
        if (num)
            out = v != 0.0;
        else
            bad("not a number");
    };
    auto real = [&](float& out, double lo, double hi) {
        if (!num) {
            bad("not a number");
        } else if (v < lo || v > hi) {
            char range[64];
            snprintf(range, sizeof(range), "out of range (%g to %g)", lo, hi);
            bad(range);
        } else {
            out = (float)v;
        }
    };
    if (k == "enabled") {
        flag(c.enabled);
    } else if (k == "swing_hand") {
        int hand = -1;
        for (int i = 0; i < 3; i++)
            if (wheelcore::SameKey(kv.value, kSwingHandKeys[i]))
                hand = i;
        if (hand >= 0)
            c.swing_hand = hand;
        else
            bad("not both, left or right");
    } else if (k == "size_m") {
        real(c.size_m, 0.1, 1.0);
    } else if (k == "ring_raise_m") {
        real(c.ring_raise_m, 0.0, 0.5);
    } else if (k == "ring_toward_head_m") {
        real(c.ring_toward_head_m, -0.3, 0.3);
    } else if (k == "open_delay_ms") {
        float ms = (float)c.open_delay_ms;
        real(ms, 0.0, 1000.0);
        c.open_delay_ms = (int)(ms + 0.5f);
    } else if (k == "hand_icon") {
        flag(c.hand_icon);
    } else if (k == "hand_icon_depth_test") {
        flag(c.hand_icon_depth_test);
    } else if (k == "hand_icon_size_m") {
        real(c.hand_icon_size_m, 0.01, 0.2);
    } else if (k == "hand_icon_offset_x_m") {
        real(c.hand_icon_offset_x_m, -0.3, 0.3);
    } else if (k == "hand_icon_offset_y_m") {
        real(c.hand_icon_offset_y_m, -0.3, 0.3);
    } else if (k == "hand_icon_offset_z_m") {
        real(c.hand_icon_offset_z_m, -0.3, 0.3);
    } else if (k == "hand_icon_spin_deg") {
        real(c.hand_icon_spin_deg, -180.0, 180.0);
    } else if (k == "hand_icon_tilt_deg") {
        real(c.hand_icon_tilt_deg, -180.0, 180.0);
    } else if (k == "haptics") {
        flag(c.haptics);
    } else {
        warn.push_back("'" + kv.key + "': unknown setting - line ignored");
    }
}

// (Re)load the section from `ini_path` (psobbvr.ini; the defaults file is
// beside it) and log a summary and every rejected line.
inline void Load(const char* ini_path, const char* why) {
    // "k=v\0k=v\0\0", psobbvr.ini's lines first. The profile functions
    // return at most 32,767 characters of a section: about forty characters
    // with eight full pages each.
    static char buf[32768];
    const DWORD n = settings::GetSection("wheel", buf, sizeof(buf), ini_path);
    Config c;
    std::vector<std::string> warn;
    std::vector<wheelcore::KeyValue> layout_lines;
    std::vector<std::string> knob_keys;
    for (const char* p = buf; n > 0 && *p != '\0'; p += strlen(p) + 1) {
        const std::string line = wheelcore::Trim(p);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        wheelcore::KeyValue kv;
        if (!wheelcore::SplitLine(line, kv)) {
            warn.push_back("'" + line + "': not key=value - line ignored");
            continue;
        }
        if (wheelcore::IsLayoutKey(kv.key)) {
            layout_lines.push_back(kv);
            continue;
        }
        // A repeated key: the profile functions only ever read the first.
        bool seen = false;
        for (const std::string& k : knob_keys)
            seen = seen || wheelcore::SameKey(kv.key, k.c_str());
        if (seen) {
            warn.push_back("'" + kv.key + "': repeated - the first line is used");
            continue;
        }
        knob_keys.push_back(kv.key);
        ApplyKnob(c, kv, warn);
    }
    wheelcore::ParseLayouts(layout_lines, c.layouts, warn);
    config = c;
    warnings = warn;
    generation++;
    snprintf(last_why, sizeof(last_why), "%s", why);
    snprintf(wheelconfig::ini_path, sizeof(wheelconfig::ini_path), "%s", ini_path);

    std::string pages;
    for (const wheelcore::CharLayout& ch : c.layouts.chars) {
        if (ch.layout.pages == 0)
            continue;
        if (!pages.empty())
            pages += ", ";
        pages += wheelcore::CharPrefix(ch.guild_card, ch.slot);
        pages += " " + std::to_string(ch.layout.pages);
    }
    diag::Log("wheel: loaded (%s): %s, swing hand %s, size %.2f m, open delay %d ms, hand "
              "icon %s, haptics %s; pages: %s; %d warning(s)",
              why, c.enabled ? "on" : "off", kSwingHandKeys[c.swing_hand], c.size_m,
              c.open_delay_ms, c.hand_icon ? "on" : "off", c.haptics ? "on" : "off",
              pages.empty() ? "none set" : pages.c_str(), (int)warnings.size());
    for (const std::string& w : warnings)
        diag::Log("wheel:   %s", w.c_str());
}

}  // namespace wheelconfig
