// SPDX-License-Identifier: GPL-3.0-or-later
// The script, the player and the recorder of test_harness.h on the PC (tools/check-test-harness.py).
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <unistd.h>
#include "test_harness.h"

using namespace Eden;
using namespace Eden::TestHarness;
using namespace ps5::pad;

#define REQUIRE(what) do { if (!(what)) { std::fprintf(stderr, "test harness check failed at line %d: %s\n", __LINE__, #what); std::abort(); } } while (0)

static Script Parse(const std::string& text, const ButtonMapping& mapping = kDefaultMapping) {
    std::istringstream input{text};
    return ParseScript(input, mapping);
}
static void Fails(const std::string& text, const std::string& starts) {
    const Script script = Parse(text);
    REQUIRE(script.events.empty());
    REQUIRE(script.error.starts_with(starts));
}
static Data Pressed(ButtonMask buttons, Stick left = {128, 128}) {
    auto sample = neutral_data();
    sample.connected = 1;
    sample.buttons = buttons;
    sample.left_stick = left;
    return sample;
}

static void CheckTimes() {
    REQUIRE(Seconds("12") == 12000u);
    REQUIRE(Seconds("12.5") == 12500u);
    REQUIRE(Seconds(".25") == 250u);
    REQUIRE(Seconds("7.") == 7000u);
    REQUIRE(Seconds("0.0167") == 16u);
    for (const char* wrong : {"", ".", "1.2.3", "-1", "1e3", "abc", "99999999999"}) REQUIRE(!Seconds(wrong));
    REQUIRE(SecondsText(0) == "0.000");
    REQUIRE(SecondsText(12050) == "12.050");
    REQUIRE(Seconds(SecondsText(98765)) == 98765u);
}

static void CheckScript() {
    const Script script = Parse("# route to the first room\n"
                                "\n"
                                "  12.5 CROSS 100   # Continue\r\n"
                                "3 LSTICK 128 0 2000\n"
                                "4 RSTICK 255 128 50\n"
                                "5 A 80\n");
    REQUIRE(script.error.empty());
    REQUIRE(script.events.size() == 4);
    // By time, whatever the order of the lines.
    REQUIRE(script.events[0].start == 3000 && script.events[0].end == 5000);
    REQUIRE(script.events[0].stick == StickSide::left && script.events[0].x == 128 && script.events[0].y == 0);
    REQUIRE(script.events[1].stick == StickSide::right && script.events[1].x == 255);
    REQUIRE(script.events[2].buttons == kButtonCircle);  // the game's A, on the default mapping
    REQUIRE(script.events[3].start == 12500 && script.events[3].buttons == kButtonCross);
    // Every name is known, and the game's names follow the game's own mapping.
    for (const NamedButton& button : kButtons) REQUIRE(ButtonNamed(button.name, kDefaultMapping) == button.mask);
    for (const std::string_view name : kGameNames) REQUIRE(ButtonNamed(name, kDefaultMapping).has_value());
    const ButtonMapping swapped = Assign(kDefaultMapping, game_a, pad_cross);
    REQUIRE(ButtonNamed("A", swapped) == kButtonCross && ButtonNamed("B", swapped) == kButtonCircle);
    // A line that cannot be read stops the script, and says where.
    Fails("1 NOPE 100\n", "line 1: unknown button");
    Fails("1 A 100\n2 A\n", "line 2: wrong number");
    Fails("x A 100\n", "line 1: bad time");
    Fails("1 A 0\n", "line 1: bad hold");
    Fails("1 LSTICK 300 0 100\n", "line 1: stick position");
    Fails("1 LSTICK 128 100\n", "line 1: wrong number");
    REQUIRE(Parse("").events.empty() && Parse("").error.empty());
}

static void CheckPlayer() {
    Player player{Parse("1 CROSS 100\n"
                        "1.05 UP 100\n"
                        "2 LSTICK 0 128 100\n"
                        "2.05 LSTICK 255 128 100\n"
                        "3 R1 1\n").events};
    REQUIRE(!player.Sample(0) && !player.Sample(999));
    REQUIRE(player.Sample(1000)->buttons == kButtonCross);
    REQUIRE(player.Sample(1000)->connected == 1);
    REQUIRE(player.Sample(1060)->buttons == (kButtonCross | kButtonUp));
    REQUIRE(player.Sample(1120)->buttons == kButtonUp);
    // One neutral sample after the last press, then the controller again.
    const auto released = player.Sample(1200);
    REQUIRE(released && released->buttons == 0 && released->left_stick.x == 128);
    REQUIRE(!player.Sample(1300));
    REQUIRE(player.Sample(2000)->left_stick.x == 0);
    REQUIRE(player.Sample(2060)->left_stick.x == 255);  // the later of two
    REQUIRE(player.Sample(2110)->left_stick.x == 255);
    REQUIRE(player.Sample(2160)->left_stick.x == 128);
    REQUIRE(!player.Sample(2170) && !player.Done());
    // Far shorter than the caller's step, and the caller came late: still seen once.
    REQUIRE(player.Sample(3100)->buttons == kButtonR1);
    REQUIRE(player.Sample(3200)->buttons == 0 && player.Done());
    REQUIRE(!player.Sample(4000));
}

static void CheckRecorder() {
    Recorder recorder;
    Ms now = 0;
    const auto hold = [&](const Data& sample, Ms ms) {
        for (const Ms end = now + ms; now < end; now += 4) recorder.Take(sample, now);
    };
    hold(Pressed(0), 1000);
    hold(Pressed(kButtonCross), 200);
    hold(Pressed(0), 300);
    hold(Pressed(0, {128, 10}), 1000);    // walking up
    hold(Pressed(0, {130, 12}), 1000);    // a wiggle is no new position
    hold(Pressed(0, {128, 128}), 500);
    hold(Pressed(kButtonUp, {120, 136}), 100);  // inside the dead zone: no stick
    hold(Pressed(0), 100);
    auto trigger = Pressed(0);
    trigger.triggers.r2 = 200;
    hold(trigger, 100);
    auto away = Pressed(kButtonSquare);
    hold(away, 100);
    away.connected = 0;
    hold(away, 100);  // a controller that went away holds nothing
    // The press that ends the session is still held when the recording ends: no line.
    hold(Pressed(kButtonTouchPad | kButtonL1), 100);
    const Script script = Parse(recorder.text);
    REQUIRE(script.error.empty());
    REQUIRE(script.events.size() == 5);
    REQUIRE(script.events[0].buttons == kButtonCross && script.events[0].start == 1000 &&
            script.events[0].end == 1200);
    REQUIRE(script.events[1].stick == StickSide::left && script.events[1].y == 10 &&
            script.events[1].start == 1500 && script.events[1].end == 3500);
    REQUIRE(script.events[2].buttons == kButtonUp);
    REQUIRE(script.events[3].buttons == kButtonR2);
    REQUIRE(script.events[4].buttons == kButtonSquare);
    REQUIRE(recorder.text.find("TOUCHPAD") == std::string::npos && recorder.text.find(" L1 ") == std::string::npos);

    // A moving stick: new lines, but not one per sample, and the replay follows it.
    Recorder sweep;
    Ms at = 0;
    for (unsigned step = 0; step < 500; ++step, at += 4)
        sweep.Take(Pressed(0, {static_cast<std::uint8_t>(128 + step / 4), 128}), at);
    sweep.Take(Pressed(0), at);
    const Script swept = Parse(sweep.text);
    REQUIRE(swept.error.empty());
    REQUIRE(swept.events.size() >= 10 && swept.events.size() <= 2000 / Recorder::kStickMs);
    Player player{swept.events};
    for (unsigned step = 100; step < 500; step += 40) {
        const auto sample = player.Sample(step * 4);
        REQUIRE(sample);
        REQUIRE(std::abs(int{sample->left_stick.x} - int(128 + step / 4)) <= 2 * Recorder::kStickStep + 13);
    }
}

// With no EDEN_TEST_ setting: nothing is on, and a session is empty.
static void CheckOff() {
    for (const char* name : {"EDEN_TEST_BOOT", "EDEN_TEST_SCRIPT", "EDEN_TEST_RECORD"}) unsetenv(name);
    Start();
    REQUIRE(BootTitle().empty() && !QuitAsked() && !Recording());
    Session session{kDefaultMapping, Clock::now(), "/roms/any.nup", 1};
    REQUIRE(!session.Sample() && !Recording());
    // Off and empty values are no setting.
    setenv("EDEN_TEST_RECORD", "0", 1);
    setenv("EDEN_TEST_BOOT", " \r", 1);
    Start();
    REQUIRE(BootTitle().empty() && !QuitAsked());
    Session other{kDefaultMapping, Clock::now(), nullptr, 0};
    REQUIRE(!Recording());
}

// A script is for the title this process booted, found the way main.cpp finds it.
static void CheckForGame() {
    constexpr std::string_view zelda = "0400486024DEC000";
    constexpr std::uint64_t zelda_id = 0x0400486024DEC000ULL, kart_id = 0x0400FF0011223000ULL;
    REQUIRE(ForGame("", false, "/roms/any.nup", kart_id));              // no EDEN_TEST_BOOT: every game
    REQUIRE(ForGame("", false, nullptr, 0));
    REQUIRE(ForGame(zelda, true, "/roms/Zelda.nup", zelda_id));         // by metadata
    REQUIRE(ForGame(zelda, true, "/roms/0400486024DEC000.nup", 0));     // by file name
    REQUIRE(ForGame("0400486024dec000", true, "/roms/Zelda.nup", zelda_id));
    REQUIRE(!ForGame(zelda, true, "/roms/Kart.nup", kart_id));          // started by hand after it
    REQUIRE(!ForGame(zelda, true, nullptr, kart_id));
    // Not booted by this process: env.txt left behind, or the boot was skipped after a crash.
    REQUIRE(!ForGame(zelda, false, "/roms/Zelda.nup", zelda_id));
    // A session the gate refuses plays nothing and records nothing, also with both set.
    detail::settings.boot = std::string{zelda};
    detail::settings.script = "no-such-route.txt";
    detail::settings.record = true;
    detail::booted = false;
    Session refused{kDefaultMapping, Clock::now(), "/roms/Zelda.nup", zelda_id};
    REQUIRE(!refused.Sample() && !Recording());
    detail::settings = {};
}

static std::atomic<int> limits{0};
static void Limit() noexcept { ++limits; }
static void CheckQuit() {
    REQUIRE(ValidTitle("0400486024DEC000") && ValidTitle("040074a01bf12000"));
    REQUIRE(!ValidTitle("0400") && !ValidTitle("0400486024DEC00G") && !ValidTitle(""));
    REQUIRE(ValidFileName("route.txt") && !ValidFileName("../route.txt") && !ValidFileName("..") && !ValidFileName(""));
    using namespace std::chrono_literals;
    const std::string file = "test-quit-" + std::to_string(getpid());
    std::thread watcher([&] { Watch(file, 10ms, 100ms, &Limit); });
    std::this_thread::sleep_for(50ms);
    REQUIRE(!QuitAsked());
    std::fclose(std::fopen(file.c_str(), "w"));
    std::this_thread::sleep_for(60ms);
    REQUIRE(QuitAsked() && limits == 0);
    REQUIRE(!std::ifstream(file).good());  // removed, so it is not read twice
    watcher.join();
    REQUIRE(limits == 1);
}

// A mistyped title must not turn a script for one game into a script for every game.
static void CheckBadBoot() {
    const auto typo = ReadSettings("0400486024DEC00", "route.txt", true);
    REQUIRE(typo.boot.empty() && typo.script.empty() && !typo.record);
    const auto good = ReadSettings("0400486024DEC000", "route.txt", true);
    REQUIRE(good.boot == "0400486024DEC000" && good.script == "route.txt" && good.record);
    const auto any = ReadSettings("", "route.txt", false);  // no title asked for: every game, as ever
    REQUIRE(any.boot.empty() && any.script == "route.txt");
    REQUIRE(ReadSettings("", "../route.txt", false).script.empty());
}

// The title comes from env.txt, whatever surrounds it there.
static void CheckBootSetting() {
    setenv("EDEN_TEST_BOOT", "0400486024DEC000\r", 1);
    Start();
    REQUIRE(BootTitle() == "0400486024DEC000");
    REQUIRE(!BootArmed());  // no test-boot-once: a left-behind env.txt boots nothing
}

// EDEN_TEST_SHOTS: times as the script's, sorted, each once, at most kMaxShots, and none at all
// when one cannot be read.
static void CheckShots() {
    const ShotList list = ParseShots(" 150, 95.5 ,120,95.5");
    REQUIRE(list.error.empty() && list.times == (std::vector<Ms>{95500, 120000, 150000}));
    REQUIRE(ShotName(95500) == "game-frame-95.5.ppm" && ShotName(120000) == "game-frame-120.ppm");
    REQUIRE(ShotName(250) == "game-frame-0.25.ppm" && ShotName(0) == "game-frame-0.ppm");
    for (const char* wrong : {"", "95,,100", "95;100", "-1", "1e3", "95 100"}) {
        const ShotList bad = ParseShots(wrong);
        REQUIRE(bad.times.empty() && !bad.error.empty());
    }
    const ShotList many = ParseShots("14,13,12,11,10,9,8,7,6,5,4,3,2,1");
    REQUIRE(many.times.size() == kMaxShots && many.times.front() == 1000 && many.times.back() == 12000);
    REQUIRE(!many.error.empty());
    // Only the booted game takes them, as a script.
    detail::settings = {};
    detail::settings.boot = "0400486024DEC000";
    detail::settings.shots = {120000};
    detail::booted = false;
    REQUIRE(ShotsFor("/roms/Zelda.nup", 0x0400486024DEC000ULL).empty());
    detail::booted = true;
    REQUIRE(ShotsFor("/roms/Zelda.nup", 0x0400486024DEC000ULL).size() == 1);
    REQUIRE(ShotsFor("/roms/Kart.nup", 0x0400FF0011223000ULL).empty());
    detail::settings = {};
    detail::booted = false;
    REQUIRE(ShotsFor("/roms/Zelda.nup", 0x0400486024DEC000ULL).empty());
}

static void CheckSteer() {
    // A STEER line ends its section as a WAIT does, and carries its points.
    const Script script = Parse("1 A 100\n2 STEER 90 500,500 600,700 r=8\n0.5 B 100\n3 END\n");
    REQUIRE(script.error.empty());
    REQUIRE(script.waits.size() == 2 && script.waits[0].steer && !script.waits[1].steer && script.waits[1].end);
    REQUIRE(script.waits[0].points.size() == 2);
    REQUIRE((script.waits[0].points[1] == std::pair<unsigned, unsigned>{600, 700}));
    REQUIRE(script.waits[0].radius == 8 && script.waits[0].timeout == 90000u && script.waits[0].at == 2000u);
    REQUIRE(script.events.size() == 2 && script.events[0].section == 0 && script.events[1].section == 1);
    REQUIRE(Parse("0 STEER 30 1,2\n1 END\n").waits[0].radius == 6);
    REQUIRE(!Parse("0 WAIT 5 LOOK 0 0 10 10 2x1:00ff\n").waits[0].steer);
    Fails("0 STEER 30\n", "line 1: wrong number of fields");
    Fails("0 STEER 0 1,2\n", "line 1: bad timeout");
    Fails("0 STEER 30 1001,2\n", "line 1: bad point");
    Fails("0 STEER 30 12\n", "line 1: bad point");
    Fails("0 STEER 30 1,2 r=0\n", "line 1: bad radius");
    Fails("0 STEER 30 r=5\n", "line 1: bad point");  // a radius alone is not a walk
    Fails("0 STEER 30 1,2 r=5 3,4\n", "line 1: bad point");  // the radius comes last
    std::string many = "0 STEER 30";
    for (int i = 0; i < 64; ++i) many += " 1,2";
    REQUIRE(Parse(many + "\n").error.empty());
    Fails(many + " 1,2\n", "line 1: 1 to 64 points");
    // The player holds the stick a STEER sets from one look to the next, and lets go when the
    // section passes (one neutral sample, then the controller again).
    Player player{{}, script.waits};
    REQUIRE(!player.Sample(10));
    player.Steer(std::pair<std::uint8_t, std::uint8_t>{200, 40});
    auto sample = player.Sample(20);
    REQUIRE(sample && sample->left_stick.x == 200 && sample->left_stick.y == 40 && sample->buttons == 0);
    sample = player.Sample(5000);
    REQUIRE(sample && sample->left_stick.x == 200);
    player.Pass(6000);
    sample = player.Sample(6010);
    REQUIRE(sample && sample->left_stick.x == 128 && sample->left_stick.y == 128);
    REQUIRE(!player.Sample(6020));
    player.Steer(std::pair<std::uint8_t, std::uint8_t>{1, 1});
    sample = player.Sample(6030);
    REQUIRE(sample && sample->left_stick.x == 1 && sample->left_stick.y == 1);
    player.Stop();  // a timeout: the stick is let go
    sample = player.Sample(6040);
    REQUIRE(sample && sample->left_stick.x == 128);
    REQUIRE(!player.Sample(6050));
}

int main() {
    CheckSteer();
    CheckTimes();
    CheckScript();
    CheckPlayer();
    CheckRecorder();
    CheckOff();
    CheckForGame();
    CheckQuit();
    CheckBadBoot();
    CheckBootSetting();
    CheckShots();
    std::puts("Test harness: script, player, recorder, inert without settings, test-quit PASS");
}
