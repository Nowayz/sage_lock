/////////////
// sage_lock.cpp : This file contains code to lock HID touch devices when a volume up/down pattern is detected.
// Author: Phillip McNallen (2023)
//////

#ifndef NOMINMAX
#define NOMINMAX
#endif
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
#include <algorithm>
#include <array>
#include <cmath>
#include <gdiplus.h>

using namespace Gdiplus;

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "Cfgmgr32.lib")
#pragma comment(lib, "Winmm.lib")
#pragma comment(lib, "gdiplus.lib")

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

	std::array<std::wstring, OverlayBitmapCount> g_OverlayImageFiles = {
		L"overlay-step-1.png",
		L"overlay-step-2.png",
		L"overlay-lock.png",
		L"overlay-unlock.png"
	};
	std::array<Bitmap*, OverlayBitmapCount> g_OverlayBitmaps{};
	int g_VolumePatternStep = 0;
	DWORD64 g_LastVolumePatternTick = 0;
	DWORD g_LastAcceptedVolumeVk = 0;
	DWORD64 g_LastAcceptedVolumeTick = 0;
	DWORD64 g_LastSuppressedVolumeLogTick = 0;
	constexpr DWORD64 VolumeSameKeyDebounceMs = 650;
	bool g_KeyboardVolumeUpDown = false;
	bool g_KeyboardVolumeDownDown = false;
	bool g_ConsumerVolumeUpDown = false;
	bool g_ConsumerVolumeDownDown = false;
	constexpr USHORT HidConsumerUsagePage = 0x000C;
	constexpr USHORT HidConsumerControlUsage = 0x0001;
	constexpr USHORT HidConsumerVolumeIncrement = 0x00E9;
	constexpr USHORT HidConsumerVolumeDecrement = 0x00EA;

	std::wstring GetExecutableDir() {
		wchar_t exePath[MAX_PATH];
		GetModuleFileNameW(NULL, exePath, MAX_PATH);
		auto path = std::wstring(exePath);
		auto pos = path.find_last_of(L"\\/");
		if (pos == std::wstring::npos) {
			return path;
		}
		return path.substr(0, pos);
	}

	void tracelog(const wchar_t* format, ...) {
		wchar_t buffer[4096];
		va_list args;
		va_start(args, format);
		vswprintf_s(buffer, 4096, format, args);
		va_end(args);

		dbgprint(L"%s", buffer);

#ifdef _DEBUG
		HANDLE hStdout = GetStdHandle(STD_OUTPUT_HANDLE);
		if (hStdout && hStdout != INVALID_HANDLE_VALUE) {
			DWORD consoleWritten = 0;
			WriteConsoleW(hStdout, buffer, lstrlenW(buffer), &consoleWritten, NULL);
		}

		auto logPath = GetExecutableDir() + L"\\sage_lock-debug.log";
		HANDLE hLog = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
			NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hLog == INVALID_HANDLE_VALUE) {
			return;
		}

		char utf8[8192];
		int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, -1, utf8, sizeof(utf8), NULL, NULL);
		if (bytes > 1) {
			DWORD written = 0;
			WriteFile(hLog, utf8, bytes - 1, &written, NULL);
		}
		CloseHandle(hLog);
#endif
	}

	void EnsureGdiPlusInitialized() {
		if (g_GdiPlusInitialized) {
			return;
		}
		GdiplusStartupInput startupInput;
		auto status = GdiplusStartup(&g_GdiPlusToken, &startupInput, NULL);
		if (status == Ok) {
			g_GdiPlusInitialized = true;
			tracelog(L"GDI+ initialized.\n");
		}
		else {
			tracelog(L"GDI+ failed to initialize: %d\n", status);
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
		const auto exeDir = GetExecutableDir();
		for (size_t i = 0; i < g_OverlayImageFiles.size(); i++) {
			auto fullPath = exeDir + L"\\" + g_OverlayImageFiles[i];
			auto bitmap = Bitmap::FromFile(fullPath.c_str(), false);
			if (!bitmap || bitmap->GetLastStatus() != Ok) {
				tracelog(L"Could not load overlay image: %s\n", fullPath.c_str());
				delete bitmap;
				g_OverlayBitmaps[i] = nullptr;
			}
			else {
				g_OverlayBitmaps[i] = bitmap;
				tracelog(L"Loaded overlay image: %s\n", fullPath.c_str());
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

	void DrawLockFallback(Graphics& graphics, float scale) {
		const float cx = OverlayWindowSize / 2.0f;
		const float cy = OverlayWindowSize / 2.0f;
		const float padW = 74.0f * scale;
		const float padH = 58.0f * scale;
		const float lockY = cy - 10.0f * scale;

		SolidBrush body(Color(255, 245, 245, 245));
		Pen shacklePen(Color(255, 245, 245, 245), 10.0f * scale);

		graphics.FillRectangle(&body, RectF(cx - padW / 2.0f, lockY, padW, padH));
		graphics.FillRectangle(&body, cx - padW / 4.0f, lockY + padH - 9.0f, padW / 2.0f, 14.0f * scale);
		graphics.DrawArc(&shacklePen, cx - padW / 2.0f + 6.0f, lockY - 22.0f, padW - 12.0f, 36.0f * scale, 0.0f, -180.0f);
	}

	void DrawFallbackStep(Graphics& graphics, int step, bool finalStep, float scale) {
		const float cx = OverlayWindowSize / 2.0f;
		const float cy = OverlayWindowSize / 2.0f;

		SolidBrush halo(Color(200, 0, 0, 0));
		graphics.FillEllipse(&halo, 34.0f, 34.0f, OverlayWindowSize - 68.0f, OverlayWindowSize - 68.0f);
		Pen edge(Color(220, 220, 220, 220), 4.0f);
		graphics.DrawEllipse(&edge, 34.0f, 34.0f, OverlayWindowSize - 68.0f, OverlayWindowSize - 68.0f);

		if (finalStep) {
			DrawLockFallback(graphics, scale);
			return;
		}

		wchar_t stepText[8];
		swprintf_s(stepText, L"%d", step);
		Font stepFont(L"Segoe UI", 120.0f, FontStyleBold, UnitPixel);
		StringFormat strFormat;
		strFormat.SetAlignment(StringAlignmentCenter);
		strFormat.SetLineAlignment(StringAlignmentCenter);
		SolidBrush textBrush(Color(255, 245, 245, 245));
		RectF textRect(0, 0, (REAL)OverlayWindowSize, (REAL)OverlayWindowSize);
		graphics.DrawString(stepText, -1, &stepFont, textRect, &strFormat, &textBrush);
	}

	void DrawProgressDots(Graphics& graphics, int activeStep) {
		const int clampedStep = (std::max)(0, (std::min)(activeStep, 4));
		const float radius = 6.0f;
		const float gap = 18.0f;
		const float totalWidth = (radius * 2.0f * 4.0f) + (gap * 3.0f);
		const float startX = (OverlayWindowSize - totalWidth) * 0.5f;
		const float y = OverlayDotsY;

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

	void DrawFinalStateText(Graphics& graphics, bool locked) {
		const wchar_t* label = locked ? L"Locked" : L"Unlocked";
		Font labelFont(L"Segoe UI", locked ? 34.0f : 30.0f, FontStyleBold, UnitPixel);
		StringFormat strFormat;
		strFormat.SetAlignment(StringAlignmentCenter);
		strFormat.SetLineAlignment(StringAlignmentCenter);

		RectF shadowRect(2.0f, OverlayLabelTop + 2.0f, (REAL)OverlayWindowSize, OverlayLabelHeight);
		RectF textRect(0, OverlayLabelTop, (REAL)OverlayWindowSize, OverlayLabelHeight);
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
			renderScale = 1.0f + 0.12f * std::sin(elapsed * 8.0f);
		}

		const int imageIndex = g_OverlayState.finalStep ?
			(g_OverlayState.finalLocked ? OverlayBitmapLock : OverlayBitmapUnlock) :
			((g_OverlayState.currentStep % 2) == 1 ? OverlayBitmapVolumeUp : OverlayBitmapVolumeDown);
		const float hudX = (overlayWindow.width - OverlayWindowSize) * 0.5f;
		const float hudY = (overlayWindow.height - OverlayWindowSize) * 0.5f;
		graphics.TranslateTransform(hudX, hudY);

		if (imageIndex >= 0 && imageIndex < (int)g_OverlayBitmaps.size() && g_OverlayBitmaps[imageIndex]) {
			auto* image = g_OverlayBitmaps[imageIndex];
			const float imgW = (float)image->GetWidth() * renderScale;
			const float imgH = (float)image->GetHeight() * renderScale;
			const float x = (OverlayWindowSize - imgW) * 0.5f;
			const float y = OverlayIconTop + (OverlayIconHeight - imgH) * 0.5f;
			graphics.DrawImage(image, x, y, imgW, imgH);
		}
		else {
			DrawFallbackStep(graphics, g_OverlayState.currentStep, g_OverlayState.finalStep, renderScale);
		}

		DrawProgressDots(graphics, g_OverlayState.currentStep);
		if (g_OverlayState.finalStep) {
			DrawFinalStateText(graphics, g_OverlayState.finalLocked);
		}

		graphics.ResetTransform();

		POINT destination = { overlayWindow.bounds.left, overlayWindow.bounds.top };
		SIZE size = { overlayWindow.width, overlayWindow.height };
		POINT sourcePoint = { 0, 0 };

		BLENDFUNCTION blend{};
		blend.BlendOp = AC_SRC_OVER;
		blend.SourceConstantAlpha = 255;
		blend.AlphaFormat = AC_SRC_ALPHA;

		if (!UpdateLayeredWindow(overlayWindow.hWnd, screenDC, &destination, &size, memDC, &sourcePoint, RGB(0, 0, 0), &blend, ULW_ALPHA)) {
			tracelog(L"UpdateLayeredWindow failed: %d\n", GetLastError());
		}

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
			tracelog(L"RegisterClassEx overlay failed: %d\n", GetLastError());
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
			else {
				tracelog(L"CreateWindowEx overlay failed for monitor [%ld,%ld,%ld,%ld]: %d\n",
					bounds.left, bounds.top, bounds.right, bounds.bottom, GetLastError());
			}
		}

		g_OverlayMonitorRects = monitorRects;
		tracelog(L"Overlay windows ready: %zu monitor(s), %zu window(s).\n", monitorRects.size(), g_OverlayWindows.size());
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
			tracelog(L"ShowSequenceOverlay skipped: no overlay windows.\n");
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
		tracelog(L"ShowSequenceOverlay step=%d final=%d windows=%zu.\n", step, finalStep ? 1 : 0, g_OverlayWindows.size());
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

	void UpdateSequenceProgress(DWORD vk) {
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
	}

	void LogRawHidBytes(const RAWHID& hid) {
#ifdef _DEBUG
		wchar_t buffer[1024];
		wchar_t* cursor = buffer;
		size_t remaining = _countof(buffer);

		int written = swprintf_s(cursor, remaining, L"Raw HID input: size=%lu count=%lu data=", hid.dwSizeHid, hid.dwCount);
		if (written < 0) return;
		cursor += written;
		remaining -= written;

		DWORD byteCount = (std::min)(hid.dwSizeHid * hid.dwCount, 32UL);
		for (DWORD i = 0; i < byteCount && remaining > 4; i++) {
			written = swprintf_s(cursor, remaining, L"%02X ", hid.bRawData[i]);
			if (written < 0) break;
			cursor += written;
			remaining -= written;
		}

		swprintf_s(cursor, remaining, L"\n");
		tracelog(L"%s", buffer);
#endif
	}

	bool TryGetVolumeKeyFromConsumerHid(const RAWINPUT* eventInfo, DWORD& vkKey) {
		if (eventInfo->header.dwType != RIM_TYPEHID) {
			return false;
		}

		const RAWHID& hid = eventInfo->data.hid;
		LogRawHidBytes(hid);

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

		if (!reportHasVolumeUp && g_ConsumerVolumeUpDown) {
			tracelog(L"Volume consumer HID release: vk=0x%02X\n", VK_VOLUME_UP);
		}
		if (!reportHasVolumeDown && g_ConsumerVolumeDownDown) {
			tracelog(L"Volume consumer HID release: vk=0x%02X\n", VK_VOLUME_DOWN);
		}

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

		if (reportHasVolumeUp || reportHasVolumeDown) {
			tracelog(L"Volume consumer HID held repeat suppressed.\n");
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
std::array<DWORD, 4> Volume_Event_History{};
WORD Current_Index = 0;
DWORD64 Last_Volume_Event = 0;
int lock_enabled = 0;
std::vector<std::wstring> g_TouchDeviceIds;

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
		Volume_Event_History.fill(0);
	}
	else {
		Current_Index++;
	}
	if (Current_Index > 3) {
		Current_Index = 0;
	}
	return Current_Index;
}

std::wstring ConfigRetToWString(CONFIGRET cr) {
	DWORD win32Error = CM_MapCrToWin32Err(cr, ERROR_GEN_FAILURE);
	wchar_t message[512]{};
	FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		NULL, win32Error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
		message, (DWORD)_countof(message), NULL);

	wchar_t result[768]{};
	swprintf_s(result, L"CONFIGRET=0x%08X Win32=%lu %s", cr, win32Error, message);
	return result;
}

void ToggleTouchDevice(const wchar_t* deviceId, bool enable) {
	DEVINST devInst = 0;
	CONFIGRET locateCr = CM_Locate_DevNodeW(&devInst, const_cast<DEVINSTID_W>(deviceId), CM_LOCATE_DEVNODE_NORMAL);
	if (locateCr != CR_SUCCESS) {
		tracelog(L"CM_Locate_DevNodeW failed for %s: %s\n", deviceId, ConfigRetToWString(locateCr).c_str());
		return;
	}

	CONFIGRET toggleCr = enable ? CM_Enable_DevNode(devInst, 0) : CM_Disable_DevNode(devInst, 0);
	tracelog(L"%s touch device via Configuration Manager: DeviceId=%s Result=%s\n",
		enable ? L"Enabled" : L"Disabled", deviceId, ConfigRetToWString(toggleCr).c_str());
}

void AddTouchDeviceId(const wchar_t* source, const std::wstring& deviceId, USHORT usagePage = 0, USHORT usage = 0) {
	if (deviceId.empty()) {
		return;
	}

	if (std::find(g_TouchDeviceIds.begin(), g_TouchDeviceIds.end(), deviceId) != g_TouchDeviceIds.end()) {
		return;
	}

	g_TouchDeviceIds.push_back(deviceId);
	tracelog(L"Touch target matched via %s: UsagePage=0x%04X Usage=0x%04X Device=%s\n",
		source, usagePage, usage, deviceId.c_str());
}

void RefreshTouchDeviceIdsFromHidCaps()
{
	tracelog(L"Refreshing HID touch device IDs for SageLock toggling.\n");
	g_TouchDeviceIds.clear();

	HDEVINFO deviceInfoSet = SetupDiGetClassDevs(&GUID_DEVINTERFACE_HID, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
	if (deviceInfoSet == INVALID_HANDLE_VALUE) {
		tracelog(L"SetupDiGetClassDevs failed: %s", GetLastErrorAsWString().c_str());
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
						CONFIGRET cr;
						// get string with deviceid 
						WCHAR deviceId[MAX_DEVICE_ID_LEN]{};
						if ((cr = CM_Get_Device_IDW(devInfoData.DevInst, deviceId, MAX_DEVICE_ID_LEN, 0)) != CR_SUCCESS) {
							tracelog(L"CM_Get_Device_IDW failed with error %08X\n", cr);
						}

						AddTouchDeviceId(L"HIDCaps", deviceId, caps.UsagePage, caps.Usage);
					}
					HidD_FreePreparsedData(preparsedData);
				}

				CloseHandle(deviceHandle);
			}
		}
		LocalFree(detailData);
	}
	SetupDiDestroyDeviceInfoList(deviceInfoSet);

	tracelog(L"SageLock toggle target count: %zu\n", g_TouchDeviceIds.size());
	if (g_TouchDeviceIds.empty()) {
		tracelog(L"WARNING: no HID touch device IDs matched; lock sequence will not toggle any devices.\n");
	}
}

void SoundEffect(bool enable)
{
	LPCWSTR soundFile = enable ? L"C:\\Windows\\Media\\Speech On.wav" : L"C:\\Windows\\Media\\Speech Off.wav";
	PlaySound(soundFile, NULL, SND_FILENAME | SND_ASYNC);
}

void SetKbdHistoryIndex(DWORD vkKey) {
	auto i = GetAvailableKbdHistoryIndex();
	Volume_Event_History[i] = vkKey;
	UpdateSequenceProgress(vkKey);
	if ((i == 3) && CheckForVolumeUpDownUpDown()) {
		lock_enabled = !lock_enabled; 
		for (auto screen : g_TouchDeviceIds) {
			ToggleTouchDevice(screen.c_str(), !lock_enabled);
		}
		SoundEffect(!lock_enabled);
		g_VolumePatternStep = 0;
		ShowSequenceOverlay(4, true, lock_enabled != 0);
		tracelog(L"Lock sequence completed. lock_enabled=%d\n", lock_enabled);
	}
}

void HandleVolumeKeyEvent(DWORD vkKey, const wchar_t* source) {
	DWORD64 now = GetTickCount64();
	DWORD64 sinceLastAccepted = now - g_LastAcceptedVolumeTick;

	if (g_LastAcceptedVolumeVk == vkKey && sinceLastAccepted < VolumeSameKeyDebounceMs) {
		if ((now - g_LastSuppressedVolumeLogTick) > 500) {
			tracelog(L"Volume %s suppressed duplicate: vk=0x%02X age=%llums\n", source, vkKey, sinceLastAccepted);
			g_LastSuppressedVolumeLogTick = now;
		}
		return;
	}

	g_LastAcceptedVolumeVk = vkKey;
	g_LastAcceptedVolumeTick = now;
	tracelog(L"Volume %s accepted: vk=0x%02X\n", source, vkKey);
	SetKbdHistoryIndex(vkKey);
}

void HandleKeyboardRawVolumeInput(const RAWKEYBOARD& keyboard) {
	if (keyboard.VKey != VK_VOLUME_UP && keyboard.VKey != VK_VOLUME_DOWN) {
		return;
	}

	bool* keyDownState = keyboard.VKey == VK_VOLUME_UP ? &g_KeyboardVolumeUpDown : &g_KeyboardVolumeDownDown;
	if (keyboard.Message == WM_KEYUP || keyboard.Message == WM_SYSKEYUP) {
		if (*keyDownState) {
			tracelog(L"Volume keyboard raw release: vk=0x%02X\n", keyboard.VKey);
		}
		*keyDownState = false;
		return;
	}

	if (keyboard.Message != WM_KEYDOWN && keyboard.Message != WM_SYSKEYDOWN) {
		return;
	}

	if (*keyDownState) {
		tracelog(L"Volume keyboard raw held repeat suppressed: vk=0x%02X\n", keyboard.VKey);
		return;
	}

	*keyDownState = true;
	HandleVolumeKeyEvent(keyboard.VKey, L"keyboard raw input");
}

LRESULT CALLBACK pWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
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
						HandleVolumeKeyEvent(vkKey, L"consumer HID input");
					}
				}
			}
		}
	}
	else if (uMsg == WM_APPCOMMAND) {
		const int command = GET_APPCOMMAND_LPARAM(lParam);
		if (command == APPCOMMAND_VOLUME_UP) {
			HandleVolumeKeyEvent(VK_VOLUME_UP, L"app command");
			return TRUE;
		}
		if (command == APPCOMMAND_VOLUME_DOWN) {
			HandleVolumeKeyEvent(VK_VOLUME_DOWN, L"app command");
			return TRUE;
		}
	}
	return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

DWORD WINAPI InputEventThread(LPVOID lpParameter) {
	g_hInstance = GetModuleHandle(NULL);
	EnsureGdiPlusInitialized();
	LoadOverlayBitmaps();

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
	else {
		tracelog(L"RegisterClassEx raw input failed: %d\n", GetLastError());
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
	if (!hWnd) {
		tracelog(L"Raw input window creation failed: %d\n", GetLastError());
	}
	else if (!RegisterRawInputDevices(Rid, 2, sizeof(Rid[0]))) {
		tracelog(L"RegisterRawInputDevices failed: %d\n", GetLastError());
	}
	else {
		tracelog(L"Raw input registered on hidden window for keyboard and consumer controls.\n");
	}

#ifdef _DEBUG
	ShowSequenceOverlay(4, true);
	tracelog(L"Debug startup overlay pulse requested.\n");
#endif

	MSG msg;
	while (GetMessage(&msg, NULL, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	DestroyOverlayWindows();
	UnloadOverlayBitmaps();
	ShutdownGdiPlus();
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
	g_hInstance = hInstance;
	tracelog(L"SageLock starting. ExecutableDir=%s\n", GetExecutableDir().c_str());

	if (CheckIfAlreadyRunning()) {
		tracelog(L"SageLock startup blocked: another instance is already running.\n");
		MessageBoxW(NULL, L"SageLock is already running", L"SageLock", MB_OK | MB_ICONERROR);
		return 0;
	}

	tracelog(L"SageLock startup target enumeration begins.\n");
	RefreshTouchDeviceIdsFromHidCaps();
	tracelog(L"SageLock startup target enumeration complete.\n");

	HANDLE hInputThread = CreateThread(NULL, NULL, InputEventThread, NULL, NULL, NULL);
	WaitForSingleObject(hInputThread, INFINITE);
	return 0;
}

