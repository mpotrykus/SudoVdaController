#include "DisplayGeometry.h"
#include <sstream>

namespace vdc {

    std::string DebugPrintRects(const std::vector<DisplayRect>& rects) {
        std::ostringstream out;

        for (size_t i = 0; i < rects.size(); ++i) {
            const auto& r = rects[i];
            out << "Index: " << i
                << ", X: " << r.pos.x
                << ", Y: " << r.pos.y
                << ", width: " << r.width
                << ", height: " << r.height
                << "\n";
        }

        return out.str();
    }

} // namespace vdc
