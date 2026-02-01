#pragma once
#include <string>
#include <guiddef.h>
#include <optional>
#include "../models/VirtualDisplay.h"

namespace vdc {

enum class DisplayAction : int {
    None = 0,
    Details = 1,
    Remove = 2,
    ToggleEnable = 3,
    Add = 4,
    SetPrimary = 5,
    ToggleHdr = 6,
    Custom = 7
};

struct TrayMenuItem {
    GUID guid{};
    DisplayAction action = DisplayAction::None;

    // place cfg before label so initializer-list usage in callers matches
    std::optional<VirtualDisplay> cfg;

    std::wstring label;
    std::wstring gdiName;
    std::wstring edid;
};

} // namespace vdc
