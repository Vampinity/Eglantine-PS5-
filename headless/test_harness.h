// SPDX-License-Identifier: GPL-3.0-or-later
// Test runs: a computer runs agreed tests on the console with nobody at the controller. env.txt
// chooses all of it (main.cpp reads it when the app starts):
//
//   EDEN_TEST_BOOT=<16 hex digits>  That title starts with no press in the launcher, as a
//                                   development build starts its own, from the same search of
//                                   roms/ (main.cpp): a folder game, or one in a USB drive's
//                                   games/, is not found and the launcher says so. Only with
//                                   /data/eglantineps5/test-boot-once there, which this start
//                                   removes: the computer leaves that file for each start it
//                                   asks for. So an env.txt it could not put back (the console
//                                   froze, and has been restarted) boots nothing, and a title
//                                   that takes the app down is not booted into again however the
//                                   app came back. After a crash or a game that would not stop
//                                   the launcher opens, also with the file there.
//   EDEN_TEST_SCRIPT=<file name>    A file in /data/eglantineps5, played on player 1's pad in
//                                   every session of the booted game (with no EDEN_TEST_BOOT, of
//                                   any game; with one that cannot be read, of none). Format below.
//   EDEN_TEST_RECORD=1              What player 1 does in such a session goes to
//                                   logs/recorded-input.txt in the same format, so a route is
//                                   recorded by playing it; the session before moves to
//                                   logs/recorded-input.prev.txt. A script in env.txt wins.
//   EDEN_TEST_SHOTS=<s>[,<s>...]    One lossless picture of the game at each of these times, in
//                                   the script's seconds (below), to logs/game-frame-<s>.ppm
//                                   (game-frame-95.ppm, game-frame-95.5.ppm): the same frame of
//                                   every build, to compare texture detail. At most kMaxShots,
//                                   the earliest; one time that cannot be read turns them all off.
//                                   logs/game-shots.txt lists this session's files, so a picture
//                                   an earlier run left is not taken for this one. Only in the
//                                   booted game, as a script; the renderer reads the frame back
//                                   and a thread of its own writes the file (TakeShots).
//
// With any of them set, the app looks for /data/eglantineps5/test-quit four times a second. It
// removes the file and closes: a game stops as Touchpad + L1 stops it, a launcher closes, main
// returns. That takes about a second. A stop still going kQuitLimit later ends with
// eden_exit_app instead: the restart of stop_limit.h would only open a launcher nobody asked for.
// A test-quit left from before the app started is removed unread.
//
// With none of them set no thread starts, no file is read or written, and the hooks in main.cpp,
// pad.cpp and frontend.cpp each read a flag that stays false or an optional that stays empty.
//
// A script line is "<seconds> <BUTTON> <hold_ms>" or "<seconds> LSTICK <x> <y> <hold_ms>" (RSTICK
// too); # starts a comment. Seconds count from the guest starting (system.Run in main.cpp), not
// from game_loaded: the shader cache is built between the two and can take minutes. They are wall
// clock, so a route written at one frame rate drifts at another. x and y are the pad's own 0-255:
// 128 is centre, 0 is left and up. BUTTON is a DualSense button (kButtons below) or a game button
// (kGameNames below, found through the game's button mapping). Presses may overlap, and for a
// stick the one that started last counts. A line that cannot be read stops the whole script: a
// route with a press missing would run on and report nonsense. EDEN_TEST lines in stderr.log say
// what happened.
//
// The script goes through Pad::Consume as the controller's samples do, so the mapping, the grips
// and the Touchpad + L1 shortcut all apply. While a press is held the controller is not read;
// between presses it is, so someone can step in.
//
// A script can wait for the screen instead of guessing a time:
//
//   <seconds> WAIT <timeout_s> LOOK <x> <y> <w> <h> <cols>x<rows>:<hex> [<min>]
//   <seconds> WAIT <timeout_s> GONE <x> <y> <w> <h> <cols>x<rows>:<hex> [<min>]
//   <seconds> MAYBE ...                        the same, but a timeout goes on instead of stopping
//   <seconds> END                              the script is done here
//
// The lines between two WAITs are a section, and a section's seconds count from the moment the
// WAIT before it was met (the first section's from the guest starting, as above). A WAIT starts
// looking at its own seconds. LOOK is met when the region looks like the grid, GONE when it no
// longer does. Then the next section starts at once: lines of the WAIT's section that have not
// started are dropped and presses still held are let go. So "1 A, 4 A, then WAIT for the next
// screen" presses A again only while that screen has not come, and "LSTICK up for 4 s, WAIT for a
// prompt" runs until the prompt shows. A WAIT that times out stops the script: nothing more is
// pressed, so a screen that came late or never cannot take a press meant for another one.
//
// The region is x, y, w, h in per mille of the picture, cut into cols x rows cells; hex is each
// cell's mean brightness, two digits a cell, row by row (make-look.py cuts one from a picture).
// The app reads the game's frame back as EDEN_TEST_SHOTS does, but into memory, every kLookEvery
// while a WAIT is looking and never otherwise, and scores the region's cells against the grid by
// correlation, 0 to 100: brightness and contrast do not count, the shapes do. min is the score a
// LOOK needs (default 85). EDEN_TEST wait_met, wait_timeout and wait_skipped lines say when, the
// best score and, on a timeout, the grid that was seen. A look and a shot share the renderer's one
// screenshot slot: one asked for while the other is pending is dropped (the look tries again).
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include "button_mapping.h"
#include "diagnostics.h"
#include "ps5_pad.hpp"
#include "storage_paths.h"

#ifdef PS5_NATIVE
extern "C" int eden_exit_app(void);  // src/lifecycle.c
#endif

namespace Eden::TestHarness {
using Clock = std::chrono::steady_clock;
using Ms = std::uint64_t;

inline constexpr std::chrono::milliseconds kQuitPoll{250};
inline constexpr std::chrono::milliseconds kQuitLimit{3000};
inline constexpr std::string_view kQuitFile = "test-quit";
inline constexpr std::string_view kBootOnceFile = "test-boot-once";
inline constexpr std::string_view kRecordFile = "recorded-input.txt";
inline constexpr std::string_view kRecordPreviousFile = "recorded-input.prev.txt";
inline constexpr std::size_t kMaxShots = 12;
inline constexpr std::string_view kShotListFile = "game-shots.txt";
// How long a shot waits for its file before the next one is due anyway.
inline constexpr std::chrono::seconds kShotWait{10};
// How often a WAIT asks for a picture. Each one is a GPU readback of the whole frame.
inline constexpr std::uint64_t kLookEvery = 500;  // ms
inline constexpr unsigned kMaxCells = 256;  // so a timeout's seen= grid fits a Report line (900 characters)

struct NamedButton {
    std::string_view name;
    ps5::pad::ButtonMask mask;
};
// In the order of PadButton (button_mapping.h), then the arrows.
inline constexpr NamedButton kButtons[] = {
    {"CROSS", ps5::pad::kButtonCross},     {"CIRCLE", ps5::pad::kButtonCircle},
    {"SQUARE", ps5::pad::kButtonSquare},   {"TRIANGLE", ps5::pad::kButtonTriangle},
    {"L1", ps5::pad::kButtonL1},           {"R1", ps5::pad::kButtonR1},
    {"L2", ps5::pad::kButtonL2},           {"R2", ps5::pad::kButtonR2},
    {"L3", ps5::pad::kButtonL3},           {"R3", ps5::pad::kButtonR3},
    {"OPTIONS", ps5::pad::kButtonOptions}, {"CREATE", ps5::pad::kButtonCreate},
    {"TOUCHPAD", ps5::pad::kButtonTouchPad},
    {"UP", ps5::pad::kButtonUp},           {"DOWN", ps5::pad::kButtonDown},
    {"LEFT", ps5::pad::kButtonLeft},       {"RIGHT", ps5::pad::kButtonRight},
};
static_assert(std::size(kButtons) == kPadButtons + 4 && pad_touchpad + 1 == kPadButtons);
// In the order of GameButton.
inline constexpr std::string_view kGameNames[kGameButtons] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT_STICK", "RIGHT_STICK"};

inline std::optional<ps5::pad::ButtonMask> ButtonNamed(std::string_view name, const ButtonMapping& mapping) {
    for (const NamedButton& button : kButtons)
        if (button.name == name) return button.mask;
    for (int game = 0; game < kGameButtons; ++game)
        if (kGameNames[game] == name) return kButtons[mapping[game]].mask;
    return {};
}

enum class StickSide : std::uint8_t { none, left, right };
struct Event {
    Ms start = 0, end = 0;
    ps5::pad::ButtonMask buttons = 0;
    StickSide stick = StickSide::none;
    std::uint8_t x = 128, y = 128;
    unsigned section = 0;  // the WAITs before it in the script
};

// Plain digits, by hand: a script's numbers need no locale, and the console's C library is
// asked for nothing new.
inline std::optional<std::uint64_t> Number(std::string_view text, std::uint64_t most) {
    if (text.empty()) return {};
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return {};
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (value > most) return {};
    }
    return value;
}
// "12", "12.5" or ".25" as milliseconds; digits past the third are dropped.
inline std::optional<Ms> Seconds(std::string_view text) {
    Ms whole = 0, fraction = 0;
    unsigned fraction_digits = 0;
    bool dot = false, digit = false;
    for (const char c : text) {
        if (c == '.' && !dot) {
            dot = true;
            continue;
        }
        if (c < '0' || c > '9') return {};
        digit = true;
        const auto value = static_cast<Ms>(c - '0');
        if (!dot) {
            whole = whole * 10 + value;
            if (whole > 10'000'000) return {};
        } else if (fraction_digits < 3) {
            fraction = fraction * 10 + value;
            ++fraction_digits;
        }
    }
    if (!digit) return {};
    for (; fraction_digits < 3; ++fraction_digits) fraction *= 10;
    return whole * 1000 + fraction;
}
inline std::string SecondsText(Ms ms) {
    std::string fraction = std::to_string(ms % 1000);
    return std::to_string(ms / 1000) + '.' + std::string(3 - fraction.size(), '0') + fraction;
}

// EDEN_TEST_SHOTS's times: sorted, each once, the earliest kMaxShots. error is set when an item
// cannot be read (then there are none) or when some were dropped (the rest stay).
struct ShotList {
    std::vector<Ms> times;
    std::string error;
};
inline ShotList ParseShots(std::string_view text) {
    ShotList list;
    for (std::size_t start = 0;;) {
        const auto comma = text.find(',', start);
        std::string_view item = text.substr(start, comma == text.npos ? text.npos : comma - start);
        const auto first = item.find_first_not_of(" \t\r\n");
        item = first == item.npos ? std::string_view{} : item.substr(first, item.find_last_not_of(" \t\r\n") - first + 1);
        const auto at = Seconds(item);
        if (!at) {
            list.times.clear();
            list.error = "bad time '" + std::string{item} + "'";
            return list;
        }
        list.times.push_back(*at);
        if (comma == text.npos) break;
        start = comma + 1;
    }
    std::sort(list.times.begin(), list.times.end());
    list.times.erase(std::unique(list.times.begin(), list.times.end()), list.times.end());
    if (list.times.size() > kMaxShots) {
        list.error = "kept the first " + std::to_string(kMaxShots) + " of " + std::to_string(list.times.size());
        list.times.resize(kMaxShots);
    }
    return list;
}
// game-frame-95.ppm, game-frame-95.5.ppm: the time as written, without trailing zeros.
inline std::string ShotName(Ms at) {
    std::string text = SecondsText(at);
    while (text.back() == '0') text.pop_back();
    if (text.back() == '.') text.pop_back();
    return "game-frame-" + text + ".ppm";
}

// One line of a script, added to events. False with error set when it cannot be read.
inline bool ParseLine(std::string_view line, const ButtonMapping& mapping, std::vector<Event>& events,
                      std::string& error) {
    std::istringstream fields{std::string{line.substr(0, line.find('#'))}};
    std::vector<std::string> words;
    for (std::string word; fields >> word;) words.push_back(std::move(word));
    if (words.empty()) return true;
    const bool stick = words.size() > 1 && (words[1] == "LSTICK" || words[1] == "RSTICK");
    if (words.size() != (stick ? 5u : 3u)) {
        error = "wrong number of fields";
        return false;
    }
    const auto start = Seconds(words[0]);
    const auto hold = Number(words.back(), 86'400'000);
    if (!start || !hold || *hold == 0) {
        error = !start ? "bad time" : "bad hold";
        return false;
    }
    Event event;
    event.start = *start;
    event.end = *start + *hold;
    if (stick) {
        const auto x = Number(words[2], 255), y = Number(words[3], 255);
        if (!x || !y) {
            error = "stick position out of 0-255";
            return false;
        }
        event.stick = words[1] == "LSTICK" ? StickSide::left : StickSide::right;
        event.x = static_cast<std::uint8_t>(*x);
        event.y = static_cast<std::uint8_t>(*y);
    } else {
        const auto mask = ButtonNamed(words[1], mapping);
        if (!mask) {
            error = "unknown button " + words[1];
            return false;
        }
        event.buttons = *mask;
    }
    events.push_back(event);
    return true;
}

// The game's frame as the renderer reads it back: 4 bytes a pixel in B, G, R, A order, the last
// row first when flip is set (graphics.cpp's CaptureNextFrame writes the same bytes to a file).
struct Picture {
    unsigned width = 0, height = 0;
    bool flip = false;
    std::vector<unsigned char> bgra;
};
using PictureFuture = std::shared_future<std::shared_ptr<const Picture>>;
// Asks the renderer for its next frame (main.cpp). Empty when there is none to ask.
using LookFunction = std::function<PictureFuture()>;

struct Look {
    unsigned x = 0, y = 0, w = 0, h = 0;  // per mille of the picture's width and height
    unsigned cols = 0, rows = 0;
    std::vector<std::uint8_t> grid;  // cols * rows mean brightnesses, row by row
    int min = 85;                    // the score a LOOK needs
    bool gone = false;               // GONE: met when the score is below min
};
struct Wait {
    Ms at = 0, timeout = 0;  // in its section's seconds; the timeout counts from at
    bool end = false;        // END
    bool maybe = false;      // MAYBE: a timeout goes on to the next section
    Look look;
    unsigned line = 0;
};

// The mean brightness, (29 B + 150 G + 77 R) / 256, of each of the look's cells in this picture,
// row by row (make-look.py does the same sums). Empty when the picture has no pixels.
inline std::vector<std::uint8_t> GridOf(const Picture& picture, const Look& look) {
    const std::uint64_t width = picture.width, height = picture.height;
    if (!width || !height || picture.bgra.size() < width * height * 4 || !look.cols || !look.rows) return {};
    const std::uint64_t x0 = look.x * width / 1000, y0 = look.y * height / 1000;
    const std::uint64_t x1 = std::max(x0 + 1, (look.x + look.w) * width / 1000);
    const std::uint64_t y1 = std::max(y0 + 1, (look.y + look.h) * height / 1000);
    std::vector<std::uint8_t> grid;
    grid.reserve(std::size_t{look.cols} * look.rows);
    for (unsigned row = 0; row < look.rows; ++row) {
        const std::uint64_t ya = y0 + (y1 - y0) * row / look.rows;
        const std::uint64_t yb = std::min(height, std::max(ya + 1, y0 + (y1 - y0) * (row + 1) / look.rows));
        for (unsigned col = 0; col < look.cols; ++col) {
            const std::uint64_t xa = x0 + (x1 - x0) * col / look.cols;
            const std::uint64_t xb = std::min(width, std::max(xa + 1, x0 + (x1 - x0) * (col + 1) / look.cols));
            std::uint64_t total = 0, count = 0;
            for (std::uint64_t y = ya; y < yb; ++y) {
                const std::uint64_t source = picture.flip ? height - 1 - y : y;
                const unsigned char* pixel = picture.bgra.data() + (source * width + xa) * 4;
                for (std::uint64_t x = xa; x < xb; ++x, pixel += 4)
                    total += 29u * pixel[0] + 150u * pixel[1] + 77u * pixel[2];
                count += xb - xa;
            }
            grid.push_back(static_cast<std::uint8_t>(total / (256 * std::max<std::uint64_t>(count, 1))));
        }
    }
    return grid;
}

// How alike two grids are: their correlation in percent, -100 to 100. Brightness and contrast
// do not count. A grid with no shape at all (one brightness) scores 0.
inline int Score(const std::vector<std::uint8_t>& want, const std::vector<std::uint8_t>& seen) {
    if (want.empty() || want.size() != seen.size()) return 0;
    double want_mean = 0, seen_mean = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        want_mean += want[i];
        seen_mean += seen[i];
    }
    want_mean /= static_cast<double>(want.size());
    seen_mean /= static_cast<double>(seen.size());
    double both = 0, want_spread = 0, seen_spread = 0;
    for (std::size_t i = 0; i < want.size(); ++i) {
        const double a = want[i] - want_mean, b = seen[i] - seen_mean;
        both += a * b;
        want_spread += a * a;
        seen_spread += b * b;
    }
    if (want_spread <= 0 || seen_spread <= 0) return 0;
    return static_cast<int>(std::lround(100 * both / std::sqrt(want_spread * seen_spread)));
}

inline std::string GridText(const Look& look, const std::vector<std::uint8_t>& grid) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text = std::to_string(look.cols) + 'x' + std::to_string(look.rows) + ':';
    for (const std::uint8_t cell : grid) {
        text += kDigits[cell >> 4];
        text += kDigits[cell & 15];
    }
    return text;
}

// A WAIT, MAYBE or END line, as its words. False with error set when it cannot be read.
inline bool ParseWait(const std::vector<std::string>& words, Wait& wait, std::string& error) {
    const auto at = Seconds(words[0]);
    if (!at) {
        error = "bad time";
        return false;
    }
    wait.at = *at;
    if (words[1] == "END") {
        wait.end = true;
        if (words.size() == 2) return true;
        error = "wrong number of fields";
        return false;
    }
    wait.maybe = words[1] == "MAYBE";
    if (words.size() != 9 && words.size() != 10) {
        error = "wrong number of fields";
        return false;
    }
    const auto timeout = Seconds(words[2]);
    if (!timeout || *timeout == 0) {
        error = "bad timeout";
        return false;
    }
    wait.timeout = *timeout;
    if (words[3] != "LOOK" && words[3] != "GONE") {
        error = "unknown condition " + words[3];
        return false;
    }
    Look& look = wait.look;
    look.gone = words[3] == "GONE";
    const auto x = Number(words[4], 1000), y = Number(words[5], 1000);
    const auto w = Number(words[6], 1000), h = Number(words[7], 1000);
    if (!x || !y || !w || !h || *w == 0 || *h == 0 || *x + *w > 1000 || *y + *h > 1000) {
        error = "region outside 0-1000";
        return false;
    }
    look.x = static_cast<unsigned>(*x);
    look.y = static_cast<unsigned>(*y);
    look.w = static_cast<unsigned>(*w);
    look.h = static_cast<unsigned>(*h);
    const std::string_view grid{words[8]};
    const auto cross = grid.find('x'), colon = grid.find(':');
    if (cross == grid.npos || colon == grid.npos || colon < cross) {
        error = "bad grid, not <cols>x<rows>:<hex>";
        return false;
    }
    const auto cols = Number(grid.substr(0, cross), 64), rows = Number(grid.substr(cross + 1, colon - cross - 1), 64);
    if (!cols || !rows || *cols == 0 || *rows == 0 || *cols * *rows > kMaxCells) {
        error = "bad grid size";
        return false;
    }
    look.cols = static_cast<unsigned>(*cols);
    look.rows = static_cast<unsigned>(*rows);
    const std::string_view hex = grid.substr(colon + 1);
    if (hex.size() != 2 * *cols * *rows) {
        error = "the grid needs " + std::to_string(2 * *cols * *rows) + " hex digits";
        return false;
    }
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    double mean = 0;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = digit(hex[i]), low = digit(hex[i + 1]);
        if (high < 0 || low < 0) {
            error = "bad hex digit";
            return false;
        }
        look.grid.push_back(static_cast<std::uint8_t>(high * 16 + low));
        mean += look.grid.back();
    }
    mean /= static_cast<double>(look.grid.size());
    double spread = 0;
    for (const std::uint8_t cell : look.grid) spread += (cell - mean) * (cell - mean);
    if (spread / static_cast<double>(look.grid.size()) < 4) {
        error = "flat grid: nothing to recognise";
        return false;
    }
    if (words.size() == 10) {
        const auto min = Number(words[9], 100);
        if (!min || *min == 0) {
            error = "bad min, 1-100";
            return false;
        }
        look.min = static_cast<int>(*min);
    }
    return true;
}

struct Script {
    std::vector<Event> events;  // by section, then start time; a recording's lines are by end time
    std::vector<Wait> waits;    // waits[i] ends section i
    std::string error;          // not empty: the script must not be played
};
inline Script ParseScript(std::istream& input, const ButtonMapping& mapping) {
    Script script;
    unsigned number = 0;
    bool ended = false;
    for (std::string line; std::getline(input, line);) {
        ++number;
        std::istringstream fields{line.substr(0, line.find('#'))};
        std::vector<std::string> words;
        for (std::string word; fields >> word;) words.push_back(std::move(word));
        if (words.empty()) continue;
        bool ok = !ended;
        if (!ok) {
            script.error = "a line after END";
        } else if (words.size() > 1 && (words[1] == "WAIT" || words[1] == "MAYBE" || words[1] == "END")) {
            Wait wait;
            wait.line = number;
            ok = ParseWait(words, wait, script.error);
            if (ok) {
                ended = wait.end;
                script.waits.push_back(std::move(wait));
            }
        } else if ((ok = ParseLine(line, mapping, script.events, script.error))) {
            script.events.back().section = static_cast<unsigned>(script.waits.size());
        }
        if (!ok) {
            script.error = "line " + std::to_string(number) + ": " + script.error;
            script.events.clear();
            script.waits.clear();
            return script;
        }
    }
    std::stable_sort(script.events.begin(), script.events.end(), [](const Event& a, const Event& b) {
        return a.section != b.section ? a.section < b.section : a.start < b.start;
    });
    return script;
}

// The pad state a script asks for at a moment.
class Player {
public:
    explicit Player(std::vector<Event> script, std::vector<Wait> wait_lines = {})
        : events{std::move(script)}, waits{std::move(wait_lines)} {}
    // Empty while the script presses nothing: the controller plays then. A press is seen at least
    // once however short it is and however late the caller comes, and one neutral sample follows
    // the last press so that the controller's own state takes over from nothing held.
    std::optional<ps5::pad::Data> Sample(Ms now) {
        for (; next < events.size() && events[next].section == section && zero + events[next].start <= now; ++next) {
            Event event = events[next];
            event.start += zero;
            event.end = std::max(event.end + zero, now + 1);
            active.push_back(event);
        }
        std::erase_if(active, [now](const Event& event) { return event.end <= now; });
        if (active.empty()) {
            if (!held) return {};
            held = false;
            return Neutral();
        }
        held = true;
        auto sample = Neutral();
        for (const Event& event : active) {
            sample.buttons |= event.buttons;
            if (event.stick == StickSide::left) sample.left_stick = {event.x, event.y};
            if (event.stick == StickSide::right) sample.right_stick = {event.x, event.y};
        }
        return sample;
    }
    // The WAIT or END of the section that plays, once its seconds have come. Null otherwise.
    const Wait* Waiting(Ms now) const {
        if (section >= waits.size() || now < zero + waits[section].at) return nullptr;
        return &waits[section];
    }
    bool TimedOut(Ms now) const {
        return section < waits.size() && now >= zero + waits[section].at + waits[section].timeout;
    }
    // The WAIT was met: the next section starts now. What is left of this one is dropped and what
    // it holds is let go (one neutral sample follows).
    void Pass(Ms now) {
        while (next < events.size() && events[next].section <= section) ++next;
        active.clear();
        ++section;
        zero = now;
    }
    // Nothing more is pressed: END, or a WAIT that timed out.
    void Stop() {
        next = events.size();
        active.clear();
        section = static_cast<unsigned>(waits.size());
    }
    bool Done() const { return next == events.size() && active.empty() && !held && section >= waits.size(); }
    std::size_t Size() const { return events.size(); }
    std::size_t WaitCount() const { return waits.size(); }

private:
    static ps5::pad::Data Neutral() {
        auto sample = ps5::pad::neutral_data();
        sample.connected = 1;
        return sample;
    }
    std::vector<Event> events;
    std::vector<Wait> waits;
    std::size_t next = 0;
    unsigned section = 0;  // the WAITs met so far
    Ms zero = 0;           // when this section started
    std::vector<Event> active;
    bool held = false;
};

// A controller's samples as a script's lines. A press is a line when it ends. A stick is a line
// per position it stayed near: a new one when it moved kStickStep or more, at most every
// kStickMs, and always when it returns to centre (kStickDead is Pad's dead zone). A press still
// held when the recording ends has no line: that drops the Touchpad + L1 that ended the session.
class Recorder {
public:
    static constexpr int kStickStep = 6;
    static constexpr int kStickDead = 10;
    static constexpr Ms kStickMs = 50;

    std::string text;  // lines not yet written out

    void Take(const ps5::pad::Data& raw, Ms now) {
        const bool usable = ps5::pad::is_usable(raw);
        ps5::pad::ButtonMask down = usable ? raw.buttons : 0;
        if (usable && raw.triggers.l2 >= 128) down |= ps5::pad::kButtonL2;
        if (usable && raw.triggers.r2 >= 128) down |= ps5::pad::kButtonR2;
        for (std::size_t i = 0; i < std::size(kButtons); ++i) {
            const auto mask = kButtons[i].mask;
            const bool is_down = (down & mask) != 0, was_down = (held & mask) != 0;
            if (is_down && !was_down) since[i] = now;
            if (!is_down && was_down) text += Press(since[i], kButtons[i].name, now);
        }
        held = down;
        TakeStick(left, "LSTICK", usable ? raw.left_stick : ps5::pad::Stick{128, 128}, now);
        TakeStick(right, "RSTICK", usable ? raw.right_stick : ps5::pad::Stick{128, 128}, now);
    }

private:
    struct Position {
        std::uint8_t x = 128, y = 128;
        Ms since = 0;
        bool Centred() const { return x == 128 && y == 128; }
    };
    static std::string Press(Ms start, std::string_view name, Ms now) {
        return SecondsText(start) + ' ' + std::string{name} + ' ' + std::to_string(std::max<Ms>(now - start, 1)) + '\n';
    }
    static std::uint8_t Settled(std::uint8_t value) {
        return std::abs(int{value} - 128) <= kStickDead ? std::uint8_t{128} : value;
    }
    void TakeStick(Position& at, std::string_view name, ps5::pad::Stick value, Ms now) {
        const Position next{Settled(value.x), Settled(value.y), now};
        if (next.x == at.x && next.y == at.y) return;
        const int moved = std::max(std::abs(int{next.x} - at.x), std::abs(int{next.y} - at.y));
        if (!next.Centred() && !at.Centred() && (moved < kStickStep || now - at.since < kStickMs)) return;
        if (!at.Centred())
            text += SecondsText(at.since) + ' ' + std::string{name} + ' ' + std::to_string(at.x) + ' ' +
                    std::to_string(at.y) + ' ' + std::to_string(std::max<Ms>(now - at.since, 1)) + '\n';
        at = next;
    }
    ps5::pad::ButtonMask held = 0;
    std::array<Ms, std::size(kButtons)> since{};
    Position left, right;
};

inline bool ValidTitle(std::string_view id) {
    return id.size() == 16 && id.find_first_not_of("0123456789abcdefABCDEF") == id.npos;
}
inline bool ValidFileName(std::string_view name) {
    return !name.empty() && name != "." && name != ".." && name.find('/') == name.npos;
}

namespace detail {
struct Settings {
    std::string boot, script;
    bool record = false;
    std::vector<Ms> shots;  // EDEN_TEST_SHOTS, read by Start
};
inline Settings settings;  // set by Start, before any game
inline bool boot_armed = false;  // Start found kBootOnceFile
inline bool booted = false;      // this process started the title by itself (MarkBooted)
inline std::atomic<bool> quit{false};

// Pad::Consume and Session are both the input worker, so nothing here locks; recording_on keeps
// Consume's other callers out.
struct RecordState {
    Recorder recorder;
    std::FILE* file = nullptr;
    Clock::time_point origin;
    Ms written = 0;
};
inline RecordState record_state;
inline std::atomic<bool> recording_on{false};

inline Ms Since(Clock::time_point origin) {
    return static_cast<Ms>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - origin).count());
}
// A console storage write takes about 25 ms (log_flusher.h): the input worker does one a second,
// and only while recording. A crash loses at most that second of the route.
inline void WriteRecorded() {
    std::string& text = record_state.recorder.text;
    if (record_state.file && !text.empty()) {
        std::fwrite(text.data(), 1, text.size(), record_state.file);
        std::fflush(record_state.file);
    }
    text.clear();
}
inline std::string Trimmed(const char* name) {
    const char* const value = std::getenv(name);
    std::string text = value ? value : "";
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == text.npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
} // namespace detail

// test-quit was seen: everything that can should close. Stays set.
inline bool QuitAsked() { return detail::quit.load(std::memory_order_relaxed); }
// The title of EDEN_TEST_BOOT, or nothing.
inline const std::string& BootTitle() { return detail::settings.boot; }
// The computer asked for this start (test-boot-once). Without that the title is not booted.
inline bool BootArmed() { return detail::boot_armed; }
// main.cpp is booting the title: from here on this process's sessions of it are test sessions.
inline void MarkBooted() { detail::booted = true; }

// Is this game the one a script or a recording is for? With EDEN_TEST_BOOT, only the title this
// process booted, by main.cpp's own two ways of finding it: its id in the file name, or in the
// game's metadata. Anything else someone starts from the launcher gets no press from a script.
inline bool ForGame(std::string_view boot, bool booted, const char* game, std::uint64_t title) {
    if (boot.empty()) return true;
    if (!booted) return false;
    const auto wanted = std::strtoull(std::string{boot}.c_str(), nullptr, 16);
    return title == wanted || (game && std::string_view{game}.find(boot) != std::string_view::npos);
}

// Waits for the file, removes it, and gives a clean close `limit` to finish. No stdio: this
// thread is what ends a stop that is stuck, perhaps holding a stdio lock.
inline void Watch(const std::string& file, std::chrono::milliseconds poll, std::chrono::milliseconds limit,
                  void (*at_limit)() noexcept) {
    while (std::remove(file.c_str()) != 0) std::this_thread::sleep_for(poll);
    detail::quit.store(true, std::memory_order_relaxed);
    std::this_thread::sleep_for(limit);
    at_limit();
}
inline void ExitNow() noexcept {
#ifdef PS5_NATIVE
    (void)eden_exit_app();  // returns only when the system refuses
#endif
}

// What env.txt's values come to. A title that cannot be read turns the script and the recording
// off: with no title left to hold them to, they would run in any game someone opens.
inline detail::Settings ReadSettings(const std::string& boot, const std::string& script, bool record) {
    detail::Settings settings;
    if (!boot.empty() && !ValidTitle(boot)) {
        Report("test", ("EDEN_TEST bad_boot value=" + boot).c_str());
        return settings;
    }
    settings.boot = boot;
    if (ValidFileName(script)) settings.script = script;
    else if (!script.empty()) Report("test", ("EDEN_TEST bad_script value=" + script).c_str());
    settings.record = record;
    return settings;
}

// Once, after env.txt was read (main.cpp). With no EDEN_TEST_ setting it does nothing.
inline void Start() {
    const std::string boot = detail::Trimmed("EDEN_TEST_BOOT"), script = detail::Trimmed("EDEN_TEST_SCRIPT");
    const bool record = detail::Trimmed("EDEN_TEST_RECORD") == "1";
    const std::string shots = detail::Trimmed("EDEN_TEST_SHOTS");
    if (boot.empty() && script.empty() && !record && shots.empty()) return;
    detail::settings = ReadSettings(boot, script, record);
    Report("test", ("EDEN_TEST on boot=" + detail::settings.boot + " script=" + detail::settings.script +
                    " record=" + (detail::settings.record ? "1" : "0")).c_str());
    // A title that could not be read turns the shots off too (ReadSettings).
    if (!shots.empty() && (boot.empty() || !detail::settings.boot.empty())) {
        ShotList list = ParseShots(shots);
        if (!list.error.empty())
            Report("test", ((list.times.empty() ? "EDEN_TEST bad_shots value=" : "EDEN_TEST shots_trimmed value=") +
                            shots + " " + list.error).c_str());
        detail::settings.shots = std::move(list.times);
        std::string times;
        for (const Ms at : detail::settings.shots) times += (times.empty() ? "" : ",") + SecondsText(at);
        if (!times.empty()) Report("test", ("EDEN_TEST shots times=" + times).c_str());
    }
    const std::string file = std::string{kDataDir} + "/" + std::string{kQuitFile};
    if (std::remove(file.c_str()) == 0) Report("test", "EDEN_TEST stale_quit_removed");
    detail::boot_armed = std::remove((std::string{kDataDir} + "/" + std::string{kBootOnceFile}).c_str()) == 0;
    try {
        std::thread([file] { Watch(file, kQuitPoll, kQuitLimit, &ExitNow); }).detach();
    } catch (...) {
        Report("test", "EDEN_TEST no_quit_watcher");
    }
}

inline bool Recording() { return detail::recording_on.load(std::memory_order_relaxed); }
// A sample of player 1's, from Pad::Consume.
inline void Record(const ps5::pad::Data& raw) {
    auto& state = detail::record_state;
    const Ms now = detail::Since(state.origin);
    state.recorder.Take(raw, now);
    if (now - state.written >= 1000) {
        state.written = now;
        detail::WriteRecorded();
    }
}

// One game session's script or recording, from the input worker. start is when the guest started;
// game and title are that session's ROM path and title id; look asks for a picture of the game for
// the script's WAIT lines (empty: a WAIT stops the script at its first look).
class Session {
public:
    Session(const ButtonMapping& mapping, Clock::time_point start, const char* game, std::uint64_t title,
            LookFunction look_function = {})
        : origin{start}, look{std::move(look_function)} {
        if (detail::settings.script.empty() && !detail::settings.record) return;
        if (!ForGame(detail::settings.boot, detail::booted, game, title)) {
            Report("test", "EDEN_TEST session_skipped reason=not_the_booted_game");
            return;
        }
        if (!detail::settings.script.empty()) {
            if (detail::settings.record) Report("test", "EDEN_TEST record_off reason=script");
            LoadScript(mapping);
        } else if (detail::settings.record) {
            BeginRecording();
        }
    }
    ~Session() {
        if (!detail::recording_on.exchange(false)) return;
        auto& state = detail::record_state;
        const Ms now = detail::Since(state.origin);
        state.recorder.text += "# ended at " + SecondsText(now) + "\n";
        detail::WriteRecorded();
        if (state.file) std::fclose(state.file);
        state.file = nullptr;
        Report("test", ("EDEN_TEST record_end at=" + SecondsText(now)).c_str());
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // What the script presses now. Empty without a script, and between its presses.
    std::optional<ps5::pad::Data> Sample() {
        if (!player) return {};
        const Ms now = detail::Since(origin);
        if (const Wait* wait = player->Waiting(now)) Check(*wait, now);
        auto sample = player->Sample(now);
        if (!finished && player->Done()) {
            finished = true;
            // script_stopped: a WAIT timed out, so the route did not run to its end.
            Report("test", ((stopped ? "EDEN_TEST script_stopped at=" : "EDEN_TEST script_done at=") +
                            SecondsText(now)).c_str());
        }
        return sample;
    }

private:
    // One step of a WAIT, on every Sample while it looks: score a picture that came, give up at
    // the timeout, ask for the next picture. The input worker only asks; the renderer reads the
    // frame back on its own thread and the future turns ready a frame or two later.
    void Check(const Wait& wait, Ms now) {
        const auto where = [&] { return "line=" + std::to_string(wait.line) + " at=" + SecondsText(now); };
        if (wait.end) {
            Report("test", ("EDEN_TEST script_end " + where()).c_str());
            player->Stop();
            return;
        }
        if (pending.valid() && pending.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            std::shared_ptr<const Picture> picture;
            try {
                picture = pending.get();
            } catch (...) {  // dropped: the renderer had a picture pending already (a shot)
            }
            pending = {};
            if (picture) seen = GridOf(*picture, wait.look);
            if (picture && !seen.empty()) {
                const int score = Score(wait.look.grid, seen);
                ++looks;
                best = std::max(best, score);
                if (wait.look.gone ? score < wait.look.min : score >= wait.look.min) {
                    Report("test", ("EDEN_TEST wait_met " + where() + " score=" + std::to_string(score) +
                                    " looks=" + std::to_string(looks)).c_str());
                    player->Pass(now);
                    Reset(now);
                    return;
                }
            }
        }
        if (player->TimedOut(now)) {
            Report("test", (std::string(wait.maybe ? "EDEN_TEST wait_skipped " : "EDEN_TEST wait_timeout ") + where() +
                            " best=" + std::to_string(best) + " looks=" + std::to_string(looks) +
                            " seen=" + GridText(wait.look, seen)).c_str());
            if (wait.maybe) {
                player->Pass(now);
            } else {
                stopped = true;
                player->Stop();
            }
            Reset(now);
            return;
        }
        if (pending.valid() || now < next_look) return;
        next_look = now + kLookEvery;
        if (!look) {
            Report("test", ("EDEN_TEST wait_error " + where() + " reason=no_pictures").c_str());
            stopped = true;
            player->Stop();
            return;
        }
        try {
            pending = look();
        } catch (...) {
            pending = {};
        }
    }
    // A new WAIT: a picture still on its way was asked for the last one and is not used.
    void Reset(Ms now) {
        pending = {};
        seen.clear();
        looks = 0;
        best = -100;
        next_look = now;
    }

    void LoadScript(const ButtonMapping& mapping) {
        const std::string& name = detail::settings.script;
        std::ifstream file(std::string{kDataDir} + "/" + name);
        if (!file) {
            Report("test", ("EDEN_TEST script_error file=" + name + " not found").c_str());
            return;
        }
        Script script = ParseScript(file, mapping);
        if (!script.error.empty()) {
            Report("test", ("EDEN_TEST script_error file=" + name + " " + script.error).c_str());
            return;
        }
        player.emplace(std::move(script.events), std::move(script.waits));
        Report("test", ("EDEN_TEST script_start file=" + name + " events=" + std::to_string(player->Size()) +
                        " waits=" + std::to_string(player->WaitCount())).c_str());
    }
    void BeginRecording() {
        const std::string path = LogFile(kRecordFile);
        (void)std::rename(path.c_str(), LogFile(kRecordPreviousFile).c_str());
        auto& state = detail::record_state;
        state.recorder = {};
        state.origin = origin;
        state.written = 0;
        state.file = std::fopen(path.c_str(), "w");
        if (!state.file) {
            Report("test", ("EDEN_TEST record_error file=" + path).c_str());
            return;
        }
        state.recorder.text = "# EDEN_TEST recorded input: <seconds> <BUTTON> <hold_ms>, <seconds> LSTICK <x> <y> <hold_ms>\n";
        detail::recording_on.store(true);
        Report("test", ("EDEN_TEST record_start file=" + path).c_str());
    }
    Clock::time_point origin;
    LookFunction look;
    std::optional<Player> player;
    bool finished = false;
    bool stopped = false;  // a WAIT timed out
    PictureFuture pending;
    std::vector<std::uint8_t> seen;  // the last picture's grid, for the timeout's report
    unsigned looks = 0;
    int best = -100;
    Ms next_look = 0;
};

// EDEN_TEST_SHOTS's times for this session: empty without the setting, and in any game but the
// booted one (ForGame).
inline std::vector<Ms> ShotsFor(const char* game, std::uint64_t title) {
    if (detail::settings.shots.empty()) return {};
    if (!ForGame(detail::settings.boot, detail::booted, game, title)) {
        Report("test", "EDEN_TEST shots_skipped reason=not_the_booted_game");
        return {};
    }
    return detail::settings.shots;
}

// One session's shots, on a thread of their own (main.cpp). At each time from start,
// capture(name) asks the renderer for its next frame and returns a std::shared_future<bool> that
// turns true when logs/<name> is written. The renderer reads the frame back on the GPU thread and
// gives the bytes to a new thread to write (VideoCore::RendererBase::RequestScreenshot), so the
// file never holds up a frame. One request at a time: the renderer drops a second while one is
// pending, and that one then reports ok=0. stopped() ends the wait between shots and for a file.
template <typename Stopped, typename Capture>
void TakeShots(Stopped&& stopped, Clock::time_point start, const std::vector<Ms>& shots, Capture&& capture) {
    // This session's list first, and none of its names left from an earlier run.
    if (std::FILE* list = std::fopen(LogFile(kShotListFile).c_str(), "w")) {
        for (const Ms at : shots) {
            (void)std::remove(LogFile(ShotName(at)).c_str());
            std::fprintf(list, "%s %s\n", SecondsText(at).c_str(), ShotName(at).c_str());
        }
        std::fclose(list);
    }
    Report("test", ("EDEN_TEST shots_start count=" + std::to_string(shots.size())).c_str());
    // Short sleeps, so a stop is seen within 100 ms.
    const auto wait_until = [&](Clock::time_point when) {
        while (!stopped()) {
            const auto now = Clock::now();
            if (now >= when) return true;
            std::this_thread::sleep_for(std::min<Clock::duration>(when - now, std::chrono::milliseconds(100)));
        }
        return false;
    };
    for (const Ms at : shots) {
        if (!wait_until(start + std::chrono::milliseconds(at))) return;
        const Ms asked = detail::Since(start);
        const std::string name = ShotName(at);
        auto written = capture(name);
        const char* outcome = "timeout";
        for (const auto limit = Clock::now() + kShotWait; !stopped() && Clock::now() < limit;) {
            if (written.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) continue;
            bool ok = false;
            try {
                ok = written.get();
            } catch (...) {  // dropped: another picture was still pending
            }
            outcome = ok ? "1" : "0";
            break;
        }
        if (outcome[0] == 't' && stopped()) outcome = "stopped";
        Report("test", ("EDEN_TEST shot at=" + SecondsText(at) + " asked=" + SecondsText(asked) + " file=" + name +
                        " ok=" + outcome).c_str());
    }
    Report("test", "EDEN_TEST shots_done");
}
} // namespace Eden::TestHarness
