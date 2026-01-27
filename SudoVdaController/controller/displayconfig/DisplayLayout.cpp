#include "DisplayLayout.h"
#include <limits>
#include <cmath>

namespace vdc {

    namespace {

        // Generate the 4 corner points of a DisplayRect
        std::vector<Coord> Corners(const DisplayRect& r) {
            return {
                { r.pos.x,           r.pos.y },
                { r.pos.x + r.width, r.pos.y },
                { r.pos.x,           r.pos.y + r.height },
                { r.pos.x + r.width, r.pos.y + r.height }
            };
        }

        // Check if two rectangles touch or overlap
        bool IsConnected(const DisplayRect& a, const DisplayRect& b) {
            bool xOverlap = (a.right() >= b.pos.x - 1) && (b.right() >= a.pos.x - 1);
            bool yOverlap = (a.bottom() >= b.pos.y - 1) && (b.bottom() >= a.pos.y - 1);
            return xOverlap && yOverlap;
        }

    } // anonymous namespace

    // -----------------------------------------------------------------------------
    // ComputeConnectionDelta
    // -----------------------------------------------------------------------------
    Coord ComputeConnectionDelta(
        const std::vector<Coord>& unknown,
        const std::vector<Coord>& connected)
    {
        // Already connected?
        for (const auto& u : unknown) {
            for (const auto& c : connected) {
                if (std::abs(u.x - c.x) <= 1 && std::abs(u.y - c.y) <= 1)
                    return { 0, 0 };
            }
        }

        // Compute minimal dx
        int bestDx = std::numeric_limits<int>::max();
        for (const auto& u : unknown) {
            for (const auto& c : connected) {
                int dx = c.x - u.x;
                if (std::abs(dx) < std::abs(bestDx))
                    bestDx = dx;
            }
        }

        // Try applying dx and see if we still need dy
        std::vector<Coord> shifted = unknown;
        for (auto& p : shifted)
            p.x += bestDx;

        // Compute minimal dy
        int bestDy = std::numeric_limits<int>::max();
        for (const auto& u : shifted) {
            for (const auto& c : connected) {
                int dy = c.y - u.y;
                if (std::abs(dy) < std::abs(bestDy))
                    bestDy = dy;
            }
        }

        return { bestDx, bestDy };
    }

    // -----------------------------------------------------------------------------
    // ComputeIsolatedLayout
    // -----------------------------------------------------------------------------
    std::vector<DisplayRect> ComputeIsolatedLayout(const std::vector<DisplayRect>& rects) {
        if (rects.empty())
            return {};

        // Copy input
        std::vector<DisplayRect> out = rects;

        // Virtual display is index 0
        const int virtualIndex = 0;

        // Step 1: Find the display with the maximum bottom-right corner
        int maxX = std::numeric_limits<int>::min();
        int maxY = std::numeric_limits<int>::min();
        int maxIndex = -1;

        for (size_t i = 0; i < out.size(); ++i) {
            if ((int)i == virtualIndex)
                continue;

            int right = out[i].right();
            int bottom = out[i].bottom();

            if (right > maxX || (right == maxX && bottom > maxY)) {
                maxX = right;
                maxY = bottom;
                maxIndex = (int)i;
            }
        }

        // Step 2: Normalize so that maxIndex becomes the origin reference
        if (maxIndex >= 0) {
            for (size_t i = 0; i < out.size(); ++i) {
                if ((int)i == virtualIndex)
                    continue;

                out[i].pos.x -= maxX;
                out[i].pos.y -= maxY;
            }
        }

        // Step 3: Place virtual display at (0,0)
        out[virtualIndex].pos = { 0, 0 };

        // Step 4: Mark connected set
        std::vector<bool> connected(out.size(), false);
        connected[virtualIndex] = true;

        bool added = true;

        // Step 5: Iteratively connect displays
        while (added) {
            added = false;

            for (size_t i = 0; i < out.size(); ++i) {
                if (connected[i])
                    continue;

                // Try to connect to any connected display
                for (size_t j = 0; j < out.size(); ++j) {
                    if (!connected[j])
                        continue;

                    if (IsConnected(out[i], out[j])) {
                        connected[i] = true;
                        added = true;
                        break;
                    }

                    // Compute minimal movement
                    auto delta = ComputeConnectionDelta(
                        Corners(out[i]),
                        Corners(out[j])
                    );

                    if (delta.x != 0 || delta.y != 0) {
                        out[i].pos.x += delta.x;
                        out[i].pos.y += delta.y;
                        connected[i] = true;
                        added = true;
                        break;
                    }
                }
            }
        }

        return out;
    }

} // namespace vdc
