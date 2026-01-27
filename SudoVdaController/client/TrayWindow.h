#pragma once

#include <windows.h>

namespace vdc {

	// Forward declarations
	struct TrayContext;

	// Creates the hidden tray window and adds the tray icon.
	// Returns the HWND of the created window, or nullptr on failure.
	HWND CreateTrayWindow(TrayContext* ctx);

	// Removes the tray icon and destroys the window.
	void DestroyTrayWindow(HWND hWnd);

} // namespace vdc
