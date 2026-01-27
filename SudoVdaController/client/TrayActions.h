#pragma once

#include <windows.h>
#include "TrayMenuBuilder.h"

namespace vdc {

	void HandleTrayCommand(HWND hWnd, UINT cmd, TrayContext* ctx);

} 
