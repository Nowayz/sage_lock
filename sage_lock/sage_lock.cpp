/////////////
// sage_lock.cpp : This file contains code to lock HID touch devices when a volume up/down pattern is detected.
// Author: Phillip McNallen (2023)
//////

#include <Windows.h>
#include <Windowsx.h>
#include <iostream>
#include <array>
#include <initguid.h>
#include <ntddstor.h>
#include <hidusage.h>
#include <SetupAPI.h>
#include <Cfgmgr32.h>
#include <Hidclass.h>
#include <Hidsdi.h>
#include <hidusage.h>
#include <vector>
#include <string>
#include <iomanip>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "Winmm.lib")

// function dbgprint prints to visual studio output window
void dbgprint(const wchar_t* format, ...) {
	wchar_t buffer[4096];
	va_list args;
	va_start(args, format);
	vswprintf_s(buffer, 4096, format, args);
	OutputDebugStringW(buffer);
	va_end(args);
}

std::wstring GetLastErrorAsWString()
{
	DWORD errorMessageID = ::GetLastError();
	if (errorMessageID == 0) {
		return std::wstring(L"No error"); //No error message has been recorded
	}
	wchar_t lpBuffer[256];
	size_t size = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL, errorMessageID, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), lpBuffer, sizeof(lpBuffer), NULL);
	return lpBuffer;
}


// GLOBALS TO TRACK VOLUME UP DOWN UP DOWN EVENTS
std::array<DWORD, 4> Volume_Event_History{};
WORD Current_Index = 0;
DWORD64 Last_Volume_Event = 0;
int lock_enabled = 0;
std::vector<std::wstring> g_TouchScreens;

// Check Volume_Event_History for UP DOWN UP DOWN events in the last 2 seconds
auto CheckForVolumeUpDownUpDown() {
	Current_Index = 0;
	return (Volume_Event_History[0] == VK_VOLUME_UP &&
		Volume_Event_History[1] == VK_VOLUME_DOWN &&
		Volume_Event_History[2] == VK_VOLUME_UP &&
		Volume_Event_History[3] == VK_VOLUME_DOWN);
}

// This function returns the index of the next available slot in the volume history array.
// The index is determined by the time since the last volume change event.
auto GetAvailableKbdHistoryIndex() {
	auto dwCurrentTime = GetTickCount64();
	auto timeSinceLast = dwCurrentTime - Last_Volume_Event;
	Last_Volume_Event = dwCurrentTime;
	if ((timeSinceLast) > 500) {
		Current_Index = 0;
	}
	else {
		Current_Index++;
	}
	if (Current_Index > 3) {
		Current_Index = 0;
	}
	return Current_Index;
}

// wrap a call to run the program pnputil with /disable-device and /enable-device
void ToggleTouchDevice(const wchar_t* deviceId, bool enable) {
	wchar_t cmd[4096];
	swprintf_s(cmd, L"pnputil.exe %s \"%s\"", enable ? L"/enable-device" : L"/disable-device", deviceId);
	dbgprint(L"Running command: %s\n", cmd);
	// Use CreateProcessW
	STARTUPINFO si;
	PROCESS_INFORMATION pi;
	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
		dbgprint(L"CreateProcess failed (%d).\n", GetLastError());
		return;
	}
	// Wait until child process exits.
	WaitForSingleObject(pi.hProcess, INFINITE);
	// Close process and thread handles.
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
}

void GetTouchScreens()
{
	HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVINTERFACE_HID, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
	if (deviceInfoSet == INVALID_HANDLE_VALUE) {
		dbgprint(L"SetupDiGetClassDevs failed: %s", GetLastErrorAsWString().c_str());
		return;
	}

	SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
	ZeroMemory(&deviceInterfaceData, sizeof(deviceInterfaceData));
	deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL, &GUID_DEVINTERFACE_HID, i, &deviceInterfaceData); i++)
	{
		DWORD requiredSize = 0;
		SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, NULL, 0, &requiredSize, NULL);

		PSP_DEVICE_INTERFACE_DETAIL_DATA detailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)LocalAlloc(LMEM_FIXED, requiredSize);
		if (detailData == NULL)
			continue;

		detailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
		SP_DEVINFO_DATA devInfoData;
		ZeroMemory(&devInfoData, sizeof(devInfoData));
		devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

		if (SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData, detailData, requiredSize, NULL, &devInfoData))
		{
			HANDLE deviceHandle = CreateFile(detailData->DevicePath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
			if (deviceHandle != INVALID_HANDLE_VALUE)
			{
				PHIDP_PREPARSED_DATA preparsedData;
				HIDP_CAPS caps;
				if (HidD_GetPreparsedData(deviceHandle, &preparsedData) == TRUE)
				{
					if (HidP_GetCaps(preparsedData, &caps) != HIDP_STATUS_SUCCESS) {
						dbgprint(L"HidP_GetCaps failed\n");
					}
					// filter for touch-screen type devices
					if (caps.UsagePage == HID_USAGE_PAGE_DIGITIZER &&
						(caps.Usage == HID_USAGE_DIGITIZER_HEAT_MAP || // surface pro touch screen device is heat_map type
							caps.Usage == HID_USAGE_DIGITIZER_TOUCH_SCREEN ||
							caps.Usage == HID_USAGE_DIGITIZER_MULTI_POINT))
					{
						CONFIGRET cr;
						// get string with deviceid 
						WCHAR deviceId[MAX_DEVICE_ID_LEN];
						if ((cr = CM_Get_Device_IDW(devInfoData.DevInst, deviceId, MAX_DEVICE_ID_LEN, 0)) != CR_SUCCESS) {
							dbgprint(L"CM_Get_Device_IDA failed with error %08X\n", cr);
						}

						dbgprint(L"Found touch screen device: %s\n", deviceId);
						g_TouchScreens.push_back(deviceId);

						CloseHandle(deviceHandle);
					}
					HidD_FreePreparsedData(preparsedData);
				}
			}
		}
		LocalFree(detailData);
	}
	SetupDiDestroyDeviceInfoList(deviceInfoSet);
}

void SoundEffect(bool enable)
{
	LPCWSTR soundFile = enable ? L"C:\\Windows\\Media\\Speech On.wav" : L"C:\\Windows\\Media\\Speech Off.wav";
	PlaySound(soundFile, NULL, SND_FILENAME | SND_ASYNC);
}

void SetKbdHistoryIndex(DWORD vkKey) {
	auto i = GetAvailableKbdHistoryIndex();
	Volume_Event_History[i] = vkKey;
	if ((i == 3) && CheckForVolumeUpDownUpDown()) {
		lock_enabled = !lock_enabled;
		
		for (auto screen : g_TouchScreens) {
			ToggleTouchDevice(screen.c_str(), !lock_enabled);
		}
		SoundEffect(!lock_enabled);
	}
}

LRESULT CALLBACK pWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if (uMsg == WM_INPUT) {
		UINT dwSize = 0;
		GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));
		if (dwSize > 0) {
			static BYTE lpb[64];
			auto eventInfo = (RAWINPUT*)lpb;
			if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb, &dwSize, sizeof(RAWINPUTHEADER)) == dwSize &&
				eventInfo->header.dwType == RIM_TYPEKEYBOARD &&
				eventInfo->data.keyboard.Message == WM_KEYDOWN &&
				(eventInfo->data.keyboard.VKey == VK_VOLUME_UP ||
					eventInfo->data.keyboard.VKey == VK_VOLUME_DOWN))
				SetKbdHistoryIndex(eventInfo->data.keyboard.VKey);
		}
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

DWORD WINAPI InputEventThread(LPVOID lpParameter) {
	static const wchar_t* winClassName = L"RECV_RAW_INPT";
	WNDCLASSEX wx = {};
	wx.cbSize = sizeof(WNDCLASSEX);
	wx.lpfnWndProc = pWndProc; // Callback to handle messages
	wx.hInstance = GetModuleHandle(NULL);
	wx.lpszClassName = winClassName;
	HWND hWnd = NULL;
	if (RegisterClassEx(&wx)) {
		hWnd = CreateWindowEx(0, winClassName, L"IOInptWin", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, NULL, NULL);
	}

	RAWINPUTDEVICE Rid[1]; // 1 = number of devices to listen to
	Rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
	Rid[0].usUsage = HID_USAGE_GENERIC_KEYBOARD;
	Rid[0].dwFlags = RIDEV_INPUTSINK;
	Rid[0].hwndTarget = hWnd;
	RegisterRawInputDevices(Rid, 1, sizeof(Rid[0]));

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return 0;
}

// CheckIfAlreadyRunning is a function that installs a global mutex and checks if it already exists
// if it does, it means that the program is already running and we should exit
bool CheckIfAlreadyRunning() {
	HANDLE hMutex = CreateMutex(NULL, TRUE, L"Global\\SAGE_LOCK_INSTANCE");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(hMutex);
		return true;
	}
	return false;
}

int WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
	if (CheckIfAlreadyRunning()) {
		MessageBoxW(NULL, L"SageLock is already running", L"SageLock", MB_OK | MB_ICONERROR);
		return 0;
	}

	// Populate Touch List
	GetTouchScreens();

	HANDLE hInputThread = CreateThread(NULL, NULL, InputEventThread, NULL, NULL, NULL);
	WaitForSingleObject(hInputThread, INFINITE);
	return 0;
}

