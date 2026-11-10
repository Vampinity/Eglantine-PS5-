// SPDX-License-Identifier: GPL-3.0-or-later
// Where Link is on Ocarina of Time's minimap and where the camera looks, read from the game's
// frame, and the stick that walks him toward a point on that map. No dependencies: test_harness.h
// uses it for the script command STEER, and tools/steer_cli.cpp runs it over recorded video.
//
// The minimap (Switch 2 version, Kokiri Forest and other outdoor maps): a yellow map that does not
// turn, bottom right of the picture. On it a deep red arrowhead with a white edge is Link, and a
// pale wedge from the arrowhead is what the camera sees. The stick moves Link relative to the
// camera, so the wedge is what turns "that way on the map" into "this way on the stick".
//
// Everything is worked out on a grid of kCols x kRows samples of the minimap region, whatever the
// picture's size, so the limits below mean the same at every resolution. They were set on
// recordings of 9 Nov 2026 (docs: console/notes/minimap-steering-2026-11-09.md): in village
// frames the marker was found in 99 to 100% and the wedge in 97 to 100%, day and night.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace steer {

// The minimap region, in per mille of the picture.
inline constexpr unsigned kRegionX = 742, kRegionY = 743, kRegionW = 250, kRegionH = 250;
inline constexpr int kCols = 320, kRows = 180;  // a 1280 x 720 picture's region, pixel for pixel

struct Fix {
    bool map = false;    // the yellow map is on screen
    bool found = false;  // Link's arrowhead
    bool view = false;   // the camera's wedge
    double x = 0, y = 0; // the arrowhead, in per mille of the picture
    double angle = 0;    // where the camera looks on the map: degrees, 0 = up, clockwise
    int area = 0;        // the arrowhead's samples (diagnostics)
};

namespace detail {
struct Rgb {
    int r, g, b;
};
inline bool Yellow(Rgb p) { return p.r > 140 && p.g > 110 && p.b < 120 && p.r - p.b > 60; }
inline bool Red(Rgb p) { return p.r > 125 && p.r - p.g > 55 && p.r - p.b > 55; }
inline bool Pale(Rgb p) { return p.b > 115 && std::min(p.r, p.g) > 150; }
}  // namespace detail

// bgra: 4 bytes a pixel in B, G, R, A order, the last row first when flip is set (the harness's
// Picture). A picture too small to hold the grid gives an empty Fix.
inline Fix Find(const unsigned char* bgra, unsigned width, unsigned height, bool flip) {
    using detail::Rgb;
    Fix fix;
    const std::uint64_t x0 = std::uint64_t{kRegionX} * width / 1000, y0 = std::uint64_t{kRegionY} * height / 1000;
    const std::uint64_t w = std::uint64_t{kRegionW} * width / 1000, h = std::uint64_t{kRegionH} * height / 1000;
    if (!bgra || w < static_cast<std::uint64_t>(kCols) / 2 || h < static_cast<std::uint64_t>(kRows) / 2) return fix;
    static thread_local std::vector<Rgb> grid;
    static thread_local std::vector<unsigned char> red, seen;
    grid.resize(std::size_t{kCols} * kRows);
    red.assign(grid.size(), 0);
    seen.assign(grid.size(), 0);
    std::size_t yellow = 0;
    for (int row = 0; row < kRows; ++row) {
        std::uint64_t y = y0 + h * static_cast<std::uint64_t>(row) / kRows;
        y = std::min<std::uint64_t>(y, height - 1);
        const std::uint64_t source = flip ? height - 1 - y : y;
        for (int col = 0; col < kCols; ++col) {
            const std::uint64_t x = std::min<std::uint64_t>(x0 + w * static_cast<std::uint64_t>(col) / kCols, width - 1);
            const unsigned char* pixel = bgra + (source * width + x) * 4;
            const Rgb p{pixel[2], pixel[1], pixel[0]};
            const std::size_t at = static_cast<std::size_t>(row) * kCols + col;
            grid[at] = p;
            yellow += detail::Yellow(p) ? 1 : 0;
            red[at] = detail::Red(p) ? 1 : 0;
        }
    }
    // No map (a menu, a loading screen, indoors), or a screen that is yellow all over.
    const double share = static_cast<double>(yellow) / static_cast<double>(grid.size());
    if (share < 0.03 || share > 0.5) return fix;
    fix.map = true;

    // The wedge round a point: the pale share of each 10 degree slice of a ring. One run of
    // strong slices, 30 to 140 degrees wide and all on one side, is the camera's view.
    const auto wedge = [&](double cy, double cx, double& angle) {
        std::array<int, 36> total{}, hits{};
        const int top = std::max(0, static_cast<int>(cy) - 31), bottom = std::min(kRows - 1, static_cast<int>(cy) + 31);
        const int left = std::max(0, static_cast<int>(cx) - 31), right = std::min(kCols - 1, static_cast<int>(cx) + 31);
        for (int row = top; row <= bottom; ++row) {
            for (int col = left; col <= right; ++col) {
                const double dy = row - cy, dx = col - cx, distance = std::hypot(dy, dx);
                if (distance < 9 || distance > 30) continue;
                double degrees = std::atan2(dx, -dy) * 180.0 / 3.14159265358979323846;
                if (degrees < 0) degrees += 360;
                const int slice = std::min(35, static_cast<int>(degrees / 10));
                ++total[slice];
                hits[slice] += detail::Pale(grid[static_cast<std::size_t>(row) * kCols + col]) ? 1 : 0;
            }
        }
        int strong = 0;
        double vx = 0, vy = 0, sum = 0;
        for (int slice = 0; slice < 36; ++slice) {
            const double part = total[slice] ? static_cast<double>(hits[slice]) / total[slice] : 0.0;
            if (part <= 0.45) continue;
            ++strong;
            const double middle = (slice * 10 + 5) * 3.14159265358979323846 / 180.0;
            vx += part * std::sin(middle);
            vy += part * std::cos(middle);
            sum += part;
        }
        if (strong < 3 || strong > 14 || sum <= 0 || std::hypot(vx, vy) / sum <= 0.75) return false;
        angle = std::atan2(vx, vy) * 180.0 / 3.14159265358979323846;
        if (angle < 0) angle += 360;
        return true;
    };

    // Red blobs the size and shape of the arrowhead. The one with a wedge is Link; without any
    // wedge, the deepest red (a map icon is orange).
    double best = -1;
    static thread_local std::vector<int> stack;
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kCols; ++col) {
            const std::size_t start = static_cast<std::size_t>(row) * kCols + col;
            if (!red[start] || seen[start]) continue;
            int area = 0, top = row, bottom = row, left = col, right = col;
            double sum_x = 0, sum_y = 0, depth = 0;
            stack.clear();
            stack.push_back(static_cast<int>(start));
            seen[start] = 1;
            while (!stack.empty()) {
                const int at = stack.back();
                stack.pop_back();
                const int y = at / kCols, x = at % kCols;
                ++area;
                sum_x += x;
                sum_y += y;
                depth += grid[at].r - grid[at].g;
                top = std::min(top, y);
                bottom = std::max(bottom, y);
                left = std::min(left, x);
                right = std::max(right, x);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int ny = y + dy, nx = x + dx;
                        if (ny < 0 || ny >= kRows || nx < 0 || nx >= kCols) continue;
                        const int next = ny * kCols + nx;
                        if (!red[next] || seen[next]) continue;
                        seen[next] = 1;
                        stack.push_back(next);
                    }
                }
            }
            if (area < 12 || area > 130 || bottom - top > 22 || right - left > 22) continue;
            if (static_cast<double>(area) / ((bottom - top + 1) * (right - left + 1)) < 0.35) continue;
            const double cy = sum_y / area, cx = sum_x / area;
            double angle = 0;
            const bool view = wedge(cy, cx, angle);
            const double score = (view ? 1000.0 : 0.0) + depth / area;
            if (score <= best) continue;
            best = score;
            fix.found = true;
            fix.view = view;
            fix.angle = angle;
            fix.area = area;
            // grid sample -> the picture's pixel it was read from -> per mille of the picture
            fix.x = (static_cast<double>(x0) + (cx + 0.5) * static_cast<double>(w) / kCols) * 1000.0 / width;
            fix.y = (static_cast<double>(y0) + (cy + 0.5) * static_cast<double>(h) / kRows) * 1000.0 / height;
        }
    }
    return fix;
}

// How far Link is from a point, both in per mille of the picture, in per mille of its width
// (a per mille of the height is 9/16 of one of the width).
inline double Distance(double x, double y, double to_x, double to_y) {
    return std::hypot(to_x - x, (to_y - y) * 9.0 / 16.0);
}

struct Stick {
    std::uint8_t x = 128, y = 128;  // the pad's own 0-255: 128 is centre, 0 is left and up
};

// The left stick that runs Link toward a point of the map, given where the camera looks: up on
// the stick is away from the camera. A fix without a view, or a point he is on, gives the centre.
inline Stick Toward(const Fix& fix, double to_x, double to_y) {
    if (!fix.found || !fix.view) return {};
    const double dx = to_x - fix.x, dy = (to_y - fix.y) * 9.0 / 16.0;
    const double length = std::hypot(dx, dy);
    if (length < 1e-6) return {};
    const double radians = fix.angle * 3.14159265358979323846 / 180.0;
    const double forward_x = std::sin(radians), forward_y = -std::cos(radians);  // on the map, y down
    const double right_x = std::cos(radians), right_y = std::sin(radians);
    const double ahead = (dx * forward_x + dy * forward_y) / length;
    const double aside = (dx * right_x + dy * right_y) / length;
    const auto axis = [](double value) {
        return static_cast<std::uint8_t>(std::clamp(std::lround(128.0 + 127.0 * value), 0L, 255L));
    };
    return {axis(aside), axis(-ahead)};
}

// A walk through a list of points, one look at a time. The harness keeps one per STEER line and
// does what Look says with the stick; nothing in here knows about the app.
struct Walk {
    enum class Do {
        Hold,   // hold `stick` until the next look
        Keep,   // Link's marker was not seen this look: keep doing what the stick does now
        Stand,  // not seen for too long: let the stick go, he stands until it is back
        Done,   // the last point is reached
    };
    std::size_t point = 0;         // points reached so far
    unsigned seen = 0, lost = 0;   // looks with marker and wedge; looks in a row without

    // points: per mille of the picture. radius: a point is reached within this many per mille of
    // the picture's width. lost_looks: looks in a row without the marker before Link stands.
    template <typename Points>
    Do Look(const Fix& fix, const Points& points, unsigned radius, unsigned lost_looks, Stick& stick) {
        if (!fix.found || !fix.view) {
            return ++lost >= lost_looks ? Do::Stand : Do::Keep;
        }
        ++seen;
        lost = 0;
        while (point < points.size() &&
               Distance(fix.x, fix.y, points[point].first, points[point].second) <= radius) {
            ++point;
        }
        if (point >= points.size()) return Do::Done;
        stick = Toward(fix, points[point].first, points[point].second);
        return Do::Hold;
    }
};

}  // namespace steer
