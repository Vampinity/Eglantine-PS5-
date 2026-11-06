#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Test runs (headless/test_harness.h).

On the PC: the script's format, the player, the recorder, test-quit, and that nothing is on
without an EDEN_TEST_ setting. Then the actual selection branch of main.cpp as a release compiles
it, and where the hooks sit. A release has no development define, so a hook that only worked
behind one would pass every other check and do nothing on the console.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
main = (root / 'headless/main.cpp').read_text()
pad = (root / 'headless/pad.cpp').read_text()
frontend = (root / 'headless/eglantineps5/frontend.cpp').read_text()
harness = (root / 'headless/test_harness.h').read_text()

# The settings are read after env.txt, and before the first launcher.
assert main.index('std::ifstream environment(Eden::AppFile("env.txt"));') < main.index('Eden::TestHarness::Start();') \
    < main.index('for (;;) {\n        Eden::StopLimit::End();')
# Each way the boot is refused, in the order of its reasons, and only a clean one boots.
skip = main[main.index('const char* const skip = !Eden::TestHarness::BootArmed()'):main.index('if (!skip) {')]
assert all(reason in skip for reason in ('"not_armed"', '!last_crash.report.empty() ? "crash"',
                                         'stop_limit_restart ? "stop_limit"', '!forwarded_game.empty() ? "forwarded"')), skip
assert 'Eden::TestHarness::MarkBooted();' in main[main.index('if (!skip) {'):main.index('EDEN_TEST boot title=')]
assert 'test_session{pad->Mapping(), session_start, guest, game_title};' in main, 'a script is told its game'
# Every way out reads test-quit: the top of the app's loop, while a game loads, in the game, in the launcher.
top = main.index('for (;;) {\n        Eden::StopLimit::End();')
assert main.index('if (Eden::TestHarness::QuitAsked()) {', top) < main.index('selected_game = SelectEglantinePS5Game(', top)
assert 'const bool asked = Eden::TestHarness::QuitAsked();' in main
assert main.index('if (pad->TakeReturnToMenu() || room_lost) {') < main.rindex('if (Eden::TestHarness::QuitAsked()) {')
assert 'while (running && !launcher.done() && !Eden::TestHarness::QuitAsked()) {' in frontend
# The script reaches the guest as the controller does, and the recorder sees what the controller sent.
assert main.index('const auto test_sample = test_session.Sample();') < main.index('if (test_sample) pad->Consume({&*test_sample, 1});')
assert 'if (test_recording) TestHarness::Record(raw);' in pad
# Nothing here needs a system function the app does not already import.
assert 'sce' not in harness.split('namespace Eden::TestHarness {', 1)[1], 'a system import in test_harness.h'
assert 'std::getenv' in harness and 'strtod' not in harness and 'stod' not in harness

start = main.index('        if (std::exchange(autoboot_pending, false)) {')
end = main.index('        Eden::BootTrace::Line("launcher closed: ', start)
branch = main[start:end]
SELECT = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>
struct Entry {
    std::filesystem::path name;
    const std::filesystem::path& path() const { return name; }
};
std::vector<Entry> entries;
std::uint64_t metadata;
int launcher_calls;
namespace Eden {
std::string AssetsPath(const char*) { return "/roms"; }
std::vector<Entry> ReadNativeDirectory(const std::string& folder, std::error_code&) {
    return folder == "/roms" ? entries : std::vector<Entry>{};
}
namespace BootTrace { template <typename... T> void Line(const char*, T...) {} }
}
std::uint64_t eden_game_title_id(const char*) { return metadata; }
std::string SelectEglantinePS5Game(int, const std::string&) { ++launcher_calls; return "/roms/chosen.xci"; }
std::string Select(bool autoboot_pending, std::string& launch_error) {
    int rooms = 0;
    std::string development_id = "0400486024DEC000", selected_game;
BRANCH
    assert(!autoboot_pending);
    return selected_game;
}
int main() {
    std::string error;
    assert(Select(false, error) == "/roms/chosen.xci" && launcher_calls == 1);  // no test run: as ever
    launcher_calls = 0;
    entries = {{"0400486024DEC000.nup"}};
    assert(Select(true, error) == "/roms/0400486024DEC000.nup" && launcher_calls == 0 && error.empty());
    entries = {{"Zelda.nup"}};
    metadata = 0x0400486024DEC000ULL;
    assert(Select(true, error) == "/roms/Zelda.nup" && launcher_calls == 0 && error.empty());
    metadata = 0;
    assert(Select(true, error) == "/roms/chosen.xci" && launcher_calls == 1 && !error.empty());  // not there
}
'''.replace('BRANCH', branch)
with tempfile.TemporaryDirectory(prefix='eden-test-harness-') as temporary:
    folder = Path(temporary)
    binary = folder / 'check'
    # No -Werror: this is a check of behaviour, and a warning of one compiler must not stop a build.
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-parameter', '-fno-rtti', '-pthread',
                    '-I', str(root / 'headless'), '-I', str(root / 'third_party'),
                    str(root / 'headless/test_harness_check.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, cwd=folder, timeout=60)
    source, select = folder / 'select.cpp', folder / 'select'
    source.write_text(SELECT)
    subprocess.run(['g++', '-std=c++20', str(source), '-o', str(select)], check=True)
    subprocess.run([str(select)], check=True)
print('Test runs: the release branch boots a title by name and metadata, a missing one opens the launcher, hooks in place PASS')
