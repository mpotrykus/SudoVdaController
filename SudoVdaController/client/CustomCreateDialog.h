#pragma once

#include <windows.h>
#include "../models/VirtualDisplay.h"

namespace vdc {

	// Shows the modal "Custom Create Display" dialog.
	// Returns true if the user pressed Create and fills outCfg.
	bool ShowCustomCreateDialog(HWND parent, VirtualDisplay& outCfg);

} // namespace vdc
