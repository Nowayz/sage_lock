/////////////
// sage_lock.cpp : This file contains code to lock HID touch devices when a volume up/down pattern is detected.
// Author: Phillip McNallen (2023)
//////

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <Windowsx.h>
#include <shellapi.h>
#include <TlHelp32.h>
#include <initguid.h>
#include <hidusage.h>
#include <SetupAPI.h>
#include <Cfgmgr32.h>
#include <Hidclass.h>
#include <Hidsdi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <array>
#include <cmath>
#include <gdiplus.h>
#include "resource.h"

using namespace Gdiplus;

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "Cfgmgr32.lib")
#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "gdiplus.lib")

namespace {
	constexpr int OverlayWindowSize = 280;
	constexpr float OverlayLabelTop = 16.0f;
	constexpr float OverlayLabelHeight = 48.0f;
	constexpr float OverlayIconTop = 54.0f;
	constexpr float OverlayIconHeight = 166.0f;
	constexpr float OverlayDotsY = 238.0f;
	constexpr DWORD OverlayStepMs = 650;
	constexpr DWORD OverlayLockMs = 1300;
	constexpr UINT OverlayTimerId = 1001;
	constexpr UINT TrayIconId = 1002;
	constexpr UINT TrayMessageId = WM_APP + 1;
	constexpr UINT TrayMenuScreenLockModeId = 2002;
	constexpr UINT TrayMenuKioskModeId = 2003;
	constexpr UINT TrayMenuCloseId = 2004;

	enum class SageLockMode {
		ScreenLock,
		Kiosk
	};

	struct KioskWindowState {
		HWND targetWindow = NULL;
		WINDOWPLACEMENT placement{};
		bool hadPlacement = false;
	};

	SageLockMode g_SelectedMode = SageLockMode::ScreenLock;
	UINT g_TaskbarCreatedMessage = 0;
	NOTIFYICONDATAW g_TrayIconData{};
	bool g_TrayIconAdded = false;
	bool g_KioskActive = false;
	KioskWindowState g_KioskWindowState{};
	bool g_KilledExplorerForKiosk = false;
	bool g_AutoRestartShellChanged = false;
	bool g_AutoRestartShellHadOriginalValue = false;
	DWORD g_AutoRestartShellOriginalValue = 1;
	bool g_KioskWorkAreaChanged = false;
	RECT g_OriginalKioskWorkArea{};

	struct OverlayWindowState {
		bool visible = false;
		bool finalStep = false;
		bool finalLocked = false;
		int currentStep = 0; // 1-4, ignored when finalStep is true
		DWORD64 hideAt = 0;
		DWORD64 finalStart = 0;
	} g_OverlayState{};

	HINSTANCE g_hInstance = nullptr;
	struct OverlayMonitorWindow {
		HWND hWnd = nullptr;
		RECT bounds{};
		int width = 0;
		int height = 0;
	};

	std::vector<OverlayMonitorWindow> g_OverlayWindows;
	std::vector<RECT> g_OverlayMonitorRects;
	HWND g_OverlayTimerWnd = nullptr;
	bool g_OverlayWindowClassRegistered = false;
	ULONG_PTR g_GdiPlusToken = 0;
	bool g_GdiPlusInitialized = false;

	enum OverlayBitmapIndex {
		OverlayBitmapVolumeUp = 0,
		OverlayBitmapVolumeDown = 1,
		OverlayBitmapLock = 2,
		OverlayBitmapUnlock = 3,
		OverlayBitmapCount = 4
	};

	std::array<int, OverlayBitmapCount> g_OverlayImageResources = {
		IDB_OVERLAY_STEP_1,
		IDB_OVERLAY_STEP_2,
		IDB_OVERLAY_LOCK,
		// The PNG itself is intentionally shifted right so the open-lock body aligns with overlay-lock.png.
		IDB_OVERLAY_UNLOCK
	};
	std::array<Bitmap*, OverlayBitmapCount> g_OverlayBitmaps{};
	int g_VolumePatternStep = 0;
	DWORD64 g_LastVolumePatternTick = 0;
	DWORD g_LastAcceptedVolumeVk = 0;
	DWORD64 g_LastAcceptedVolumeTick = 0;
	constexpr DWORD64 VolumeSameKeyDebounceMs = 650;
	bool g_KeyboardVolumeUpDown = false;
	bool g_KeyboardVolumeDownDown = false;
	bool g_ConsumerVolumeUpDown = false;
	bool g_ConsumerVolumeDownDown = false;
	constexpr USHORT HidConsumerUsagePage = 0x000C;
	constexpr USHORT HidConsumerControlUsage = 0x0001;
	constexpr USHORT HidConsumerVolumeIncrement = 0x00E9;
	constexpr USHORT HidConsumerVolumeDecrement = 0x00EA;

	const wchar_t* GetModeName(SageLockMode mode) {
		return mode == SageLockMode::Kiosk ? L"Kiosk Mode" : L"Screen Lock Mode";
	}

	bool NtTerminateProcessHandle(HANDLE process, LONG exitStatus) {
		using NtTerminateProcessProc = LONG(NTAPI*)(HANDLE ProcessHandle, LONG ExitStatus);
		static NtTerminateProcessProc ntTerminateProcess =
			reinterpret_cast<NtTerminateProcessProc>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtTerminateProcess"));

		if (!ntTerminateProcess) {
			return false;
		}

		return ntTerminateProcess(process, exitStatus) >= 0;
	}

	void EnsureGdiPlusInitialized() {
		if (g_GdiPlusInitialized) {
			return;
		}
		GdiplusStartupInput startupInput;
		auto status = GdiplusStartup(&g_GdiPlusToken, &startupInput, NULL);
		if (status == Ok) {
			g_GdiPlusInitialized = true;
		}
	}

	void ShutdownGdiPlus() {
		if (g_GdiPlusInitialized) {
			GdiplusShutdown(g_GdiPlusToken);
			g_GdiPlusInitialized = false;
			g_GdiPlusToken = 0;
		}
	}

	void LoadOverlayBitmaps() {
		HINSTANCE module = g_hInstance ? g_hInstance : GetModuleHandleW(NULL);
		for (size_t i = 0; i < g_OverlayImageResources.size(); i++) {
			const int resourceId = g_OverlayImageResources[i];
			HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), L"PNG");
			if (!resource) {
				g_OverlayBitmaps[i] = nullptr;
				continue;
			}

			DWORD resourceSize = SizeofResource(module, resource);
			HGLOBAL loadedResource = LoadResource(module, resource);
			const void* resourceData = loadedResource ? LockResource(loadedResource) : nullptr;
			if (!resourceSize || !resourceData) {
				g_OverlayBitmaps[i] = nullptr;
				continue;
			}

			HGLOBAL imageMemory = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
			if (!imageMemory) {
				g_OverlayBitmaps[i] = nullptr;
				continue;
			}

			void* imageBuffer = GlobalLock(imageMemory);
			if (!imageBuffer) {
				GlobalFree(imageMemory);
				g_OverlayBitmaps[i] = nullptr;
				continue;
			}

			memcpy(imageBuffer, resourceData, resourceSize);
			GlobalUnlock(imageMemory);

			IStream* imageStream = nullptr;
			if (CreateStreamOnHGlobal(imageMemory, TRUE, &imageStream) != S_OK || !imageStream) {
				GlobalFree(imageMemory);
				g_OverlayBitmaps[i] = nullptr;
				continue;
			}

			auto bitmap = Bitmap::FromStream(imageStream, false);
			imageStream->Release();
			if (!bitmap || bitmap->GetLastStatus() != Ok) {
				delete bitmap;
				g_OverlayBitmaps[i] = nullptr;
			}
			else {
				g_OverlayBitmaps[i] = bitmap;
			}
		}
	}

	void UnloadOverlayBitmaps() {
		for (auto& bmp : g_OverlayBitmaps) {
			delete bmp;
			bmp = nullptr;
		}
	}

	bool SameMonitorLayout(const std::vector<RECT>& monitorRects) {
		if (monitorRects.size() != g_OverlayMonitorRects.size()) {
			return false;
		}

		for (size_t i = 0; i < monitorRects.size(); i++) {
			const auto& current = monitorRects[i];
			const auto& known = g_OverlayMonitorRects[i];
			if (current.left != known.left || current.top != known.top ||
				current.right != known.right || current.bottom != known.bottom) {
				return false;
			}
		}

		return true;
	}

	BOOL CALLBACK CollectMonitorRectProc(HMONITOR hMonitor, HDC, LPRECT, LPARAM lParam) {
		auto monitorRects = reinterpret_cast<std::vector<RECT>*>(lParam);
		MONITORINFO monitorInfo{};
		monitorInfo.cbSize = sizeof(monitorInfo);
		if (GetMonitorInfoW(hMonitor, &monitorInfo)) {
			RECT overlayBounds = monitorInfo.rcWork;
			if (overlayBounds.right <= overlayBounds.left || overlayBounds.bottom <= overlayBounds.top) {
				overlayBounds = monitorInfo.rcMonitor;
			}
			monitorRects->push_back(overlayBounds);
		}
		return TRUE;
	}

	std::vector<RECT> CollectMonitorRects() {
		std::vector<RECT> monitorRects;
		EnumDisplayMonitors(NULL, NULL, CollectMonitorRectProc, reinterpret_cast<LPARAM>(&monitorRects));

		if (monitorRects.empty()) {
			RECT primary{};
			primary.left = 0;
			primary.top = 0;
			primary.right = GetSystemMetrics(SM_CXSCREEN);
			primary.bottom = GetSystemMetrics(SM_CYSCREEN);
			monitorRects.push_back(primary);
		}

		return monitorRects;
	}

	void DrawProgressDots(Graphics& graphics, int activeStep, float dotsY = OverlayDotsY) {
		const int clampedStep = (std::max)(0, (std::min)(activeStep, 4));
		const float radius = 6.0f;
		const float gap = 18.0f;
		const float totalWidth = (radius * 2.0f * 4.0f) + (gap * 3.0f);
		const float startX = (OverlayWindowSize - totalWidth) * 0.5f;
		const float y = dotsY;

		SolidBrush activeBrush(Color(245, 255, 255, 255));
		SolidBrush inactiveBrush(Color(120, 255, 255, 255));
		Pen edgePen(Color(180, 0, 0, 0), 1.0f);

		for (int i = 0; i < 4; i++) {
			const float x = startX + i * ((radius * 2.0f) + gap);
			SolidBrush* brush = i < clampedStep ? &activeBrush : &inactiveBrush;
			graphics.FillEllipse(brush, x, y, radius * 2.0f, radius * 2.0f);
			graphics.DrawEllipse(&edgePen, x, y, radius * 2.0f, radius * 2.0f);
		}
	}

	void DrawFinalStateText(Graphics& graphics, bool locked, float labelTop = OverlayLabelTop) {
		const bool kioskMode = g_SelectedMode == SageLockMode::Kiosk;
		const wchar_t* label = kioskMode ? (locked ? L"Focused" : L"Unfocused") : (locked ? L"Locked" : L"Unlocked");
		const float fontSize = kioskMode ? (locked ? 32.0f : 27.0f) : (locked ? 34.0f : 30.0f);
		Font labelFont(L"Segoe UI", fontSize, FontStyleBold, UnitPixel);
		StringFormat strFormat;
		strFormat.SetAlignment(StringAlignmentCenter);
		strFormat.SetLineAlignment(StringAlignmentCenter);

		RectF shadowRect(2.0f, labelTop + 2.0f, (REAL)OverlayWindowSize, OverlayLabelHeight);
		RectF textRect(0, labelTop, (REAL)OverlayWindowSize, OverlayLabelHeight);
		SolidBrush shadowBrush(Color(210, 0, 0, 0));
		SolidBrush textBrush(Color(255, 255, 255, 255));

		graphics.DrawString(label, -1, &labelFont, shadowRect, &strFormat, &shadowBrush);
		graphics.DrawString(label, -1, &labelFont, textRect, &strFormat, &textBrush);
	}

	void RenderOverlayFrame(const OverlayMonitorWindow& overlayWindow) {
		if (!overlayWindow.hWnd || !g_OverlayState.visible || overlayWindow.width <= 0 || overlayWindow.height <= 0) return;

		HDC screenDC = GetDC(NULL);
		HDC memDC = CreateCompatibleDC(screenDC);
		BITMAPINFO bmpInfo{};
		bmpInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmpInfo.bmiHeader.biWidth = overlayWindow.width;
		bmpInfo.bmiHeader.biHeight = -overlayWindow.height;
		bmpInfo.bmiHeader.biPlanes = 1;
		bmpInfo.bmiHeader.biBitCount = 32;
		bmpInfo.bmiHeader.biCompression = BI_RGB;

		void* bits = nullptr;
		HBITMAP dib = CreateDIBSection(memDC, &bmpInfo, DIB_RGB_COLORS, &bits, NULL, 0);
		if (!dib) {
			DeleteDC(memDC);
			ReleaseDC(NULL, screenDC);
			return;
		}

		HGDIOBJ oldBmp = SelectObject(memDC, dib);
		Graphics graphics(memDC);
		graphics.SetSmoothingMode(SmoothingModeAntiAlias);
		graphics.Clear(Color(0, 0, 0, 0));

		float renderScale = 1.0f;
		if (g_OverlayState.finalStep) {
			auto elapsed = (std::max)(0.0f, float(GetTickCount64() - g_OverlayState.finalStart) / 1000.0f);
			renderScale = 1.0f + 0.05f * std::sin(elapsed * 8.0f);
		}

		const int imageIndex = g_OverlayState.finalStep ?
			(g_OverlayState.finalLocked ? OverlayBitmapLock : OverlayBitmapUnlock) :
			((g_OverlayState.currentStep % 2) == 1 ? OverlayBitmapVolumeUp : OverlayBitmapVolumeDown);
		const float hudX = (overlayWindow.width - OverlayWindowSize) * 0.5f;
		const float hudY = (overlayWindow.height - OverlayWindowSize) * 0.5f;
		const float finalLabelTop = OverlayLabelTop - 10.0f;
		const float progressDotsY = OverlayDotsY + (OverlayIconHeight * 1.05f * 0.05f);
		graphics.TranslateTransform(hudX, hudY);

		if (imageIndex >= 0 && imageIndex < (int)g_OverlayBitmaps.size() && g_OverlayBitmaps[imageIndex]) {
			auto* image = g_OverlayBitmaps[imageIndex];
			const float imgW = (float)image->GetWidth() * renderScale;
			const float imgH = (float)image->GetHeight() * renderScale;
			const float x = (OverlayWindowSize - imgW) * 0.5f;
			const float y = OverlayIconTop + (OverlayIconHeight - imgH) * 0.5f;

			graphics.DrawImage(image, x, y, imgW, imgH);
		}
		DrawProgressDots(graphics, g_OverlayState.currentStep, progressDotsY);
		if (g_OverlayState.finalStep) {
			DrawFinalStateText(graphics, g_OverlayState.finalLocked, finalLabelTop);
		}

		graphics.ResetTransform();

		POINT destination = { overlayWindow.bounds.left, overlayWindow.bounds.top };
		SIZE size = { overlayWindow.width, overlayWindow.height };
		POINT sourcePoint = { 0, 0 };

		BLENDFUNCTION blend{};
		blend.BlendOp = AC_SRC_OVER;
		blend.SourceConstantAlpha = 255;
		blend.AlphaFormat = AC_SRC_ALPHA;

		UpdateLayeredWindow(overlayWindow.hWnd, screenDC, &destination, &size, memDC, &sourcePoint, RGB(0, 0, 0), &blend, ULW_ALPHA);

		SelectObject(memDC, oldBmp);
		DeleteObject(dib);
		DeleteDC(memDC);
		ReleaseDC(NULL, screenDC);
	}

	void RenderOverlayFrames() {
		for (const auto& overlayWindow : g_OverlayWindows) {
			RenderOverlayFrame(overlayWindow);
		}
	}

	void StopOverlayTimer() {
		if (g_OverlayTimerWnd) {
			KillTimer(g_OverlayTimerWnd, OverlayTimerId);
			g_OverlayTimerWnd = nullptr;
		}
	}

	void StartOverlayTimer() {
		if (!g_OverlayTimerWnd && !g_OverlayWindows.empty()) {
			g_OverlayTimerWnd = g_OverlayWindows.front().hWnd;
			SetTimer(g_OverlayTimerWnd, OverlayTimerId, 16, NULL);
		}
	}

	void DestroyOverlayWindows() {
		StopOverlayTimer();

		for (auto& overlayWindow : g_OverlayWindows) {
			if (overlayWindow.hWnd) {
				DestroyWindow(overlayWindow.hWnd);
				overlayWindow.hWnd = nullptr;
			}
		}
		g_OverlayWindows.clear();
		g_OverlayMonitorRects.clear();
	}

	LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

	bool RegisterOverlayWindowClass(HINSTANCE hInstance) {
		if (g_OverlayWindowClassRegistered) {
			return true;
		}

		static const wchar_t* overlayClassName = L"SAGE_LOCK_OVERLAY_WINDOW";

		WNDCLASSEX wc{};
		wc.cbSize = sizeof(WNDCLASSEX);
		wc.lpfnWndProc = OverlayWndProc;
		wc.hInstance = hInstance;
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.lpszClassName = overlayClassName;

		if (!RegisterClassEx(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
			return false;
		}

		g_OverlayWindowClassRegistered = true;
		return true;
	}

	void EnsureOverlayWindowsForCurrentMonitors(HINSTANCE hInstance) {
		auto monitorRects = CollectMonitorRects();
		if (SameMonitorLayout(monitorRects) && g_OverlayWindows.size() == monitorRects.size()) {
			return;
		}

		DestroyOverlayWindows();

		if (!RegisterOverlayWindowClass(hInstance)) {
			return;
		}

		static const wchar_t* overlayClassName = L"SAGE_LOCK_OVERLAY_WINDOW";
		for (const auto& bounds : monitorRects) {
			OverlayMonitorWindow overlayWindow;
			overlayWindow.bounds = bounds;
			overlayWindow.width = bounds.right - bounds.left;
			overlayWindow.height = bounds.bottom - bounds.top;

			if (overlayWindow.width <= 0 || overlayWindow.height <= 0) {
				continue;
			}

			overlayWindow.hWnd = CreateWindowEx(
				WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
				overlayClassName,
				L"",
				WS_POPUP,
				bounds.left, bounds.top, overlayWindow.width, overlayWindow.height,
				NULL, NULL, hInstance, NULL
			);

			if (overlayWindow.hWnd) {
				ShowWindow(overlayWindow.hWnd, SW_HIDE);
				g_OverlayWindows.push_back(overlayWindow);
			}
		}

		g_OverlayMonitorRects = monitorRects;
	}

	void ShowOverlayWindows() {
		for (const auto& overlayWindow : g_OverlayWindows) {
			SetWindowPos(overlayWindow.hWnd, HWND_TOPMOST, overlayWindow.bounds.left, overlayWindow.bounds.top,
				overlayWindow.width, overlayWindow.height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
			ShowWindow(overlayWindow.hWnd, SW_SHOWNOACTIVATE);
		}
	}

	void ShowSequenceOverlay(int step, bool finalStep, bool finalLocked = false) {
		if (step < 1 || step > 4) {
			return;
		}

		EnsureOverlayWindowsForCurrentMonitors(g_hInstance);
		if (g_OverlayWindows.empty()) {
			return;
		}

		g_OverlayState.visible = true;
		g_OverlayState.currentStep = step;
		g_OverlayState.finalStep = finalStep;
		g_OverlayState.finalLocked = finalLocked;
		g_OverlayState.finalStart = finalStep ? GetTickCount64() : 0;
		g_OverlayState.hideAt = GetTickCount64() + (finalStep ? OverlayLockMs : OverlayStepMs);

		ShowOverlayWindows();
		RenderOverlayFrames();
		StartOverlayTimer();
	}

	void HideSequenceOverlay() {
		g_OverlayState.visible = false;
		StopOverlayTimer();

		for (const auto& overlayWindow : g_OverlayWindows) {
			ShowWindow(overlayWindow.hWnd, SW_HIDE);
		}
	}

	void HandleSequenceOverlayTick() {
		if (!g_OverlayState.visible) return;
		DWORD64 now = GetTickCount64();
		if (now > g_OverlayState.hideAt) {
			HideSequenceOverlay();
			return;
		}
		RenderOverlayFrames();
	}

	bool UpdateSequenceProgress(DWORD vk) {
		const DWORD expected[4] = { VK_VOLUME_UP, VK_VOLUME_DOWN, VK_VOLUME_UP, VK_VOLUME_DOWN };
		const auto now = GetTickCount64();

		if ((now - g_LastVolumePatternTick) > 800) {
			g_VolumePatternStep = 0;
		}
		g_LastVolumePatternTick = now;

		if (g_VolumePatternStep < 4 && vk == expected[g_VolumePatternStep]) {
			g_VolumePatternStep++;
		}
		else {
			g_VolumePatternStep = (vk == VK_VOLUME_UP) ? 1 : 0;
		}

		g_VolumePatternStep = std::min(g_VolumePatternStep, 4);
		if (g_VolumePatternStep > 0) {
			ShowSequenceOverlay(g_VolumePatternStep, false);
		}
		else {
			HideSequenceOverlay();
		}

		return g_VolumePatternStep == 4;
	}

	bool TryGetVolumeKeyFromConsumerHid(const RAWINPUT* eventInfo, DWORD& vkKey) {
		if (eventInfo->header.dwType != RIM_TYPEHID) {
			return false;
		}

		const RAWHID& hid = eventInfo->data.hid;

		bool reportHasVolumeUp = false;
		bool reportHasVolumeDown = false;
		for (DWORD reportIndex = 0; reportIndex < hid.dwCount; reportIndex++) {
			const BYTE* report = hid.bRawData + (reportIndex * hid.dwSizeHid);
			for (DWORD i = 0; i < hid.dwSizeHid; i++) {
				USHORT oneByteUsage = report[i];
				if (oneByteUsage == HidConsumerVolumeIncrement) {
					reportHasVolumeUp = true;
				}
				if (oneByteUsage == HidConsumerVolumeDecrement) {
					reportHasVolumeDown = true;
				}

				if (i + 1 < hid.dwSizeHid) {
					USHORT twoByteUsage = (USHORT)(report[i] | (report[i + 1] << 8));
					if (twoByteUsage == HidConsumerVolumeIncrement) {
						reportHasVolumeUp = true;
					}
					if (twoByteUsage == HidConsumerVolumeDecrement) {
						reportHasVolumeDown = true;
					}
				}
			}
		}

		bool volumeUpPress = reportHasVolumeUp && !g_ConsumerVolumeUpDown;
		bool volumeDownPress = reportHasVolumeDown && !g_ConsumerVolumeDownDown;

		g_ConsumerVolumeUpDown = reportHasVolumeUp;
		g_ConsumerVolumeDownDown = reportHasVolumeDown;

		if (volumeUpPress) {
			vkKey = VK_VOLUME_UP;
			return true;
		}
		if (volumeDownPress) {
			vkKey = VK_VOLUME_DOWN;
			return true;
		}

		return false;
	}

	LRESULT CALLBACK OverlayWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
		switch (uMsg) {
		case WM_TIMER:
			HandleSequenceOverlayTick();
			return 0;
		case WM_NCHITTEST:
			return HTTRANSPARENT;
		case WM_ERASEBKGND:
			return 1;
		}
		return DefWindowProc(hWnd, uMsg, wParam, lParam);
	}

}

// GLOBALS TO TRACK VOLUME UP DOWN UP DOWN EVENTS
bool lock_enabled = false;
std::vector<std::wstring> g_TouchDeviceIds;

void ToggleTouchDevice(const wchar_t* deviceId, bool enable) {
	DEVINST devInst = 0;
	CONFIGRET locateCr = CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(deviceId), CM_LOCATE_DEVNODE_NORMAL);
	if (locateCr != CR_SUCCESS) {
		return;
	}

	if (enable) {
		CM_Enable_DevNode(devInst, 0);
	}
	else {
		CM_Disable_DevNode(devInst, 0);
	}
}

void AddTouchDeviceId(const std::wstring& deviceId) {
	if (deviceId.empty()) {
		return;
	}

	if (std::find(g_TouchDeviceIds.begin(), g_TouchDeviceIds.end(), deviceId) != g_TouchDeviceIds.end()) {
		return;
	}

	g_TouchDeviceIds.push_back(deviceId);
}

void RefreshTouchDeviceIdsFromHidCaps()
{
	g_TouchDeviceIds.clear();

	HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVINTERFACE_HID, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
	if (deviceInfoSet == INVALID_HANDLE_VALUE) {
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
				HIDP_CAPS caps{};
				if (HidD_GetPreparsedData(deviceHandle, &preparsedData) == TRUE)
				{
					// filter for touch-screen type devices
					if (HidP_GetCaps(preparsedData, &caps) == HIDP_STATUS_SUCCESS &&
						caps.UsagePage == HID_USAGE_PAGE_DIGITIZER &&
						(caps.Usage == HID_USAGE_DIGITIZER_HEAT_MAP || // surface pro touch screen device is heat_map type
							caps.Usage == HID_USAGE_DIGITIZER_TOUCH_SCREEN ||
							caps.Usage == HID_USAGE_DIGITIZER_MULTI_POINT))
					{
						WCHAR deviceId[MAX_DEVICE_ID_LEN]{};
						if (CM_Get_Device_IDW(devInfoData.DevInst, deviceId, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
							AddTouchDeviceId(deviceId);
						}
					}
					HidD_FreePreparsedData(preparsedData);
				}

				CloseHandle(deviceHandle);
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

void UpdateTrayIcon() {
	if (!g_TrayIconAdded) {
		return;
	}

	swprintf_s(g_TrayIconData.szTip, L"SageLock - Mode: %s", GetModeName(g_SelectedMode));
	g_TrayIconData.uFlags = NIF_TIP;
	Shell_NotifyIconW(NIM_MODIFY, &g_TrayIconData);
}

bool AddTrayIcon(HWND hWnd) {
	if (g_TrayIconAdded) {
		return true;
	}

	ZeroMemory(&g_TrayIconData, sizeof(g_TrayIconData));
	g_TrayIconData.cbSize = sizeof(g_TrayIconData);
	g_TrayIconData.hWnd = hWnd;
	g_TrayIconData.uID = TrayIconId;
	g_TrayIconData.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
	g_TrayIconData.uCallbackMessage = TrayMessageId;
	g_TrayIconData.hIcon = LoadIconW(g_hInstance ? g_hInstance : GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_SAGE_LOCK));
	if (!g_TrayIconData.hIcon) {
		g_TrayIconData.hIcon = LoadIconW(NULL, IDI_APPLICATION);
	}
	swprintf_s(g_TrayIconData.szTip, L"SageLock - Mode: %s", GetModeName(g_SelectedMode));

	g_TrayIconAdded = Shell_NotifyIconW(NIM_ADD, &g_TrayIconData) == TRUE;
	return g_TrayIconAdded;
}

void RemoveTrayIcon() {
	if (!g_TrayIconAdded) {
		return;
	}

	Shell_NotifyIconW(NIM_DELETE, &g_TrayIconData);
	g_TrayIconAdded = false;
	ZeroMemory(&g_TrayIconData, sizeof(g_TrayIconData));
}

void ReaddTrayIcon(HWND hWnd) {
	g_TrayIconAdded = false;
	AddTrayIcon(hWnd);
	UpdateTrayIcon();
}

void ShowTrayMenu(HWND hWnd) {
	HMENU menu = CreatePopupMenu();
	if (!menu) {
		return;
	}

	AppendMenuW(menu, MF_STRING | (g_SelectedMode == SageLockMode::ScreenLock ? MF_CHECKED : MF_UNCHECKED),
		TrayMenuScreenLockModeId, L"Screen Lock Mode (Disable Touch Screen)");
	AppendMenuW(menu, MF_STRING | (g_SelectedMode == SageLockMode::Kiosk ? MF_CHECKED : MF_UNCHECKED),
		TrayMenuKioskModeId, L"Kiosk Mode (Exclusive App)");
	AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
	AppendMenuW(menu, MF_STRING, TrayMenuCloseId, L"Close SageLock");

	SetForegroundWindow(hWnd);
	POINT cursor{};
	GetCursorPos(&cursor);
	TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, cursor.x, cursor.y, 0, hWnd, NULL);
	PostMessageW(hWnd, WM_NULL, 0, 0);
	DestroyMenu(menu);
}

void TerminateExplorerProcesses() {
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snapshot == INVALID_HANDLE_VALUE) {
		return;
	}

	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	if (Process32FirstW(snapshot, &entry)) {
		do {
			if (_wcsicmp(entry.szExeFile, L"explorer.exe") != 0) {
				continue;
			}

			HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, entry.th32ProcessID);
			if (!process) {
				continue;
			}

			if (NtTerminateProcessHandle(process, 0)) {
				g_KilledExplorerForKiosk = true;
			}
			CloseHandle(process);
		} while (Process32NextW(snapshot, &entry));
	}
	CloseHandle(snapshot);
}

bool SetAutoRestartShellDisabled() {
	if (g_AutoRestartShellChanged) {
		return true;
	}

	HKEY key = NULL;
	LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
		0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
	if (status != ERROR_SUCCESS) {
		return false;
	}

	DWORD value = 0;
	DWORD valueSize = sizeof(value);
	DWORD valueType = 0;
	status = RegQueryValueExW(key, L"AutoRestartShell", NULL, &valueType, reinterpret_cast<LPBYTE>(&value), &valueSize);
	g_AutoRestartShellHadOriginalValue = status == ERROR_SUCCESS && valueType == REG_DWORD && valueSize == sizeof(value);
	if (g_AutoRestartShellHadOriginalValue) {
		g_AutoRestartShellOriginalValue = value;
	}
	else {
		g_AutoRestartShellOriginalValue = 1;
	}

	DWORD disabled = 0;
	status = RegSetValueExW(key, L"AutoRestartShell", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&disabled), sizeof(disabled));
	RegCloseKey(key);
	if (status != ERROR_SUCCESS) {
		return false;
	}

	g_AutoRestartShellChanged = true;
	return true;
}

void RestoreAutoRestartShell() {
	if (!g_AutoRestartShellChanged) {
		return;
	}

	HKEY key = NULL;
	LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
		L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
		0, KEY_SET_VALUE, &key);
	if (status != ERROR_SUCCESS) {
		return;
	}

	if (g_AutoRestartShellHadOriginalValue) {
		status = RegSetValueExW(key, L"AutoRestartShell", 0, REG_DWORD,
			reinterpret_cast<const BYTE*>(&g_AutoRestartShellOriginalValue), sizeof(g_AutoRestartShellOriginalValue));
	}
	else {
		status = RegDeleteValueW(key, L"AutoRestartShell");
		if (status == ERROR_FILE_NOT_FOUND) {
			status = ERROR_SUCCESS;
		}
	}
	RegCloseKey(key);

	if (status == ERROR_SUCCESS) {
		g_AutoRestartShellChanged = false;
	}
}

void RestartExplorerIfNeeded() {
	const bool shouldRestartExplorer = g_KilledExplorerForKiosk;
	RestoreAutoRestartShell();
	if (!shouldRestartExplorer) {
		return;
	}

	STARTUPINFOW startupInfo{};
	startupInfo.cb = sizeof(startupInfo);
	PROCESS_INFORMATION processInfo{};
	wchar_t commandLine[] = L"explorer.exe";
	if (CreateProcessW(NULL, commandLine, NULL, NULL, FALSE, 0, NULL, NULL, &startupInfo, &processInfo)) {
		CloseHandle(processInfo.hThread);
		CloseHandle(processInfo.hProcess);
	}
	g_KilledExplorerForKiosk = false;
}

bool SetKioskWorkAreaToFullMonitor(HWND targetWindow) {
	if (g_KioskWorkAreaChanged) {
		return true;
	}

	RECT originalWorkArea{};
	if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &originalWorkArea, 0)) {
		return false;
	}

	HMONITOR monitor = MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST);
	MONITORINFO monitorInfo{};
	monitorInfo.cbSize = sizeof(monitorInfo);
	if (!GetMonitorInfoW(monitor, &monitorInfo)) {
		return false;
	}

	RECT fullMonitorWorkArea = monitorInfo.rcMonitor;
	if (!SystemParametersInfoW(SPI_SETWORKAREA, 0, &fullMonitorWorkArea, SPIF_SENDCHANGE)) {
		return false;
	}

	g_OriginalKioskWorkArea = originalWorkArea;
	g_KioskWorkAreaChanged = true;
	return true;
}

void RestoreKioskWorkArea() {
	if (!g_KioskWorkAreaChanged) {
		return;
	}

	RECT originalWorkArea = g_OriginalKioskWorkArea;
	if (SystemParametersInfoW(SPI_SETWORKAREA, 0, &originalWorkArea, SPIF_SENDCHANGE)) {
		g_KioskWorkAreaChanged = false;
	}
}

bool IsSageLockOwnedWindow(HWND hWnd) {
	wchar_t className[128]{};
	GetClassNameW(hWnd, className, (int)_countof(className));
	return wcscmp(className, L"RECV_RAW_INPT") == 0 || wcscmp(className, L"SAGE_LOCK_OVERLAY_WINDOW") == 0;
}

BOOL CALLBACK MinimizeWindowsExceptProc(HWND hWnd, LPARAM lParam) {
	HWND targetWindow = reinterpret_cast<HWND>(lParam);
	if (hWnd == targetWindow || !IsWindowVisible(hWnd) || GetWindow(hWnd, GW_OWNER) || IsSageLockOwnedWindow(hWnd)) {
		return TRUE;
	}

	ShowWindow(hWnd, SW_MINIMIZE);
	return TRUE;
}

void MinimizeWindowsExcept(HWND targetWindow) {
	EnumWindows(MinimizeWindowsExceptProc, reinterpret_cast<LPARAM>(targetWindow));
}

void MaximizeKioskWindowToWorkArea() {
	if (!g_KioskActive || !IsWindow(g_KioskWindowState.targetWindow)) {
		return;
	}

	ShowWindow(g_KioskWindowState.targetWindow, SW_RESTORE);
	ShowWindow(g_KioskWindowState.targetWindow, SW_MAXIMIZE);
	SetForegroundWindow(g_KioskWindowState.targetWindow);
}

bool EnterKioskMode() {
	HWND foregroundWindow = GetForegroundWindow();
	if (!foregroundWindow || IsSageLockOwnedWindow(foregroundWindow)) {
		return false;
	}

	g_KioskWindowState = {};
	g_KioskWindowState.targetWindow = foregroundWindow;
	g_KioskWindowState.placement.length = sizeof(g_KioskWindowState.placement);
	g_KioskWindowState.hadPlacement = GetWindowPlacement(foregroundWindow, &g_KioskWindowState.placement) == TRUE;

	if (!SetAutoRestartShellDisabled()) {
		g_KioskWindowState = {};
		return false;
	}

	g_KioskActive = true;
	MinimizeWindowsExcept(foregroundWindow);
	TerminateExplorerProcesses();
	SetKioskWorkAreaToFullMonitor(foregroundWindow);
	MaximizeKioskWindowToWorkArea();
	return true;
}

void ExitKioskMode() {
	if (!g_KioskActive) {
		return;
	}

	HWND targetWindow = g_KioskWindowState.targetWindow;
	if (IsWindow(targetWindow)) {
		if (g_KioskWindowState.hadPlacement) {
			SetWindowPlacement(targetWindow, &g_KioskWindowState.placement);
		}
	}

	RestoreKioskWorkArea();
	RestartExplorerIfNeeded();
	g_KioskWindowState = {};
	g_KioskActive = false;
}

void DisableScreenLockMode() {
	if (!lock_enabled) {
		return;
	}

	lock_enabled = false;
	for (const auto& screen : g_TouchDeviceIds) {
		ToggleTouchDevice(screen.c_str(), true);
	}
	SoundEffect(true);
}

void SelectSageLockMode(SageLockMode mode) {
	if (g_SelectedMode == mode) {
		return;
	}

	DisableScreenLockMode();
	ExitKioskMode();
	g_SelectedMode = mode;
	UpdateTrayIcon();
}

void CloseSageLock(HWND hWnd) {
	DisableScreenLockMode();
	ExitKioskMode();
	RemoveTrayIcon();
	DestroyWindow(hWnd);
	PostQuitMessage(0);
}

void CompleteLockSequence() {
	bool finalLocked = false;
	if (g_SelectedMode == SageLockMode::ScreenLock) {
		lock_enabled = !lock_enabled;
		const bool enableTouch = !lock_enabled;
		for (const auto& screen : g_TouchDeviceIds) {
			ToggleTouchDevice(screen.c_str(), enableTouch);
		}
		SoundEffect(enableTouch);
		finalLocked = lock_enabled;
	}
	else {
		if (g_KioskActive) {
			ExitKioskMode();
			SoundEffect(true);
			finalLocked = false;
		}
		else {
			finalLocked = EnterKioskMode();
			SoundEffect(!finalLocked);
		}
	}

	g_VolumePatternStep = 0;
	ShowSequenceOverlay(4, true, finalLocked);
	UpdateTrayIcon();
}

void HandleVolumeKeyEvent(DWORD vkKey) {
	DWORD64 now = GetTickCount64();

	if (g_LastAcceptedVolumeVk == vkKey && (now - g_LastAcceptedVolumeTick) < VolumeSameKeyDebounceMs) {
		return;
	}

	g_LastAcceptedVolumeVk = vkKey;
	g_LastAcceptedVolumeTick = now;
	if (UpdateSequenceProgress(vkKey)) {
		CompleteLockSequence();
	}
}

void HandleKeyboardRawVolumeInput(const RAWKEYBOARD& keyboard) {
	if (keyboard.VKey != VK_VOLUME_UP && keyboard.VKey != VK_VOLUME_DOWN) {
		return;
	}

	bool* keyDownState = keyboard.VKey == VK_VOLUME_UP ? &g_KeyboardVolumeUpDown : &g_KeyboardVolumeDownDown;
	if (keyboard.Message == WM_KEYUP || keyboard.Message == WM_SYSKEYUP) {
		*keyDownState = false;
		return;
	}

	if (keyboard.Message != WM_KEYDOWN && keyboard.Message != WM_SYSKEYDOWN) {
		return;
	}

	if (*keyDownState) {
		return;
	}

	*keyDownState = true;
	HandleVolumeKeyEvent(keyboard.VKey);
}

LRESULT CALLBACK pWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	if (g_TaskbarCreatedMessage != 0 && uMsg == g_TaskbarCreatedMessage) {
		ReaddTrayIcon(hWnd);
		return 0;
	}

	if (uMsg == WM_INPUT) {
		UINT dwSize = 0;
		GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));
		if (dwSize > 0) {
			std::vector<BYTE> lpb(dwSize);
			auto eventInfo = reinterpret_cast<RAWINPUT*>(lpb.data());
			if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb.data(), &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
				if (eventInfo->header.dwType == RIM_TYPEKEYBOARD) {
					HandleKeyboardRawVolumeInput(eventInfo->data.keyboard);
				}
				else {
					DWORD vkKey = 0;
					if (TryGetVolumeKeyFromConsumerHid(eventInfo, vkKey)) {
						HandleVolumeKeyEvent(vkKey);
					}
				}
			}
		}
	}
	else if (uMsg == WM_COMMAND) {
		switch (LOWORD(wParam)) {
		case TrayMenuScreenLockModeId:
			SelectSageLockMode(SageLockMode::ScreenLock);
			return 0;
		case TrayMenuKioskModeId:
			SelectSageLockMode(SageLockMode::Kiosk);
			return 0;
		case TrayMenuCloseId:
			CloseSageLock(hWnd);
			return 0;
		}
	}
	else if (uMsg == TrayMessageId) {
		if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU || lParam == WM_LBUTTONUP) {
			ShowTrayMenu(hWnd);
			return 0;
		}
	}
	else if (uMsg == WM_APPCOMMAND) {
		const int command = GET_APPCOMMAND_LPARAM(lParam);
		if (command == APPCOMMAND_VOLUME_UP) {
			HandleVolumeKeyEvent(VK_VOLUME_UP);
			return TRUE;
		}
		if (command == APPCOMMAND_VOLUME_DOWN) {
			HandleVolumeKeyEvent(VK_VOLUME_DOWN);
			return TRUE;
		}
	}
	else if (uMsg == WM_DESTROY) {
		DisableScreenLockMode();
		ExitKioskMode();
		RemoveTrayIcon();
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

DWORD WINAPI InputEventThread(LPVOID lpParameter) {
	g_hInstance = GetModuleHandle(NULL);
	EnsureGdiPlusInitialized();
	LoadOverlayBitmaps();
	g_TaskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

	static const wchar_t* winClassName = L"RECV_RAW_INPT";
	WNDCLASSEX wx = {};
	wx.cbSize = sizeof(WNDCLASSEX);
	wx.lpfnWndProc = pWndProc; // Callback to handle messages
	wx.hInstance = GetModuleHandle(NULL);
	wx.lpszClassName = winClassName;
	HWND hWnd = NULL;
	if (RegisterClassEx(&wx)) {
		hWnd = CreateWindowEx(0, winClassName, L"IOInptWin", WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, wx.hInstance, NULL);
	}

	RAWINPUTDEVICE Rid[2]; // keyboard plus HID consumer-control devices
	Rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
	Rid[0].usUsage = HID_USAGE_GENERIC_KEYBOARD;
	Rid[0].dwFlags = RIDEV_INPUTSINK;
	Rid[0].hwndTarget = hWnd;
	Rid[1].usUsagePage = HidConsumerUsagePage;
	Rid[1].usUsage = HidConsumerControlUsage;
	Rid[1].dwFlags = RIDEV_INPUTSINK;
	Rid[1].hwndTarget = hWnd;
	if (hWnd) {
		AddTrayIcon(hWnd);
		RegisterRawInputDevices(Rid, 2, sizeof(Rid[0]));
	}

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	DestroyOverlayWindows();
	RemoveTrayIcon();
	UnloadOverlayBitmaps();
	ShutdownGdiPlus();
	return 0;
}

// CheckIfAlreadyRunning is a function that installs a global mutex and checks if it already exists
// if it does, it means that the program is already running and we should exit
bool CheckIfAlreadyRunning() {
	static HANDLE hMutex = NULL;
	hMutex = CreateMutex(NULL, TRUE, L"Global\\SAGE_LOCK_INSTANCE");
	if (!hMutex) {
		return false;
	}

	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		CloseHandle(hMutex);
		hMutex = NULL;
		return true;
	}
	return false;
}

int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
{
	UNREFERENCED_PARAMETER(hPrevInstance);
	UNREFERENCED_PARAMETER(lpCmdLine);
	UNREFERENCED_PARAMETER(nShowCmd);

	g_hInstance = hInstance;

	if (CheckIfAlreadyRunning()) {
		MessageBoxW(NULL, L"SageLock is already running", L"SageLock", MB_OK | MB_ICONERROR);
		return 0;
	}

	RefreshTouchDeviceIdsFromHidCaps();

	HANDLE hInputThread = CreateThread(NULL, NULL, InputEventThread, NULL, NULL, NULL);
	if (!hInputThread) {
		return 1;
	}

	WaitForSingleObject(hInputThread, INFINITE);
	CloseHandle(hInputThread);
	return 0;
}

