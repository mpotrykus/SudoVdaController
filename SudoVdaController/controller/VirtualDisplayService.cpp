#include "../pch.h"
#include "../third_party/sudovda/sudovda-ioctl.h"
#include "../third_party/sudovda/sudovda.h"
#include "VirtualDisplayService.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <objbase.h>
#include "../utils/GuidUtils.h"
#include "../utils/StringUtils.h"

using namespace SUDOVDA;

namespace vdc {

	// START ISOLATED DISPLAY DECLARATIONS
	struct positionwidthheight;
	struct coordinates;
	struct coordinatesdifferences;
	struct coordinates
	{
		int x;
		int y;
	};

	struct positionwidthheight
	{
		struct coordinates position;
		int width;
		int height;
		int modeindex;
	};

	struct coordinatesdifferences
	{
		struct coordinates left;
		struct coordinates right;
		struct coordinates Difference;
		struct coordinates AbsDifference;

	};

	std::vector <std::wstring> matchDisplay(std::wstring sMatch);
	std::vector< struct positionwidthheight*>rearrangeVirtualDisplayForLowerRight(std::vector< struct positionwidthheight*> displays);
	std::string printAllDisplays(std::vector< struct positionwidthheight*> displays);
	std::vector < struct coordinates > moveToBeConnected(std::vector < struct coordinates > unknown, std::vector< struct coordinates> connected);

	// END ISOLATED DISPLAY DECLARATIONS

	VirtualDisplayService::VirtualDisplayService() {
	}

	VirtualDisplayService::~VirtualDisplayService() {
		closeVDisplayDevice();
	}

	bool VirtualDisplayService::Open() {
		// If the driver handle is already valid, consider it opened.
		if (SUDOVDA_DRIVER_HANDLE != INVALID_HANDLE_VALUE) {
			return true;
		}
		auto st = openVDisplayDevice();
		return st == DRIVER_STATUS::OK;
	}

	LONG VirtualDisplayService::getDeviceSettings(const wchar_t* deviceName, DEVMODEW& devMode) {
		devMode.dmSize = sizeof(DEVMODEW);
		return EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &devMode);
	}

	LONG VirtualDisplayService::changeDisplaySettings(const wchar_t* deviceName, int width, int height, int refresh_rate, bool bApplyIsolated) {
		UINT32 pathCount = 0;
		UINT32 modeCount = 0;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount)) {
			LOG_ERROR("Failed to query display configuration size.");
			return ERROR_INVALID_PARAMETER;
		}

		std::vector<DISPLAYCONFIG_PATH_INFO> pathArray(pathCount);
		std::vector<DISPLAYCONFIG_MODE_INFO> modeArray(modeCount);
		std::vector<struct positionwidthheight*> displayArray;
		struct positionwidthheight* pCurrentElement;

		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, pathArray.data(), &modeCount, modeArray.data(), nullptr) != ERROR_SUCCESS) {
			LOG_ERROR("Failed to query display configuration.");
			return ERROR_INVALID_PARAMETER;
		}

		bool bAtVirtualDisplay;
		bool bVirtualDisplayAlreadyAdded = false;
		std::string sDisplayOutput;

		if (bApplyIsolated == true)
		{
			for (UINT32 i = 0; i < pathCount; i++) {
				DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
				sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
				sourceName.header.size = sizeof(sourceName);
				sourceName.header.adapterId = pathArray[i].sourceInfo.adapterId;
				sourceName.header.id = pathArray[i].sourceInfo.id;
				bAtVirtualDisplay = false;

				if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
					continue;
				}

				auto* sourceInfo = &pathArray[i].sourceInfo;
				auto* targetInfo = &pathArray[i].targetInfo;

				if (std::wstring_view(sourceName.viewGdiDeviceName) == std::wstring_view(deviceName))
				{
					bAtVirtualDisplay = true;
				}

				if (true) {
					for (UINT32 j = 0; j < modeCount; j++) {
						if (
							modeArray[j].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
							modeArray[j].adapterId.HighPart == sourceInfo->adapterId.HighPart &&
							modeArray[j].adapterId.LowPart == sourceInfo->adapterId.LowPart &&
							modeArray[j].id == sourceInfo->id
							) {
							auto* sourceMode = &modeArray[j].sourceMode;

							LOG_INFO("Current mode found : [% dx % dx % d]", sourceMode->width, sourceMode->height, targetInfo->refreshRate);

							pCurrentElement = new (struct positionwidthheight);

							pCurrentElement->position.x = modeArray[j].sourceMode.position.x;
							pCurrentElement->position.y = modeArray[j].sourceMode.position.y;
							pCurrentElement->height = modeArray[j].sourceMode.height;
							pCurrentElement->width = modeArray[j].sourceMode.width;
							pCurrentElement->modeindex = j;

							// This is the virtual display - insert at the front of the vector
							if (bAtVirtualDisplay == true && bVirtualDisplayAlreadyAdded == false)
							{
								displayArray.insert(displayArray.begin() + 0, pCurrentElement);
								bVirtualDisplayAlreadyAdded = true;
							}
							else {
								displayArray.push_back(pCurrentElement);
							}
						}
					}
				}
			}

			sDisplayOutput = "";
			sDisplayOutput += "Before: \n";
			sDisplayOutput += printAllDisplays(displayArray);

			displayArray = rearrangeVirtualDisplayForLowerRight(displayArray);

			sDisplayOutput += "";
			sDisplayOutput += "After: \n";
			sDisplayOutput += printAllDisplays(displayArray);

			int iIndex;
			int xdifference, ydifference = 0;
			for (iIndex = 0; iIndex < displayArray.size(); iIndex += 1)
			{

				// Find the primary display and get the offset to apply to all of the displays to keep the same primary
				if (modeArray[(displayArray[iIndex]->modeindex)].sourceMode.position.x == 0 &&
					modeArray[(displayArray[iIndex]->modeindex)].sourceMode.position.y == 0)
				{
					xdifference = (displayArray[iIndex]->position.x) * -1;
					ydifference = (displayArray[iIndex]->position.y) * -1;
					break;
				}
			}

			// Set all of the OS Displays to their new locations; Do not change the primary
			// Update the real vector for the system call
			for (iIndex = 0; iIndex < displayArray.size(); iIndex += 1)
			{
				modeArray[(displayArray[iIndex]->modeindex)].sourceMode.position.x = displayArray[iIndex]->position.x + xdifference;
				modeArray[(displayArray[iIndex]->modeindex)].sourceMode.position.y = displayArray[iIndex]->position.y + ydifference;
				modeArray[(displayArray[iIndex]->modeindex)].sourceMode.height = displayArray[iIndex]->height;
				modeArray[(displayArray[iIndex]->modeindex)].sourceMode.width = displayArray[iIndex]->width;
			}

			// Apply the changes only if the virtual display was found
			if (bVirtualDisplayAlreadyAdded == true) {
				LONG status = SetDisplayConfig(
					pathCount,
					pathArray.data(),
					modeCount,
					modeArray.data(),
					SDC_APPLY
					| SDC_USE_SUPPLIED_DISPLAY_CONFIG
					| SDC_SAVE_TO_DATABASE
				);
				if (status != ERROR_SUCCESS) {
					LOG_ERROR("Failed to apply display settings.");
				}
				else {
					LOG_INFO("Display settings updated successfully.");
				}
			}
			for (iIndex = 0; iIndex < displayArray.size(); iIndex += 1)
			{
				if (displayArray[iIndex] != nullptr)
				{
					delete displayArray[iIndex];
				}
				displayArray.clear();
			}
		}

		// After performing the isolated display movements, do the regular movements
		for (UINT32 i = 0; i < pathCount; i++) {
			DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
			sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			sourceName.header.size = sizeof(sourceName);
			sourceName.header.adapterId = pathArray[i].sourceInfo.adapterId;
			sourceName.header.id = pathArray[i].sourceInfo.id;

			if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
				continue;
			}

			auto* sourceInfo = &pathArray[i].sourceInfo;
			auto* targetInfo = &pathArray[i].targetInfo;

			if (std::wstring_view(sourceName.viewGdiDeviceName) == std::wstring_view(deviceName)) {
				LOG_INFO("Display found: %ls", deviceName);
				for (UINT32 j = 0; j < modeCount; j++) {
					if (
						modeArray[j].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
						modeArray[j].adapterId.HighPart == sourceInfo->adapterId.HighPart &&
						modeArray[j].adapterId.LowPart == sourceInfo->adapterId.LowPart &&
						modeArray[j].id == sourceInfo->id
						) {
						auto* sourceMode = &modeArray[j].sourceMode;

						LOG_INFO("Current mode found: [%dx%dx%d]", sourceMode->width, sourceMode->height, targetInfo->refreshRate);

						sourceMode->width = width;
						sourceMode->height = height;

						targetInfo->refreshRate = { (UINT32)refresh_rate, 1000 };

						// Apply the changes
						LONG status = SetDisplayConfig(
							pathCount,
							pathArray.data(),
							modeCount,
							modeArray.data(),
							SDC_APPLY
							| SDC_USE_SUPPLIED_DISPLAY_CONFIG
							| SDC_SAVE_TO_DATABASE
						);
						if (status != ERROR_SUCCESS) {
							LOG_ERROR("Failed to apply display settings.");
						}
						else {
							LOG_INFO("Display settings updated successfully.");
						}

						return status;
					}
				}

				LOG_ERROR("Mode [%dx%dx%d] not found for display: %ls", width, height, refresh_rate, deviceName);
				return ERROR_INVALID_PARAMETER;
			}
		}

		LOG_ERROR("Display not found: %ls", deviceName);
		return ERROR_DEVICE_NOT_CONNECTED;
	}

	bool VirtualDisplayService::findDisplayIds(const wchar_t* displayName, LUID& adapterId, uint32_t& targetId) {
		UINT32 pathCount;
		UINT32 modeCount;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount)) {
			return false;
		}

		std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
		std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr)) {
			return false;
		}

		auto path = std::find_if(paths.begin(), paths.end(), [&displayName](DISPLAYCONFIG_PATH_INFO _path) {
			DISPLAYCONFIG_PATH_SOURCE_INFO sourceInfo = _path.sourceInfo;

			DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
			sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			sourceName.header.size = sizeof(sourceName);
			sourceName.header.adapterId = sourceInfo.adapterId;
			sourceName.header.id = sourceInfo.id;

			if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
				return false;
			}

			return std::wstring_view(displayName) == sourceName.viewGdiDeviceName;
			});

		if (path == paths.end()) {
			return false;
		}

		adapterId = path->sourceInfo.adapterId;
		targetId = path->targetInfo.id;

		return true;
	}

	void VirtualDisplayService::closeVDisplayDevice() {
		if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
			return;
		}

		CloseHandle(SUDOVDA_DRIVER_HANDLE);

		SUDOVDA_DRIVER_HANDLE = INVALID_HANDLE_VALUE;
	}

	VirtualDisplayService::DRIVER_STATUS VirtualDisplayService::openVDisplayDevice() {
		uint32_t retryInterval = 20;
		while (true) {
			SUDOVDA_DRIVER_HANDLE = OpenDevice(&SUVDA_INTERFACE_GUID);
			if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
				if (retryInterval > 320) {
					LOG_ERROR("Open device failed");
					return DRIVER_STATUS::FAILED;
				}
				retryInterval *= 2;
				Sleep(retryInterval);
				continue;
			}

			break;
		}

		if (!CheckProtocolCompatible(SUDOVDA_DRIVER_HANDLE)) {
			LOG_ERROR("SUDOVDA protocol not compatible with driver");
			closeVDisplayDevice();
			return DRIVER_STATUS::VERSION_INCOMPATIBLE;
		}

		return DRIVER_STATUS::OK;
	}

	bool VirtualDisplayService::startPingThread(std::function<void()> failCb) {
		if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
			return false;
		}

		VIRTUAL_DISPLAY_GET_WATCHDOG_OUT watchdogOut;
		if (GetWatchdogTimeout(SUDOVDA_DRIVER_HANDLE, watchdogOut)) {
			LOG_INFO("Watchdog: Timeout %d, Countdown %d", watchdogOut.Timeout, watchdogOut.Countdown);
		}
		else {
			LOG_ERROR("Watchdog fetch failed");
			return false;
		}

		if (watchdogOut.Timeout) {
			auto sleepInterval = watchdogOut.Timeout * 1000 / 3;
			std::thread ping_thread([sleepInterval, failCb = std::move(failCb)] {
				uint8_t fail_count = 0;
				for (;;) {
					if (!sleepInterval) return;
					if (!PingDriver(SUDOVDA_DRIVER_HANDLE)) {
						fail_count += 1;
						if (fail_count > 3) {
							failCb();
							return;
						}
					};
					Sleep(sleepInterval);
				}
				});

			ping_thread.detach();
		}

		return true;
	}

	bool VirtualDisplayService::setRenderAdapterByName(const std::wstring& adapterName) {
		if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
			return false;
		}

		Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
		if (!SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
			return false;
		}

		Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
		DXGI_ADAPTER_DESC desc;
		int i = 0;
		while (SUCCEEDED(factory->EnumAdapters(i, &adapter))) {
			i += 1;

			if (!SUCCEEDED(adapter->GetDesc(&desc))) {
				continue;
			}

			if (std::wstring_view(desc.Description) != adapterName) {
				continue;
			}

			if (SetRenderAdapter(SUDOVDA_DRIVER_HANDLE, desc.AdapterLuid)) {
				return true;
			}
		}

		return false;
	}

	std::optional<std::wstring> VirtualDisplayService::createVirtualDisplay(
		const char* s_client_uid,
		const char* s_client_name,
		uint32_t width,
		uint32_t height,
		float fps,
		const GUID& guid
	) {
		if (!Open()) return std::nullopt;

		auto client_uid = GuidToString(guid);
		if (client_uid.empty()) {
			LOG_WARN("Failed to stringify GUID");
			return std::nullopt;
		}

		if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
			return std::wstring();
		}

		VIRTUAL_DISPLAY_ADD_OUT output;
		UINT refresh_millihz = static_cast<UINT>(std::lroundf(fps));

		if (!AddVirtualDisplay(SUDOVDA_DRIVER_HANDLE, width, height, refresh_millihz, guid, s_client_name, s_client_uid, output)) {
			LOG_ERROR("Failed to add virtual display.");
			return std::wstring();
		}

		uint32_t retryInterval = 20;
		wchar_t deviceName[CCHDEVICENAME]{};
		while (!GetAddedDisplayName(output, deviceName)) {
			Sleep(retryInterval);
			if (retryInterval > 320) {
				LOG_WARN("Cannot get name for newly added virtual display");
				return std::wstring();
			}
			retryInterval *= 2;
		}

		LOG_INFO("Virtual display added successfully: %ls", deviceName);
		LOG_INFO("Configuration: W: %d, H: %d, FPS: %.2f", width, height, static_cast<float>(refresh_millihz));

		auto dev = std::wstring(deviceName);
		if (dev.empty()) {
			LOG_ERROR("Failed to create a virtual display");
			return std::nullopt;
		}

		startPingThread([dev]() {
			LOG_ERROR("Watchdog detected driver failure for device: %s", dev.c_str());
		});

		return dev;
	}

	bool VirtualDisplayService::removeVirtualDisplay(const GUID& guid) {
		if (!Open()) return false;

		if (SUDOVDA_DRIVER_HANDLE == INVALID_HANDLE_VALUE) {
			return false;
		}

		if (RemoveVirtualDisplay(SUDOVDA_DRIVER_HANDLE, guid)) {
			LOG_INFO("Virtual display removed successfully.");
			return true;
		}
		else {
			return false;
		}
	}

	// START ISOLATED DISPLAY METHODS
	// Shows the coordinates/height/width for the displays in the vector structure
	std::string VirtualDisplayService::printAllDisplays(std::vector< struct positionwidthheight*> displays) {
		int iIndex;
		std::string sOutput;

		for (iIndex = 0; iIndex < displays.size(); iIndex++)
		{
			sOutput += "Index: ";
			sOutput += std::to_string(iIndex);
			sOutput += ", X : ";
			sOutput += std::to_string(displays[iIndex]->position.x);
			sOutput += ", Y : ";
			sOutput += std::to_string(displays[iIndex]->position.y);
			sOutput += ", width : ";
			sOutput += std::to_string(displays[iIndex]->width);
			sOutput += ", height : ";
			sOutput += std::to_string(displays[iIndex]->height);
			sOutput += "\n";

		}
		return sOutput;
	}

	// Helper method for the rearrangeVirtualDisplayForLowerRight() method to move the unknown unconnected display to be connected to the
	// second display which is assumed to be already connected
	//
	// It will return the move that the unknown display would need to perform
	std::vector < struct coordinates > VirtualDisplayService::moveToBeConnected(std::vector < struct coordinates > unknown, std::vector< struct coordinates> connected) {
		// Figure out if the boxes are connected
		// Assume that there are 4 points
		int iIndex, iIndex2;

		std::vector< struct coordinatesdifferences > differences;

		std::vector< struct coordinatesdifferences > vertical;
		std::vector< struct coordinatesdifferences > horizontal;

		std::vector < struct coordinates >moveResult;

		std::vector < struct coordinates > unknown2;

		struct coordinatesdifferences sTemp1;
		struct coordinates sNoMove;

		sNoMove.x = 0;
		sNoMove.y = 0;

		struct coordinates sDoMove;
		sDoMove.x = 0;
		sDoMove.y = 0;

		bool bCornerConnect = false;
		bool bVerticalConnect = false;
		bool bHorizontalConnect = false;

		int iCountLess;
		int iCountGreater;

		// Subtract all of the points
		for (iIndex = 0; iIndex < connected.size(); iIndex += 1) {
			for (iIndex2 = 0; iIndex2 < unknown.size(); iIndex2 += 1) {
				sTemp1.left.x = connected[iIndex].x;
				sTemp1.left.y = connected[iIndex].y;
				sTemp1.right.x = unknown[iIndex2].x;
				sTemp1.right.y = unknown[iIndex2].y;

				sTemp1.Difference.x = sTemp1.left.x - sTemp1.right.x;
				sTemp1.Difference.y = sTemp1.left.y - sTemp1.right.y;

				sTemp1.AbsDifference.x = abs(sTemp1.Difference.x);
				sTemp1.AbsDifference.y = abs(sTemp1.Difference.y);

				differences.push_back(sTemp1);
			}
		}

		for (iIndex = 0; iIndex < differences.size(); iIndex += 1) {

			// See if they are any corner connects
			sTemp1 = differences[iIndex];
			if (sTemp1.AbsDifference.x <= 1 && sTemp1.AbsDifference.y <= 1) {
				bCornerConnect = true;
				break;
			}

			// See if there are any vertical connects
			if (sTemp1.AbsDifference.x <= 1) {
				vertical.push_back(sTemp1);
			}

			// See if there are any horizontal connects
			if (sTemp1.AbsDifference.y <= 1) {
				horizontal.push_back(sTemp1);
			}
		}

		// Check the vertical connects
		iCountLess = 0;
		iCountGreater = 0;
		for (iIndex = 0; iIndex < vertical.size(); iIndex += 1) {
			if (vertical[iIndex].left.y <= vertical[iIndex].right.y) {
				iCountLess += 1;
			}
			if (vertical[iIndex].left.y >= vertical[iIndex].right.y) {
				iCountGreater += 1;
			}
		}

		// Check the sum off all of the counts
		if (((iCountLess > 0) && (iCountGreater == 0)) ||
			((iCountGreater > 0) && (iCountLess == 0)) ||
			(iCountLess == 0 && iCountGreater == 0)) {
			// Boxes are on the same vertical but above or below each other
			bVerticalConnect = false;
		}
		else {
			bVerticalConnect = true;
		}

		// Check the horizontal connects
		iCountLess = 0;
		iCountGreater = 0;
		for (iIndex = 0; iIndex < horizontal.size(); iIndex += 1) {
			if (horizontal[iIndex].left.x <= horizontal[iIndex].right.x) {
				iCountLess += 1;
			}
			if (horizontal[iIndex].left.x >= horizontal[iIndex].right.x) {
				iCountGreater += 1;
			}
		}
		// Check the sum off all of the counts
		if (((iCountLess > 0) && (iCountGreater == 0)) ||
			((iCountGreater > 0) && (iCountLess == 0)) ||
			(iCountLess == 0 && iCountGreater == 0)) {
			// Boxes are on the same horizontal but to the left or right of each other
			bHorizontalConnect = false;
		}
		else {
			bHorizontalConnect = true;
		}

		// End the logic if there is no move required
		if (bHorizontalConnect == true ||
			bVerticalConnect == true ||
			bCornerConnect == true) {
			moveResult.push_back(sNoMove);
			return moveResult;
		}

		// Otherwise, show the move required
		int iShortestX = INT_MAX;
		int iShortestXIndex = -1;

		// Try the horizontal (x) move first
		for (iIndex = 0; iIndex < differences.size(); iIndex += 1) {
			if (differences[iIndex].AbsDifference.x < iShortestX) {
				iShortestXIndex = iIndex;
				iShortestX = differences[iIndex].AbsDifference.x;
			}
		}

		if (iShortestX <= 1) {
			// X move is not required
		}
		else {
			// This is the X to move
			sDoMove.x = differences[iShortestXIndex].Difference.x;

			// Perform the x move on the left so that we can check the y
			unknown2 = unknown;
			for (iIndex = 0; iIndex < unknown2.size(); iIndex += 1) {
				unknown2[iIndex].x += sDoMove.x;
			}

			// Call oneself recursively only once so that we can see if there is Y to do.
			std::vector < struct coordinates >moveResult2;
			moveResult2 = moveToBeConnected(unknown2, connected);

			// Format the answer for a return
			sDoMove.y = moveResult2[0].y;

			moveResult.push_back(sDoMove);
			return moveResult;
		}

		// Figure out the y move required
		// Otherwise, show the move required
		int iShortestY = INT_MAX;
		int iShortestYIndex = -1;

		// Try the horizontal (x) move first
		for (iIndex = 0; iIndex < differences.size(); iIndex += 1) {
			if (differences[iIndex].AbsDifference.y < iShortestY) {
				iShortestYIndex = iIndex;
				iShortestY = differences[iIndex].AbsDifference.y;
			}
		}

		if (iShortestY <= 1) {
			// Y move is not required
		}
		else {
			// This is the Y to move
			sDoMove.y = differences[iShortestYIndex].Difference.y;
			moveResult.push_back(sDoMove);
			return moveResult;
		}
		moveResult.push_back(sNoMove);
		return moveResult;
	}

	// Main method to rearrange the displays to have one isolated display in the lower right and
	// move the other displays as necessary especially if there are holes
	std::vector< struct positionwidthheight*> VirtualDisplayService::rearrangeVirtualDisplayForLowerRight(std::vector< struct positionwidthheight*> displays) {

		// Make a temporary connected List based on the current Displays
		// Here connected means that the displays are "touching" by either the
		// vertical axis or a horizontal axis or a corner.
		int count = displays.size();
		std::vector< int > vConnected(count, 0);

		// Need the index of the virtual display to put into the lower right corner as primary
		int changeIndex = 0;

		// Find the Maxx and Maxy for the current displays
		int imaxx = INT_MIN;
		int imaxy = INT_MIN;
		int imaxindex = -1;

		int itempx;
		int itempy;
		int itempvalid = 0;

		// Figure out the maxx and maxy, and the index for that rectangle
		for (int index = 0; index < count; index++) {
			itempx = displays[index]->position.x + displays[index]->width;
			itempy = displays[index]->position.y + displays[index]->height;
			itempvalid = 1;
			if (changeIndex == index) {
				itempvalid = 0;
			}
			if (itempvalid > 0) {
				if (imaxx < itempx) {
					imaxx = itempx;
					imaxy = itempy;
					imaxindex = index;
				}
				else if (imaxx == itempx) {
					if (imaxy < itempy) {
						imaxy = itempy;
						imaxindex = index;
					}
				}
			}
		}


		// Adjust all of the other windows based on the offset for the display that will be 0,0 in the lower right corner.
		if (imaxindex > -1) {
			// Adjusting other displays based on the offset for the display that will be 0,0 in the lower right corner
			for (int index = 0; index < count; index++) {
				itempvalid = 1;
				if (changeIndex == index) {
					itempvalid = 0;
				}
				if (itempvalid > 0) {
					displays[index]->position.x -= imaxx;
					displays[index]->position.y -= imaxy;
				}
			}
		}

		// Get the location, width and height of the window that is moving
		// Make sure the correct display is set to 0,0.
		for (int index = 0; index < count; index++) {
			if (index == changeIndex) {
				displays[index]->position.x = 0;
				displays[index]->position.y = 0;
				vConnected[index] = 1;
			}
		}

		bool bAddedConnected;
		int connectedboxx, connectedboxy, connectedboxwidth, connectedboxheight;
		int secondboxx, secondboxy, secondboxwidth, secondboxheight, secondboxindex;

		bool bFirstTime = true;
		int xmin;
		int ymin;
		int minindexconnected;
		int minindexnonconnected;

		std::vector< struct coordinates> connectedboxpoints;
		std::vector< struct coordinates> secondboxpoints;
		struct coordinates sTempCoordinates;

		// MAIN LOOP to rearrange displays to be connected to each other.
		// This is either corner to corner or vertical side or horizontal side
		do {
			xmin = INT_MAX;
			ymin = INT_MAX;
			minindexconnected = -1;
			minindexnonconnected = -1;

			do {
				bAddedConnected = false;

				for (int index = 0; index < count; index++) {
					if (vConnected[index] == 1) {
						// Skip the virtual window if this is not the first time because we do not want an displays connected to it
						if (bFirstTime == false && index == changeIndex) {
							continue;
						}

						connectedboxx = displays[index]->position.x;
						connectedboxy = displays[index]->position.y;
						connectedboxwidth = displays[index]->width;
						connectedboxheight = displays[index]->height;

						connectedboxpoints.clear();

						sTempCoordinates.x = connectedboxx;
						sTempCoordinates.y = connectedboxy;
						connectedboxpoints.push_back(sTempCoordinates);

						sTempCoordinates.x = connectedboxx + connectedboxwidth;
						sTempCoordinates.y = connectedboxy;
						connectedboxpoints.push_back(sTempCoordinates);

						sTempCoordinates.x = connectedboxx;
						sTempCoordinates.y = connectedboxy + connectedboxheight;
						connectedboxpoints.push_back(sTempCoordinates);

						sTempCoordinates.x = connectedboxx + connectedboxwidth;
						sTempCoordinates.y = connectedboxy + connectedboxheight;
						connectedboxpoints.push_back(sTempCoordinates);

						// Go through all other boxes and see if there is a connected box to this one
						for (int index2 = 0; index2 < count; index2++) {
							if (index2 == index || vConnected[index2] == 1 || index2 == changeIndex) {
								// Skip oneself and the skip boxes already connected and skip over changeIndex
								continue;
							}
							secondboxx = displays[index2]->position.x;
							secondboxy = displays[index2]->position.y;
							secondboxwidth = displays[index2]->width;
							secondboxheight = displays[index2]->height;
							secondboxindex = index2;

							secondboxpoints.clear();

							sTempCoordinates.x = secondboxx;
							sTempCoordinates.y = secondboxy;
							secondboxpoints.push_back(sTempCoordinates);

							sTempCoordinates.x = secondboxx + secondboxwidth;
							sTempCoordinates.y = secondboxy;
							secondboxpoints.push_back(sTempCoordinates);

							sTempCoordinates.x = secondboxx;
							sTempCoordinates.y = secondboxy + secondboxheight;
							secondboxpoints.push_back(sTempCoordinates);

							sTempCoordinates.x = secondboxx + secondboxwidth;
							sTempCoordinates.y = secondboxy + secondboxheight;
							secondboxpoints.push_back(sTempCoordinates);

							// What would it take to MOVE the display to be connected to another connected display
							// The result of this may not be used as there may be a closer display when we go through the list
							std::vector < struct coordinates > sToMove = moveToBeConnected(secondboxpoints, connectedboxpoints);

							// No movement necessary
							if (sToMove[0].x == 0 && sToMove[0].y == 0) {
								vConnected[secondboxindex] = 1;

								// NEWLY ADDED
								bFirstTime = false;

								bAddedConnected = true;
								xmin = INT_MAX;
								ymin = INT_MAX;

								// Need to restart the whole loop sequence to not connect more than one at the same time.
								break;
							}
							else {
								if (index != changeIndex) {
									// Want to see if this display would be the closest one to move via the x coordinates
									if (abs(sToMove[0].x) < xmin) {
										xmin = abs(sToMove[0].x);
										ymin = abs(sToMove[0].y);
										minindexconnected = index;
										minindexnonconnected = index2;
									}
								}
							}
						}
					}

					// Need to restart the whole loop sequence to not connect more than one at the same time.
					if (bAddedConnected == true) {
						break;
					}
				}
			} while (bAddedConnected == true);

			// We are finish adding the connected box during the initial pass throguh
			// We should also have the minimal display to move
			bFirstTime = false;

			if (xmin != INT_MAX || ymin != INT_MAX) {
				connectedboxx = displays[minindexconnected]->position.x;
				connectedboxy = displays[minindexconnected]->position.y;
				connectedboxwidth = displays[minindexconnected]->width;
				connectedboxheight = displays[minindexconnected]->height;

				connectedboxpoints.clear();

				sTempCoordinates.x = connectedboxx;
				sTempCoordinates.y = connectedboxy;
				connectedboxpoints.push_back(sTempCoordinates);

				sTempCoordinates.x = connectedboxx + connectedboxwidth;
				sTempCoordinates.y = connectedboxy;
				connectedboxpoints.push_back(sTempCoordinates);

				sTempCoordinates.x = connectedboxx;
				sTempCoordinates.y = connectedboxy + connectedboxheight;
				connectedboxpoints.push_back(sTempCoordinates);

				sTempCoordinates.x = connectedboxx + connectedboxwidth;
				sTempCoordinates.y = connectedboxy + connectedboxheight;
				connectedboxpoints.push_back(sTempCoordinates);



				secondboxx = displays[minindexnonconnected]->position.x;
				secondboxy = displays[minindexnonconnected]->position.y;
				secondboxwidth = displays[minindexnonconnected]->width;
				secondboxheight = displays[minindexnonconnected]->height;
				secondboxindex = minindexnonconnected;

				secondboxpoints.clear();

				sTempCoordinates.x = secondboxx;
				sTempCoordinates.y = secondboxy;
				secondboxpoints.push_back(sTempCoordinates);

				sTempCoordinates.x = secondboxx + secondboxwidth;
				sTempCoordinates.y = secondboxy;
				secondboxpoints.push_back(sTempCoordinates);

				sTempCoordinates.x = secondboxx;
				sTempCoordinates.y = secondboxy + secondboxheight;
				secondboxpoints.push_back(sTempCoordinates);

				sTempCoordinates.x = secondboxx + secondboxwidth;
				sTempCoordinates.y = secondboxy + secondboxheight;
				secondboxpoints.push_back(sTempCoordinates);

				// Perform the actual move
				std::vector < struct coordinates > sToMove = moveToBeConnected(secondboxpoints, connectedboxpoints);

				// Apply the move to the array of displays
				displays[minindexnonconnected]->position.x += sToMove[0].x;
				displays[minindexnonconnected]->position.y += sToMove[0].y;
			}
		} while (xmin != INT_MAX);

		return displays;
	}

	// Utility function to match the DeviceString to the Display Names
	// Typical DeviceStrings are the driver names
	//
	// Example: matchDisplay(L"SudoMaker Virtual Display Adapter")
	// Result: L"\\\\.\\Display2"

	std::vector <std::wstring> VirtualDisplayService::matchDisplay(std::wstring sMatch) {
		DISPLAY_DEVICEW displayDevice;
		displayDevice.cb = sizeof(DISPLAY_DEVICE);

		std::wstring matchDeviceName;

		std::vector <std::wstring>vMatches;

		int deviceIndex = 0;
		while (EnumDisplayDevicesW(NULL, deviceIndex, &displayDevice, 0)) {
			if (std::wstring(displayDevice.DeviceString) == sMatch &&
				displayDevice.StateFlags > 0) {
				matchDeviceName = displayDevice.DeviceName;
				vMatches.push_back(matchDeviceName);
			}
			deviceIndex++;
		}
		return vMatches;
	}

	// END ISOLATED DISPLAY METHODS
}