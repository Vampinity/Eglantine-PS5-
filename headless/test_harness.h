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
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
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

struct Script {
    std::vector<Event> events;  // by start time; a recording's lines are by end time
    std::string error;          // not empty: the script must not be played
};
inline Script ParseScript(std::istream& input, const ButtonMapping& mapping) {
    Script script;
    unsigned number = 0;
    for (std::string line; std::getline(input, line);) {
        ++number;
        if (!ParseLine(line, mapping, script.events, script.error)) {
            script.error = "line " + std::to_string(number) + ": " + script.error;
            script.events.clear();
            return script;
        }
    }
    std::stable_sort(script.events.begin(), script.events.end(),
                     [](const Event& a, const Event& b) { return a.start < b.start; });
    return script;
}

// The pad state a script asks for at a moment.
class Player {
public:
    explicit Player(std::vector<Event> script) : events{std::move(script)} {}
    // Empty while the script presses nothing: the controller plays then. A press is seen at least
    // once however short it is and however late the caller comes, and one neutral sample follows
    // the last press so that the controller's own state takes over from nothing held.
    std::optional<ps5::pad::Data> Sample(Ms now) {
        for (; next < events.size() && events[next].start <= now; ++next) {
            active.push_back(events[next]);
            active.back().end = std::max(active.back().end, now + 1);
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
    bool Done() const { return next == events.size() && active.empty() && !held; }
    std::size_t Size() const { return events.size(); }

private:
    static ps5::pad::Data Neutral() {
        auto sample = ps5::pad::neutral_data();
        sample.connected = 1;
        return sample;
    }
    std::vector<Event> events;
    std::size_t next = 0;
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
    if (boot.empty() && script.empty() && !record) return;
    detail::settings = ReadSettings(boot, script, record);
    Report("test", ("EDEN_TEST on boot=" + detail::settings.boot + " script=" + detail::settings.script +
                    " record=" + (detail::settings.record ? "1" : "0")).c_str());
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
// game and title are that session's ROM path and title id.
class Session {
public:
    Session(const ButtonMapping& mapping, Clock::time_point start, const char* game, std::uint64_t title)
        : origin{start} {
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
        auto sample = player->Sample(now);
        if (!finished && player->Done()) {
            finished = true;
            Report("test", ("EDEN_TEST script_done at=" + SecondsText(now)).c_str());
        }
        return sample;
    }

private:
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
        player.emplace(std::move(script.events));
        Report("test", ("EDEN_TEST script_start file=" + name + " events=" + std::to_string(player->Size())).c_str());
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
    std::optional<Player> player;
    bool finished = false;
};
} // namespace Eden::TestHarness
