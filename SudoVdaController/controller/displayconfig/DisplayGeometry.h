#pragma once

#include <vector>
#include <string>

namespace vdc {

    // A simple 2D coordinate.
    struct Coord {
        int x = 0;
        int y = 0;
    };

    // A rectangle-like structure used by the layout algorithm.
    // Represents the position and size of a display.
    struct DisplayRect {
        Coord pos;     // top-left corner
        int width = 0;
        int height = 0;

        // Optional: index into the DISPLAYCONFIG_MODE_INFO array
        // (the layout engine doesn't need to know what this means)
        int modeIndex = -1;

        int right() const noexcept { return pos.x + width; }
        int bottom() const noexcept { return pos.y + height; }
    };

    // Differences between two coordinates (used by the old algorithm)
    struct CoordDiff {
        Coord left;
        Coord right;
        Coord diff;
        Coord absDiff;
    };

    // Utility: pretty-print a list of DisplayRect for debugging.
    std::string DebugPrintRects(const std::vector<DisplayRect>& rects);

} // namespace vdc
