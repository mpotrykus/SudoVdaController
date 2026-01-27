#pragma once

#include "DisplayGeometry.h"
#include <vector>

namespace vdc {

    // Computes the layout for isolated mode:
    // - The virtual display (index 0) is placed at (0,0)
    // - All other displays are repositioned so that they remain connected
    // - The layout is normalized so that the virtual display ends up in the lower-right
    //
    // Input:
    //   rects[0] = virtual display
    //   rects[1..N] = physical displays
    //
    // Output:
    //   A new vector of DisplayRect with updated positions.
    std::vector<DisplayRect> ComputeIsolatedLayout(const std::vector<DisplayRect>& rects);

    // Computes the minimal movement needed to connect two rectangles.
    // Returns a delta {dx, dy} that moves `unknown` toward `connected`.
    // If already connected, returns {0,0}.
    Coord ComputeConnectionDelta(
        const std::vector<Coord>& unknownCorners,
        const std::vector<Coord>& connectedCorners
    );

} // namespace vdc
