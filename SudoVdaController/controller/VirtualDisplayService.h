#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <vector>

#ifndef FILE_DEVICE_UNKNOWN
#define FILE_DEVICE_UNKNOWN 0x00000022
#endif

#include "../third_party/sudovda/sudovda.h"
#include <optional>

namespace vdc {

    extern HANDLE SUDOVDA_DRIVER_HANDLE;
	
	class VirtualDisplayService {
	public:
		VirtualDisplayService();
		~VirtualDisplayService();
		
		enum class DRIVER_STATUS {
			UNKNOWN = 1,
			OK = 0,
			FAILED = -1,
			VERSION_INCOMPATIBLE = -2,
			WATCHDOG_FAILED = -3
		};


		bool Open();

		std::optional<std::wstring> createVirtualDisplay(
			const char* s_client_uid,
			const char* s_client_name,
			uint32_t width,
			uint32_t height,
			float fps,
			const GUID& guid
		);
		bool removeVirtualDisplay(const GUID& guid);

		LONG getDeviceSettings(const wchar_t* deviceName, DEVMODEW& devMode);
		LONG changeDisplaySettings(const wchar_t* deviceName, int width, int height, int refresh_rate, bool bApplyIsolated = false);
		bool findDisplayIds(const wchar_t* displayName, LUID& adapterId, uint32_t& targetId);

		DRIVER_STATUS openVDisplayDevice();
		void closeVDisplayDevice();

		bool startPingThread(std::function<void()> failCb);
		bool setRenderAdapterByName(const std::wstring& adapterName);
		std::vector<std::wstring> matchDisplay(std::wstring sMatch);

		std::string printAllDisplays(std::vector< struct positionwidthheight*> displays);
		std::vector < struct coordinates > moveToBeConnected(std::vector < struct coordinates > unknown, std::vector< struct coordinates> connected);
		std::vector< struct positionwidthheight*> rearrangeVirtualDisplayForLowerRight(std::vector< struct positionwidthheight*> displays);
	};
}
