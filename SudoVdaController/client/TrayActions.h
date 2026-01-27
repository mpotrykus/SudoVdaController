#pragma once

#include <windows.h>
#include "TrayMenuBuilder.h"

namespace vdc {

	// Executes a tray command (WM_COMMAND → menu ID)
	void HandleTrayCommand(HWND hWnd, UINT cmd, TrayContext* ctx);

} // namespace vdc
