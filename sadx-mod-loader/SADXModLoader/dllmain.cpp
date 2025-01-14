#include "stdafx.h"

#include <cassert>

#include <deque>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <sstream>
#include "direct3d.h"
#include "bgscale.h"
#include "hudscale.h"
#include "testspawn.h"
#include "json.hpp"

using std::deque;
using std::ifstream;
using std::string;
using std::wstring;
using std::unique_ptr;
using std::unordered_map;
using std::vector;
using json = nlohmann::json;

// Win32 headers.
#include <DbgHelp.h>
#include <Shlwapi.h>
#include <GdiPlus.h>
#include "resource.h"

#include "git.h"
#include "version.h"
#include <direct.h>

#include "IniFile.hpp"
#include "CodeParser.hpp"
#include "MediaFns.hpp"
#include "TextConv.hpp"
#include "SADXModLoader.h"
#include "LandTableInfo.h"
#include "ModelInfo.h"
#include "AnimationFile.h"
#include "TextureReplacement.h"
#include "FileReplacement.h"
#include "FileSystem.h"
#include "Events.h"
#include "AutoMipmap.h"
#include "uiscale.h"
#include "FixFOV.h"
#include "EXEData.h"
#include "DLLData.h"
#include "ChunkSpecularFix.h"
#include "CrashDump.h"
#include "MaterialColorFixes.h"
#include "sound.h"
#include "InterpolationFixes.h"
#include "MinorPatches.h"
#include "jvList.h"
#include "Gbix.h"
#include "input.h"
#include <ShlObj.h>
#include "gvm.h"
#include "ExtendedSaveSupport.h"
#include "NodeLimit.h"

static HINSTANCE g_hinstDll = nullptr;

/**
 * Show an error message indicating that this isn't the 2004 US version.
 * This function also calls ExitProcess(1).
 */
static void ShowNon2004USError()
{
	MessageBox(nullptr, L"This copy of Sonic Adventure DX is not the 2004 US version.\n\n"
		L"Please obtain the EXE file from the 2004 US version and try again.",
		L"SADX Mod Loader", MB_ICONERROR);
	ExitProcess(1);
}

/**
 * Hook SADX's CreateFileA() import.
 */
static void HookCreateFileA()
{
	ULONG ulSize = 0;

	// SADX module handle. (main executable)
	HMODULE hModule = GetModuleHandle(nullptr);

	PROC pNewFunction = (PROC)MyCreateFileA;
	// Get the actual CreateFileA() using GetProcAddress().
	PROC pActualFunction = GetProcAddress(GetModuleHandle(L"Kernel32.dll"), "CreateFileA");
	assert(pActualFunction != nullptr);

	auto pImportDesc = (PIMAGE_IMPORT_DESCRIPTOR)ImageDirectoryEntryToData(hModule, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &ulSize);

	if (pImportDesc == nullptr)
	{
		return;
	}

	for (; pImportDesc->Name; pImportDesc++)
	{
		// get the module name
		auto pcszModName = (PCSTR)((PBYTE)hModule + pImportDesc->Name);

		// check if the module is kernel32.dll
		if (pcszModName != nullptr && _stricmp(pcszModName, "Kernel32.dll") == 0)
		{
			// get the module
			auto pThunk = (PIMAGE_THUNK_DATA)((PBYTE)hModule + pImportDesc->FirstThunk);

			for (; pThunk->u1.Function; pThunk++)
			{
				PROC* ppfn = (PROC*)&pThunk->u1.Function;
				if (*ppfn == pActualFunction)
				{
					// Found CreateFileA().
					DWORD dwOldProtect = 0;
					VirtualProtect(ppfn, sizeof(pNewFunction), PAGE_WRITECOPY, &dwOldProtect);
					WriteData(ppfn, pNewFunction);
					VirtualProtect(ppfn, sizeof(pNewFunction), dwOldProtect, &dwOldProtect);
					// FIXME: Would it be listed multiple times?
					break;
				} // Function that we are looking for
			}
		}
	}
}

/**
 * Change write protection of the .rdata section.
 * @param protect True to protect; false to unprotect.
 */
static void SetRDataWriteProtection(bool protect)
{
	// Reference: https://stackoverflow.com/questions/22588151/how-to-find-data-segment-and-code-segment-range-in-program
	// TODO: Does this handle ASLR? (SADX doesn't use ASLR, though...)

	// SADX module handle. (main executable)
	HMODULE hModule = GetModuleHandle(nullptr);

	// Get the PE header.
	const IMAGE_NT_HEADERS* const pNtHdr = ImageNtHeader(hModule);
	// Section headers are located immediately after the PE header.
	const auto* pSectionHdr = reinterpret_cast<const IMAGE_SECTION_HEADER*>(pNtHdr + 1);

	// Find the .rdata section.
	for (unsigned int i = pNtHdr->FileHeader.NumberOfSections; i > 0; i--, pSectionHdr++)
	{
		if (strncmp(reinterpret_cast<const char*>(pSectionHdr->Name), ".rdata", sizeof(pSectionHdr->Name)) != 0)
		{
			continue;
		}

		// Found the .rdata section.
		// Verify that this matches SADX.
		if (pSectionHdr->VirtualAddress != 0x3DB000 ||
			pSectionHdr->Misc.VirtualSize != 0xB6B88)
		{
			// Not SADX, or the wrong version.
			ShowNon2004USError();
			ExitProcess(1);
		}

		const intptr_t vaddr = reinterpret_cast<intptr_t>(hModule) + pSectionHdr->VirtualAddress;
		DWORD flOldProtect;
		DWORD flNewProtect = (protect ? PAGE_READONLY : PAGE_WRITECOPY);
		VirtualProtect(reinterpret_cast<void*>(vaddr), pSectionHdr->Misc.VirtualSize, flNewProtect, &flOldProtect);
		return;
	}

	// .rdata section not found.
	ShowNon2004USError();
	ExitProcess(1);
}

struct message
{
	string text;
	uint32_t time;
};

static deque<message> msgqueue;

static const uint32_t fadecolors[] = {
	0xF7FFFFFF, 0xEEFFFFFF, 0xE6FFFFFF, 0xDDFFFFFF,
	0xD5FFFFFF, 0xCCFFFFFF, 0xC4FFFFFF, 0xBBFFFFFF,
	0xB3FFFFFF, 0xAAFFFFFF, 0xA2FFFFFF, 0x99FFFFFF,
	0x91FFFFFF, 0x88FFFFFF, 0x80FFFFFF, 0x77FFFFFF,
	0x6FFFFFFF, 0x66FFFFFF, 0x5EFFFFFF, 0x55FFFFFF,
	0x4DFFFFFF, 0x44FFFFFF, 0x3CFFFFFF, 0x33FFFFFF,
	0x2BFFFFFF, 0x22FFFFFF, 0x1AFFFFFF, 0x11FFFFFF,
	0x09FFFFFF, 0
};

// Code Parser.
static CodeParser codeParser;

static void __cdecl ProcessCodes()
{
	codeParser.processCodeList();
	RaiseEvents(modFrameEvents);
	uiscale::check_stack_balance();

	const int numrows = (VerticalResolution / (int)DebugFontSize);
	int pos = (int)msgqueue.size() <= numrows - 1 ? numrows - 1 - (msgqueue.size() - 1) : 0;

	if (msgqueue.empty())
	{
		return;
	}

	for (auto iter = msgqueue.begin();
		iter != msgqueue.end(); ++iter)
	{
		int c = -1;

		if (300 - iter->time < LengthOfArray(fadecolors))
		{
			c = fadecolors[LengthOfArray(fadecolors) - (300 - iter->time) - 1];
		}

		SetDebugFontColor((int)c);
		DisplayDebugString(pos++, (char*)iter->text.c_str());
		if (++iter->time >= 300)
		{
			msgqueue.pop_front();

			if (msgqueue.empty())
			{
				break;
			}

			iter = msgqueue.begin();
		}

		if (pos == numrows)
		{
			break;
		}
	}
}

//used to get external lib location and extra config
std::wstring appPath;
std::wstring extLibPath;

void SetAppPathConfig(std::wstring gamepath)
{
	appPath = gamepath + L"\\SAManager\\"; // Account for portable
	extLibPath = appPath + L"extlib\\";
	WCHAR appDataLocalPath[MAX_PATH];
	if (!Exists(appPath))
	{
		if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, appDataLocalPath)))
		{
			appPath = appDataLocalPath;
			appPath += L"\\SAManager\\";
			extLibPath = appPath + L"extlib\\";
		}
	}
}

static bool dbgConsole, dbgScreen;
static bool pauseWhenInactive;
// File for logging debugging output.
static FILE* dbgFile = nullptr;

/**
 * SADX Debug Output function.
 * @param Format Format string.
 * @param ... Arguments.
 * @return Return value from vsnprintf().
 */
static int __cdecl SADXDebugOutput(const char* Format, ...)
{
	va_list ap;
	va_start(ap, Format);
	int result = vsnprintf(nullptr, 0, Format, ap) + 1;
	va_end(ap);
	char* buf = new char[result + 1];
	va_start(ap, Format);
	result = vsnprintf(buf, result + 1, Format, ap);
	va_end(ap);

	// Console output.
	if (dbgConsole)
	{
		// TODO: Convert from Shift-JIS to CP_ACP?
		fputs(buf, stdout);
		fflush(stdout);
	}

	// Screen output.
	if (dbgScreen)
	{
		message msg = { { buf }, 0 };
		// Remove trailing newlines if present.
		while (!msg.text.empty() &&
			(msg.text[msg.text.size() - 1] == '\n' ||
				msg.text[msg.text.size() - 1] == '\r'))
		{
			msg.text.resize(msg.text.size() - 1);
		}
		msgqueue.push_back(msg);
	}

	// File output.
	if (dbgFile)
	{
		// SADX prints text in Shift-JIS.
		// Convert it to UTF-8 before writing it to the debug file.
		char* utf8 = SJIStoUTF8(buf);
		if (utf8)
		{
			fputs(utf8, dbgFile);
			fflush(dbgFile);
			delete[] utf8;
		}
	}

	delete[] buf;
	return result;
}

enum windowmodes { windowed, fullscreen };

struct windowsize
{
	int x;
	int y;
	int width;
	int height;
};

struct windowdata
{
	int x;
	int y;
	int width;
	int height;
	DWORD style;
	DWORD exStyle;
};

// Used for borderless windowed mode.
// Defines the size of the outer-window which wraps the game window and draws the background.
static windowdata outerSizes[] = {
	{ CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, WS_CAPTION | WS_SYSMENU | WS_VISIBLE, 0 }, // windowed
	{ 0, 0, CW_USEDEFAULT, CW_USEDEFAULT, WS_POPUP | WS_VISIBLE, WS_EX_APPWINDOW } // fullscreen
};

// Used for borderless windowed mode.
// Defines the size of the inner-window on which the game is rendered.
static windowsize innerSizes[2] = {};

static HWND accelWindow = nullptr;
static windowmodes windowMode = windowmodes::windowed;
static HACCEL accelTable = nullptr;

DataPointer(int, dword_3D08534, 0x3D08534);

static void __cdecl HandleWindowMessages_r()
{
	MSG Msg; // [sp+4h] [bp-1Ch]@1

	if (PeekMessageA(&Msg, nullptr, 0, 0, 1u))
	{
		do
		{
			if (!TranslateAccelerator(accelWindow, accelTable, &Msg))
			{
				TranslateMessage(&Msg);
				DispatchMessageA(&Msg);
			}
		} while (PeekMessageA(&Msg, nullptr, 0, 0, 1u));
		dword_3D08534 = Msg.wParam;
	}
	else
	{
		dword_3D08534 = Msg.wParam;
	}
}

static vector<RECT> screenBounds;
static Gdiplus::Bitmap* backgroundImage = nullptr;
static bool switchingWindowMode = false;
static bool borderlessWindow = false;
static char voiceLanguage = 1;
static char textLanguage = 1;
static bool scaleScreen = true;
static bool windowResize = false;
static unsigned int screenNum = 1;
static bool customWindowSize = false;
static int customWindowWidth = 640;
static int customWindowHeight = 480;
static bool textureFilter = true;

static BOOL CALLBACK GetMonitorSize(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData)
{
	screenBounds.push_back(*lprcMonitor);
	return TRUE;
}

static const uint8_t wndpatch[] = { 0xA1, 0x30, 0xFD, 0xD0, 0x03, 0xEB, 0x08 }; // mov eax,[hWnd] / jmp short 0xf
static int currentScreenSize[2];

static LRESULT CALLBACK WrapperWndProc(HWND wrapper, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_ACTIVATE:
		if ((LOWORD(wParam) != WA_INACTIVE && !lParam) || reinterpret_cast<HWND>(lParam) == WindowHandle)
		{
			SetFocus(WindowHandle);
			return 0;
		}
		break;

	case WM_CLOSE:
		// we also need to let SADX do cleanup
		SendMessageA(WindowHandle, WM_CLOSE, wParam, lParam);
		// what we do here is up to you: we can check if SADX decides to close, and if so, destroy ourselves, or something like that
		return 0;

	case WM_ERASEBKGND:
	{
		if (backgroundImage == nullptr || windowResize)
		{
			break;
		}

		Gdiplus::Graphics gfx((HDC)wParam);
		gfx.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);

		RECT rect;
		GetClientRect(wrapper, &rect);

		auto w = rect.right - rect.left;
		auto h = rect.bottom - rect.top;

		if (w == innerSizes[windowMode].width && h == innerSizes[windowMode].height)
		{
			break;
		}

		gfx.DrawImage(backgroundImage, 0, 0, w, h);
		return 0;
	}

	case WM_SIZE:
	{
		auto& inner = innerSizes[windowMode];

		if (windowResize)
		{
			inner.x = 0;
			inner.y = 0;
			inner.width = LOWORD(lParam);
			inner.height = HIWORD(lParam);
		}

		// update the inner window (game view)
		SetWindowPos(WindowHandle, HWND_TOP, inner.x, inner.y, inner.width, inner.height, 0);
		break;
	}

	case WM_COMMAND:
	{
		if (wParam != MAKELONG(ID_FULLSCREEN, 1))
			break;

		switchingWindowMode = true;

		if (windowMode == windowed)
		{
			RECT rect;
			GetWindowRect(wrapper, &rect);

			outerSizes[windowMode].x = rect.left;
			outerSizes[windowMode].y = rect.top;
			outerSizes[windowMode].width = rect.right - rect.left;
			outerSizes[windowMode].height = rect.bottom - rect.top;
			outerSizes[windowMode].style = GetWindowLongA(accelWindow, GWL_STYLE);
			outerSizes[windowMode].exStyle = GetWindowLongA(accelWindow, GWL_EXSTYLE);
		}

		windowMode = windowMode == windowed ? fullscreen : windowed;

		// update outer window (draws background)
		const auto& outer = outerSizes[windowMode];
		SetWindowLongA(accelWindow, GWL_STYLE, outer.style);
		SetWindowLongA(accelWindow, GWL_EXSTYLE, outer.exStyle);
		SetWindowPos(accelWindow, HWND_NOTOPMOST, outer.x, outer.y, outer.width, outer.height, SWP_FRAMECHANGED);

		switchingWindowMode = false;
		return 0;
	}

	case WM_ACTIVATEAPP:
		if (!switchingWindowMode)
		{
			if (pauseWhenInactive)
			{
				if (wParam)
				{
					if (!IsGamePaused())
					{
						UnpauseAllSounds(0);
					}
					else
						ResumeMusic();
				}
				else
					PauseAllSounds(0);
			}
			WndProc_B(WindowHandle, uMsg, wParam, lParam);
		}

		if (windowMode == windowed)
		{
			while (ShowCursor(TRUE) < 0);
		}
		else
		{
			while (ShowCursor(FALSE) > 0);
		}

	default:
		break;
	}

	// alternatively we can return SendMe
	return DefWindowProcA(wrapper, uMsg, wParam, lParam);
}

static RECT   last_rect = {};
static Uint32 last_width = 0;
static Uint32 last_height = 0;
static DWORD  last_style = 0;
static DWORD  last_exStyle = 0;

static void enable_fullscreen_mode(HWND handle)
{
	IsWindowed = false;
	last_width = HorizontalResolution;
	last_height = VerticalResolution;

	GetWindowRect(handle, &last_rect);

	last_style = GetWindowLongA(handle, GWL_STYLE);
	last_exStyle = GetWindowLongA(handle, GWL_EXSTYLE);

	SetWindowLongA(handle, GWL_STYLE, WS_POPUP | WS_SYSMENU | WS_VISIBLE);

	while (ShowCursor(FALSE) > 0);
}

static void enable_windowed_mode(HWND handle)
{
	SetWindowLongA(handle, GWL_STYLE, last_style);
	SetWindowLongA(handle, GWL_EXSTYLE, last_exStyle);

	auto width = last_rect.right - last_rect.left;
	auto height = last_rect.bottom - last_rect.top;

	if (width <= 0 || height <= 0)
	{
		last_rect = {};

		last_rect.right = 640;
		last_rect.bottom = 480;

		AdjustWindowRectEx(&last_rect, last_style, false, last_exStyle);

		width = last_rect.right - last_rect.left;
		height = last_rect.bottom - last_rect.top;
	}

	SetWindowPos(handle, HWND_NOTOPMOST, last_rect.left, last_rect.top, width, height, 0);
	IsWindowed = true;

	while (ShowCursor(TRUE) < 0);
}

static LRESULT CALLBACK WndProc_Resizable(HWND handle, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	switch (Msg)
	{
	default:
		break;

	case WM_SYSKEYDOWN:
		if (wParam != VK_F4 && wParam != VK_F2 && wParam != VK_RETURN) return 0;

	case WM_SYSKEYUP:
		if (wParam != VK_F4 && wParam != VK_F2 && wParam != VK_RETURN) return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		break;

	case WM_SIZE:
	{
		if (customWindowSize)
		{
			break;
		}

		if (!IsWindowed || Direct3D_Device == nullptr)
		{
			return 0;
		}

		int w = LOWORD(lParam);
		int h = HIWORD(lParam);

		if (!w || !h)
		{
			break;
		}

		direct3d::change_resolution(w, h);
		break;
	}

	case WM_COMMAND:
	{
		if (wParam != MAKELONG(ID_FULLSCREEN, 1))
		{
			break;
		}

		if (direct3d::is_windowed() && IsWindowed)
		{
			enable_fullscreen_mode(handle);

			const auto& rect = screenBounds[screenNum == 0 ? 0 : screenNum - 1];

			const auto w = rect.right - rect.left;
			const auto h = rect.bottom - rect.top;

			direct3d::change_resolution(w, h, false);
		}
		else
		{
			direct3d::change_resolution(last_width, last_height, true);
			enable_windowed_mode(handle);
		}

		return 0;
	}
	}

	return DefWindowProcA(handle, Msg, wParam, lParam);
}

LRESULT __stdcall WndProc_hook(HWND handle, UINT Msg, WPARAM wParam, LPARAM lParam)
{
	if ((Msg == WM_SYSKEYDOWN || Msg == WM_SYSKEYUP) && ((wParam != VK_F4 && wParam != VK_F2 && wParam != VK_RETURN))) return 0;
	return DefWindowProcA(handle, Msg, wParam, lParam);
}

wstring borderimg = L"mods\\Border.png";

static void ResumeAllSoundsPause()
{
	if (!IsGamePaused())
	{
		UnpauseAllSounds(0);
	}
	else
		ResumeMusic();
}

static void PauseAllSoundsAndMusic()
{
	PauseAllSounds(0);
}

static void CreateSADXWindow_r(HINSTANCE hInstance, int nCmdShow)
{
	// Primary window class name.
	const char* const lpszClassName = GetWindowClassName();

	// Hook default return of SADX's window procedure to force it to return DefWindowProc
	WriteJump(reinterpret_cast<void*>(0x00789E48), WndProc_hook);

	// Primary window class for SADX.
	WNDCLASSA v8{}; // [sp+4h] [bp-28h]@1

	v8.style = 0;
	v8.lpfnWndProc = (windowResize ? WndProc_Resizable : WndProc);
	v8.cbClsExtra = 0;
	v8.cbWndExtra = 0;
	v8.hInstance = hInstance;
	v8.hIcon = LoadIconA(hInstance, MAKEINTRESOURCEA(101));
	v8.hCursor = LoadCursor(nullptr, IDC_ARROW);
	v8.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
	v8.lpszMenuName = nullptr;
	v8.lpszClassName = lpszClassName;

	if (!RegisterClassA(&v8))
	{
		MessageBox(nullptr, L"Failed to register class A to create SADX Window, game won't work.", L"Failed to create SADX Window", MB_OK | MB_ICONERROR);
		return;
	}

	RECT windowRect;

	windowRect.top = 0;
	windowRect.left = 0;

	if (customWindowSize)
	{
		windowRect.right = customWindowWidth;
		windowRect.bottom = customWindowHeight;
	}
	else
	{
		windowRect.right = HorizontalResolution;
		windowRect.bottom = VerticalResolution;
	}

	if (borderlessWindow || IsWindowed)
	{
		currentScreenSize[0] = GetSystemMetrics(SM_CXSCREEN);
		currentScreenSize[1] = GetSystemMetrics(SM_CYSCREEN);
		WriteData((int**)0x79426E, &currentScreenSize[0]);
		WriteData((int**)0x79427A, &currentScreenSize[1]);
	}

	EnumDisplayMonitors(nullptr, nullptr, GetMonitorSize, 0);

	int screenX, screenY, screenW, screenH, wsX, wsY, wsW, wsH;
	if (screenNum > 0)
	{
		if (screenBounds.size() < screenNum)
		{
			screenNum = 1;
		}

		RECT screenSize = screenBounds[screenNum - 1];

		wsX = screenX = screenSize.left;
		wsY = screenY = screenSize.top;
		wsW = screenW = screenSize.right - screenSize.left;
		wsH = screenH = screenSize.bottom - screenSize.top;
	}
	else
	{
		screenX = GetSystemMetrics(SM_XVIRTUALSCREEN);
		screenY = GetSystemMetrics(SM_YVIRTUALSCREEN);
		screenW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
		screenH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

		wsX = 0;
		wsY = 0;
		wsW = GetSystemMetrics(SM_CXSCREEN);
		wsH = GetSystemMetrics(SM_CYSCREEN);
	}

	accelTable = LoadAcceleratorsA(g_hinstDll, MAKEINTRESOURCEA(IDR_ACCEL_WRAPPER_WINDOW));

	if (borderlessWindow)
	{
		PrintDebug("Creating SADX Window in borderless mode...\n");

		if (windowResize)
		{
			outerSizes[windowed].style |= WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SIZEBOX;
		}

		AdjustWindowRectEx(&windowRect, outerSizes[windowed].style, false, 0);

		outerSizes[windowed].width = windowRect.right - windowRect.left;
		outerSizes[windowed].height = windowRect.bottom - windowRect.top;

		outerSizes[windowed].x = wsX + ((wsW - outerSizes[windowed].width) / 2);
		outerSizes[windowed].y = wsY + ((wsH - outerSizes[windowed].height) / 2);

		outerSizes[fullscreen].x = screenX;
		outerSizes[fullscreen].y = screenY;
		outerSizes[fullscreen].width = screenW;
		outerSizes[fullscreen].height = screenH;

		if (customWindowSize)
		{
			float num = min((float)customWindowWidth / (float)HorizontalResolution, (float)customWindowHeight / (float)VerticalResolution);

			innerSizes[windowed].width = (int)((float)HorizontalResolution * num);
			innerSizes[windowed].height = (int)((float)VerticalResolution * num);
			innerSizes[windowed].x = (customWindowWidth - innerSizes[windowed].width) / 2;
			innerSizes[windowed].y = (customWindowHeight - innerSizes[windowed].height) / 2;
		}
		else
		{
			innerSizes[windowed].width = HorizontalResolution;
			innerSizes[windowed].height = VerticalResolution;
			innerSizes[windowed].x = 0;
			innerSizes[windowed].y = 0;
		}

		if (scaleScreen)
		{
			float num = min((float)screenW / (float)HorizontalResolution, (float)screenH / (float)VerticalResolution);

			innerSizes[fullscreen].width = (int)((float)HorizontalResolution * num);
			innerSizes[fullscreen].height = (int)((float)VerticalResolution * num);
		}
		else
		{
			innerSizes[fullscreen].width = HorizontalResolution;
			innerSizes[fullscreen].height = VerticalResolution;
		}

		innerSizes[fullscreen].x = (screenW - innerSizes[fullscreen].width) / 2;
		innerSizes[fullscreen].y = (screenH - innerSizes[fullscreen].height) / 2;

		windowMode = IsWindowed ? windowed : fullscreen;

		if (!FileExists(borderimg))
		{
			borderimg = L"mods\\Border_Default.png";
		}

		if (FileExists(borderimg))
		{
			Gdiplus::GdiplusStartupInput gdiplusStartupInput;
			ULONG_PTR gdiplusToken;
			Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);
			backgroundImage = Gdiplus::Bitmap::FromFile(borderimg.c_str());
		}

		// Register a window class for the wrapper window.
		WNDCLASSA w;

		w.style = 0;
		w.lpfnWndProc = WrapperWndProc;
		w.cbClsExtra = 0;
		w.cbWndExtra = 0;
		w.hInstance = hInstance;
		w.hIcon = LoadIconA(hInstance, MAKEINTRESOURCEA(101));
		w.hCursor = LoadCursor(nullptr, IDC_ARROW);
		w.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
		w.lpszMenuName = nullptr;
		w.lpszClassName = "WrapperWindow";

		if (!RegisterClassA(&w))
		{
			MessageBox(nullptr, L"Failed to register class A to create SADX Window with Borderless settings, game won't work.", L"Failed to create SADX Window", MB_OK | MB_ICONERROR);
			return;
		}

		const auto& outerSize = outerSizes[windowMode];

		accelWindow = CreateWindowExA(outerSize.exStyle,
			"WrapperWindow",
			lpszClassName,
			outerSize.style,
			outerSize.x, outerSize.y, outerSize.width, outerSize.height,
			nullptr, nullptr, hInstance, nullptr);

		if (accelWindow == nullptr)
		{
			MessageBox(nullptr, L"Failed to Create AccelWindow with Borderless settings, game won't work.", L"Failed to create SADX Window", MB_OK | MB_ICONERROR);
			return;
		}

		const auto& innerSize = innerSizes[windowMode];

		WindowHandle = CreateWindowExA(0,
			lpszClassName,
			lpszClassName,
			WS_CHILD | WS_VISIBLE,
			innerSize.x, innerSize.y, innerSize.width, innerSize.height,
			accelWindow, nullptr, hInstance, nullptr);

		if (WindowHandle == nullptr)
		{
			MessageBox(nullptr, L"Failed to Create SADX Window with Borderless settings, game won't work.", L"Failed to create SADX Window", MB_OK | MB_ICONERROR);
			return;
		}

		SetFocus(WindowHandle);
		ShowWindow(accelWindow, nCmdShow);
		UpdateWindow(accelWindow);
		SetForegroundWindow(accelWindow);

		PrintDebug("Successfully created SADX Window!\n");
		IsWindowed = true;

		WriteData((void*)0x402C61, wndpatch);
	}
	else
	{
		PrintDebug("Creating SADX Window...\n");

		DWORD dwStyle = WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
		DWORD dwExStyle = 0;

		if (windowResize)
		{
			dwStyle |= WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SIZEBOX;
		}

		AdjustWindowRectEx(&windowRect, dwStyle, false, dwExStyle);

		int w = windowRect.right - windowRect.left;
		int h = windowRect.bottom - windowRect.top;
		int x = wsX + ((wsW - w) / 2);
		int y = wsY + ((wsH - h) / 2);

		WindowHandle = CreateWindowExA(dwExStyle,
			lpszClassName,
			lpszClassName,
			dwStyle,
			x, y, w, h,
			nullptr, nullptr, hInstance, nullptr);

		if (WindowHandle == nullptr)
		{
			MessageBox(nullptr, L"Failed to Create SADX Window, game won't work.", L"Failed to create SADX Window", MB_OK | MB_ICONERROR);
			return;
		}


		if (!IsWindowed)
		{
			enable_fullscreen_mode(WindowHandle);
		}

		ShowWindow(WindowHandle, nCmdShow);
		UpdateWindow(WindowHandle);
		SetForegroundWindow(WindowHandle);

		accelWindow = WindowHandle;

		WriteCall((void*)0x00401920, ResumeAllSoundsPause);
		WriteCall((void*)0x00401939, PauseAllSoundsAndMusic);

		PrintDebug("Successfully created SADX Window!\n");
	}

	// Hook the window message handler.
	WriteJump((void*)HandleWindowMessages, (void*)HandleWindowMessages_r);
}

static __declspec(naked) void CreateSADXWindow_asm()
{
	__asm
	{
		mov ebx, [esp + 4]
		push ebx
		push eax
		call CreateSADXWindow_r
		add esp, 8
		retn
	}
}

unordered_map<unsigned char, unordered_map<int, StartPosition>> StartPositions;
unordered_map<unsigned char, unordered_map<int, FieldStartPosition>> FieldStartPositions;
unordered_map<int, PathDataPtr> Paths;
bool PathsInitialized;
unordered_map<unsigned char, vector<PVMEntry>> CharacterPVMs;
vector<PVMEntry> CommonObjectPVMs;
bool CommonObjectPVMsInitialized;
unordered_map<unsigned char, vector<TrialLevelListEntry>> _TrialLevels;
unordered_map<unsigned char, vector<TrialLevelListEntry>> _TrialSubgames;
const char* mainsavepath = "SAVEDATA";
const char* chaosavepath = "SAVEDATA";
string windowtitle;
vector<SoundList> _SoundLists;
vector<MusicInfo> _MusicList;
vector<__int16> _USVoiceDurationList;
vector<__int16> _JPVoiceDurationList;
extern HelperFunctions helperFunctions;
LoaderSettings loaderSettings = {};

static const char* const dlldatakeys[] = {
	"CHRMODELSData",
	"ADV00MODELSData",
	"ADV01MODELSData",
	"ADV01CMODELSData",
	"ADV02MODELSData",
	"ADV03MODELSData",
	"BOSSCHAOS0MODELSData",
	"CHAOSTGGARDEN02MR_DAYTIMEData",
	"CHAOSTGGARDEN02MR_EVENINGData",
	"CHAOSTGGARDEN02MR_NIGHTData"
};

void __cdecl SetLanguage()
{
	VoiceLanguage = voiceLanguage;
	TextLanguage = textLanguage;
}

Bool __cdecl FixEKey(int i)
{
	return IsFreeCameraAllowed() == TRUE && GetKey(i) == TRUE;
}

const auto loc_794566 = (void*)0x00794566;

void __declspec(naked) PolyBuff_Init_FixVBuffParams()
{
	__asm
	{
		push D3DPOOL_MANAGED
		push ecx
		push D3DUSAGE_WRITEONLY
		jmp loc_794566
	}
}

void __cdecl Direct3D_TextureFilterPoint_ForceLinear()
{
	Direct3D_Device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	Direct3D_Device->SetRenderState(D3DRS_LIGHTING, 0);
	Direct3D_Device->SetRenderState(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
	Direct3D_Device->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
	Direct3D_Device->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
	Direct3D_Device->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
}

void __cdecl SetPreferredFilterOption()
{
	if (textureFilter == true)
	{
		Direct3D_TextureFilterPoint_ForceLinear();
	}
	else
	{
		Direct3D_TextureFilterPoint();
	}
}

void __cdecl Direct3D_EnableHudAlpha_Point(bool enable)
{
	Direct3D_EnableHudAlpha(enable);
	Direct3D_TextureFilterPoint();
}

void __cdecl njDrawTextureMemList_NoFilter(NJS_TEXTURE_VTX* polygons, Int count, Uint32 gbix, Int flag)
{
	njAlphaMode(flag);
	Direct3D_TextureFilterPoint();
	njSetTextureNumG(gbix);
	if (uiscale::is_scale_enabled() == true) uiscale::scale_texmemlist(polygons, count);
	DrawRect_TextureVertexTriangleStrip(polygons, count);
	Direct3D_TextureFilterLinear();
}

void __cdecl FixLandTableLightType()
{
	Direct3D_PerformLighting(0);
}

void __cdecl PreserveLightType(int a1)
{
	Direct3D_PerformLighting(a1 * 2); // Bitwise shift left
}

void ProcessVoiceDurationRegisters()
{
	if (!_USVoiceDurationList.empty())
	{
		auto size = _USVoiceDurationList.size();
		auto newlist = new __int16[size];
		memcpy(newlist, _USVoiceDurationList.data(), sizeof(__int16) * size);
		WriteData((__int16**)0x4255A2, newlist);
	}

	if (!_JPVoiceDurationList.empty())
	{
		auto size = _JPVoiceDurationList.size();
		auto newlist = new __int16[size];
		memcpy(newlist, _JPVoiceDurationList.data(), sizeof(__int16) * size);
		WriteData((__int16**)0x42552F, newlist);
	}

	_USVoiceDurationList.clear();
	_JPVoiceDurationList.clear();
}

//Following order for the BASS dlls is REALLY important, NEVER touch it or BASS will FAIL to load.
const std::wstring bassDLLs[] =
{
	L"bass.dll",
	L"libatrac9.dll",
	L"libcelt-0061.dll",
	L"libcelt-0110.dll",
	L"libg719_decode.dll",
	L"libmpg123-0.dll",
	L"libspeex-1.dll",
	L"libvorbis.dll",
	L"avutil-vgmstream-57.dll",
	L"avcodec-vgmstream-59.dll",
	L"avformat-vgmstream-59.dll",
	L"jansson.dll",
	L"swresample-vgmstream-4.dll",
	L"bass_vgmstream.dll",
};

static void __cdecl InitAudio()
{

	if (loaderSettings.EnableBassMusic || !Exists("system\\sounddata\\bgm"))
	{
		wstring bassFolder = extLibPath + L"BASS\\";

		if (!FileExists(bassFolder + L"bass.dll"))
			bassFolder = L"BASS\\";

		bool bassDLL = false;

		for (uint8_t i = 0; i < LengthOfArray(bassDLLs); i++)
		{
			wstring fullPath = bassFolder + bassDLLs[i];
			bassDLL = LoadLibrary(fullPath.c_str());
		}

		if (bassDLL)
		{
			WriteCall((void*)0x42544C, PlayMusicFile_r);
			WriteCall((void*)0x4254F4, PlayVoiceFile_r);
			WriteCall((void*)0x425569, PlayVoiceFile_r);
			WriteCall((void*)0x513187, PlayVideoFile_r);
			WriteCall((void*)0x425488, PlayMusicFile_CD_r);
			WriteCall((void*)0x425591, PlayVoiceFile_CD_r);
			WriteCall((void*)0x42551D, PlayVoiceFile_CD_r);
			WriteJump((void*)0x40D1EA, WMPInit_r);
			WriteJump((void*)0x40CF50, WMPRestartMusic_r);
			WriteJump((void*)0x40D060, PauseMusic_r);
			WriteJump((void*)0x40D0A0, ResumeMusic_r);
			WriteJump((void*)0x40CFF0, WMPClose_r);
			WriteJump((void*)0x40D28A, WMPRelease_r);
			WriteJump((void*)0x40CF20, sub_40CF20_r);
			PrintDebug("Loaded Bass DLLs dependencies\n");
		}
		else
		{
			PrintDebug("Failed to load bass DLL dependencies\n");
		}
	}

	if (loaderSettings.HRTFSound)
	{
		// allow HRTF 3D sound
		WriteData<uint8_t>(reinterpret_cast<uint8_t*>(0x00402773), 0xEBu);
	}
}

void InitPatches()
{
	//Fix the game not saving camera setting properly 
	if (loaderSettings.CCEF)
	{
		WriteData((int16_t*)0x438330, (int16_t)0x0D81);
		WriteData((int16_t*)0x434870, (int16_t)0x0D81);
	}

	// Fixes N-sided polygons (Gamma's headlight) by using
	// triangle strip vertex buffer initializers.

	if (loaderSettings.E102PolyFix)
	{
		for (size_t i = 0; i < MeshSetInitFunctions.size(); ++i)
		{
			auto& a = MeshSetInitFunctions[i];
			a[NJD_MESHSET_N >> 14] = a[NJD_MESHSET_TRIMESH >> 14];
		}

		for (size_t i = 0; i < PolyBuffDraw_VertexColor.size(); ++i)
		{
			auto& a = PolyBuffDraw_VertexColor[i];
			a[NJD_MESHSET_N >> 14] = a[NJD_MESHSET_TRIMESH >> 14];
		}

		for (size_t i = 0; i < PolyBuffDraw_NoVertexColor.size(); ++i)
		{
			auto& a = PolyBuffDraw_NoVertexColor[i];
			a[NJD_MESHSET_N >> 14] = a[NJD_MESHSET_TRIMESH >> 14];
		}
	}

	if (loaderSettings.PixelOffsetFix)
	{
		// Replaces half-pixel offset addition with subtraction
		WriteData((uint8_t*)0x0077DE1E, (uint8_t)0x25); // njDrawQuadTextureEx
		WriteData((uint8_t*)0x0077DE33, (uint8_t)0x25); // njDrawQuadTextureEx
		WriteData((uint8_t*)0x0078E822, (uint8_t)0x25); // DrawRect_TextureVertexTriangleStrip
		WriteData((uint8_t*)0x0078E83C, (uint8_t)0x25); // DrawRect_TextureVertexTriangleStrip
		WriteData((uint8_t*)0x0078E991, (uint8_t)0x25); // njDrawTriangle2D_List
		WriteData((uint8_t*)0x0078E9AE, (uint8_t)0x25); // njDrawTriangle2D_List
		WriteData((uint8_t*)0x0078EA41, (uint8_t)0x25); // Direct3D_DrawTriangleFan2D
		WriteData((uint8_t*)0x0078EA5E, (uint8_t)0x25); // Direct3D_DrawTriangleFan2D
		WriteData((uint8_t*)0x0078EAE1, (uint8_t)0x25); // njDrawTriangle2D_Strip
		WriteData((uint8_t*)0x0078EAFE, (uint8_t)0x25); // njDrawTriangle2D_Strip
		WriteData((uint8_t*)0x0077DBFC, (uint8_t)0x25); // njDrawPolygon
		WriteData((uint8_t*)0x0077DC16, (uint8_t)0x25); // njDrawPolygon
		WriteData((uint8_t*)0x0078E8F1, (uint8_t)0x25); // njDrawLine2D_Direct3D
		WriteData((uint8_t*)0x0078E90E, (uint8_t)0x25); // njDrawLine2D_Direct3D
	}

	if (loaderSettings.ChaoPanelFix)
	{
		// Chao stat panel screen dimensions fix
		WriteData((float**)0x007377FE, (float*)&_nj_screen_.w);
	}

	if (loaderSettings.LightFix)
	{
		// Fix light incorrectly being applied on LandTables
		WriteCall(reinterpret_cast<void*>(0x0043A6D5), FixLandTableLightType);

		// Enable light type preservation in the draw queue
		WriteCall(reinterpret_cast<void*>(0x004088B1), PreserveLightType);

		// Do not reset light type for queued models
		WriteData<2>(reinterpret_cast<void*>(0x004088A6), 0x90i8);
	}

	if (loaderSettings.NodeLimit)
		IncreaseNodeLimit();

	if (loaderSettings.ChunkSpecFix)
		ChunkSpecularFix_Init();


	if (loaderSettings.KillGbix)
		Init_NOGbixHack();
}


static vector<string>& split(const string& s, char delim, vector<string>& elems)
{
	std::stringstream ss(s);
	string item;

	while (std::getline(ss, item, delim))
	{
		elems.push_back(item);
	}

	return elems;
}


static vector<string> split(const string& s, char delim)
{
	vector<string> elems;
	split(s, delim, elems);
	return elems;
}

static string trim(const string& s)
{
	auto st = s.find_first_not_of(' ');

	if (st == string::npos)
	{
		st = 0;
	}

	auto ed = s.find_last_not_of(' ');

	if (ed == string::npos)
	{
		ed = s.size() - 1;
	}

	return s.substr(st, (ed + 1) - st);
}

static const unordered_map<string, uint8_t> charnamemap = {
	{ "sonic",    Characters_Sonic },
	{ "eggman",   Characters_Eggman },
	{ "tails",    Characters_Tails },
	{ "knuckles", Characters_Knuckles },
	{ "tikal",    Characters_Tikal },
	{ "amy",      Characters_Amy },
	{ "gamma",    Characters_Gamma },
	{ "big",      Characters_Big }
};

static uint8_t ParseCharacter(const string& str)
{
	string str2 = trim(str);
	transform(str2.begin(), str2.end(), str2.begin(), ::tolower);
	auto ch = charnamemap.find(str2);
	if (ch != charnamemap.end())
		return ch->second;
	else
		return (uint8_t)strtol(str.c_str(), nullptr, 10);
}
extern void RegisterCharacterWelds(const uint8_t character, const char* iniPath);


std::vector<Mod> modlist;

bool IsWindowsVistaOrGreater()
{
	// Taken from https://stackoverflow.com/questions/1963992/check-windows-version
	OSVERSIONINFOEXW osvi = {};
	osvi.dwOSVersionInfoSize = sizeof(osvi);
	DWORDLONG const dwlConditionMask = VerSetConditionMask(
		VerSetConditionMask(
			VerSetConditionMask(
				0, VER_MAJORVERSION, VER_GREATER_EQUAL),
			VER_MINORVERSION, VER_GREATER_EQUAL),
		VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL);
	osvi.dwMajorVersion = HIBYTE(_WIN32_WINNT_VISTA);
	osvi.dwMinorVersion = LOBYTE(_WIN32_WINNT_VISTA);
	osvi.wServicePackMajor = 0;

	return VerifyVersionInfoW(&osvi, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR, dwlConditionMask) != FALSE;
}

static void __cdecl InitMods()
{
	// Hook present function to handle device lost/reset states
	direct3d::init();

	// Get sonic.exe's path and filename.
	wchar_t pathbuf[MAX_PATH];
	GetModuleFileName(nullptr, pathbuf, MAX_PATH);
	wstring exepath(pathbuf);
	wstring exefilename;
	string::size_type slash_pos = exepath.find_last_of(L"/\\");
	if (slash_pos != string::npos)
	{
		exefilename = exepath.substr(slash_pos + 1);
		if (slash_pos > 0)
			exepath = exepath.substr(0, slash_pos);
	}

	// Convert the EXE filename to lowercase.
	transform(exefilename.begin(), exefilename.end(), exefilename.begin(), ::towlower);

	// Get path for Mod Manager settings and libraries
	SetAppPathConfig(exepath);

	// Load profiles JSON file
	std::ifstream ifs(appPath + L"\\SADX\\Profiles.json");
	json json_profiles = json::parse(ifs);
	ifs.close();
	
	// Get current profile index
	int ind_profile = json_profiles.value("ProfileIndex", 0);

	// Get current profile filename
	json proflist = json_profiles["ProfilesList"];
	std::string profname = proflist.at(ind_profile)["Filename"];

	// Convert profile name from UTF8 stored in JSON to wide string
	int count = MultiByteToWideChar(CP_UTF8, 0, profname.c_str(), profname.length(), NULL, 0);
	std::wstring profname_w(count, 0);
	MultiByteToWideChar(CP_UTF8, 0, profname.c_str(), profname.length(), &profname_w[0], count);

	// Load the current profile
	std::ifstream ifs_p(appPath + L"\\SADX\\" + profname_w);
	json json_config = json::parse(ifs_p);
	int setver = json_config.value("SettingsVersion", 0);

	// Graphics settings
	json json_graphics = json_config["Graphics"];
	loaderSettings.ScreenNum = json_graphics.value("SelectedScreen", 0);
	loaderSettings.HorizontalResolution = json_graphics.value("HorizontalResolution", 640);
	loaderSettings.VerticalResolution = json_graphics.value("VerticalResolution", 480);
	loaderSettings.ForceAspectRatio = json_graphics.value("Enable43ResolutionRatio", false);
	loaderSettings.EnableVsync = json_graphics.value("EnableVsync", true);
	loaderSettings.PauseWhenInactive = json_graphics.value("EnablePauseOnInactive", true);
	loaderSettings.WindowedFullscreen = json_graphics.value("EnableBorderless", true);
	loaderSettings.StretchFullscreen = json_graphics.value("EnableScreenScaling", true);
	loaderSettings.CustomWindowSize = json_graphics.value("EnableCustomWindow", false);
	loaderSettings.WindowWidth = json_graphics.value("CustomWindowWidth", 640);
	loaderSettings.WindowHeight = json_graphics.value("CustomWindowHeight", 480);
	loaderSettings.MaintainWindowAspectRatio = json_graphics.value("EnableKeepResolutionRatio", false);
	loaderSettings.ResizableWindow = json_graphics.value("EnableResizableWindow", true);
	loaderSettings.BackgroundFillMode = json_graphics.value("FillModeBackground", 2);
	loaderSettings.FmvFillMode = json_graphics.value("FillModeFMV", 1);
	// ModeTextureFiltering ?
	// ModeUIFiltering ?
	loaderSettings.ScaleHud = json_graphics.value("EnableUIScaling", true);
	loaderSettings.AutoMipmap = json_graphics.value("EnableForcedMipmapping", true);
	loaderSettings.TextureFilter = json_graphics.value("EnableForcedTextureFilter", true);
	
	// Controller settings
	json json_controller = json_config["Controller"];
	loaderSettings.InputMod = json_controller.value("EnabledInputMod", true);

	// Sound settings
	json json_sound = json_config["Sound"];
	loaderSettings.EnableBassMusic = json_sound.value("EnableBassMusic", true);
	loaderSettings.EnableBassSFX = json_sound.value("EnableBassSFX", true);	
	loaderSettings.SEVolume = json_sound.value("SEVolume", 100);

	// Test Spawn settings
	json json_testspawn = json_config["TestSpawn"];
	// UseCharacter?
	// UseLevel?
	// UseEvent?
	// UseGameMode?
	// UseSave?
	loaderSettings.TestSpawnLevel = json_testspawn.value("CharacterIndex", 0);
	loaderSettings.TestSpawnAct = json_testspawn.value("CharacterIndex", 0);
	loaderSettings.TestSpawnCharacter = json_testspawn.value("CharacterIndex", 0);
	loaderSettings.TestSpawnEvent = json_testspawn.value("EventIndex", 0);
	loaderSettings.TestSpawnGameMode = json_testspawn.value("GameModeIndex", 0);
	loaderSettings.TestSpawnSaveID = json_testspawn.value("SaveIndex", 0);
	loaderSettings.TextLanguage = json_testspawn.value("GameTextLanguage", 1);
	loaderSettings.VoiceLanguage = json_testspawn.value("GameVoiceLanguage", 1);
	// UseManual ?
	loaderSettings.TestSpawnPositionEnabled = json_testspawn.value("UsePosition", false);
	loaderSettings.TestSpawnX = json_testspawn.value("XPosition", 0);
	loaderSettings.TestSpawnY = json_testspawn.value("YPosition", 0);
	loaderSettings.TestSpawnZ = json_testspawn.value("ZPosition", 0);
	loaderSettings.TestSpawnRotation = json_testspawn.value("Rotation", 0);
	
	// Patches settings
	json json_patches = json_config["Patches"];
	loaderSettings.HRTFSound = json_patches.value("HRTFSound", false);
	loaderSettings.CCEF = json_patches.value("KeepCamSettings", true);
	loaderSettings.PolyBuff = json_patches.value("FixVertexColorRendering", true);
	loaderSettings.MaterialColorFix = json_patches.value("MaterialColorFix", true);
	loaderSettings.NodeLimit = json_patches.value("NodeLimit", true);
	loaderSettings.FovFix = json_patches.value("FOVFix", true);
	loaderSettings.SCFix = json_patches.value("SkyChaseResolutionFix", true);
	loaderSettings.Chaos2CrashFix = json_patches.value("Chaos2CrashFix", true);
	loaderSettings.ChunkSpecFix = json_patches.value("ChunkSpecularFix", true);
	loaderSettings.E102PolyFix = json_patches.value("E102NGonFix", true);
	loaderSettings.ChaoPanelFix = json_patches.value("ChaoPanelFix", true);
	loaderSettings.PixelOffsetFix = json_patches.value("PixelOffSetFix", true);
	loaderSettings.LightFix = json_patches.value("LightFix", true);
	loaderSettings.KillGbix = json_patches.value("KillGBIX", false);
	loaderSettings.DisableCDCheck = json_patches.value("DisableCDCheck", true);
	loaderSettings.ExtendedSaveSupport = json_patches.value("ExtendedSaveSupport", true);

	// Debug settings
	json json_debug = json_config["DebugSettings"];
	loaderSettings.DebugConsole = json_debug.value("EnableDebugConsole", false);
	loaderSettings.DebugScreen = json_debug.value("EnableDebugScreen", false);
	loaderSettings.DebugFile = json_debug.value("EnableDebugFile", false);
	loaderSettings.DebugCrashLog = json_debug.value("EnableDebugCrashLog", true);
	// EnableShowConsole ?

	// Process the main Mod Loader settings.
	if (loaderSettings.DebugConsole)
	{
		// Enable the debug console.
		// TODO: setvbuf()?
		AllocConsole();
		SetConsoleTitle(L"SADX Mod Loader output");
		freopen("CONOUT$", "wb", stdout);
		dbgConsole = true;
	}

	dbgScreen = loaderSettings.DebugScreen;
	if (loaderSettings.DebugFile)
	{
		// Enable debug logging to a file.
		// dbgFile will be nullptr if the file couldn't be opened.
		dbgFile = _wfopen(L"mods\\SADXModLoader.log", L"a+");
	}

	// Is any debug method enabled?
	if (dbgConsole || dbgScreen || dbgFile)
	{
		WriteJump((void*)PrintDebug, (void*)SADXDebugOutput);

		PrintDebug("SADX Mod Loader v" VERSION_STRING " (API version %d), built " __TIMESTAMP__ "\n",
			ModLoaderVer);

#ifdef MODLOADER_GIT_VERSION
#ifdef MODLOADER_GIT_DESCRIBE
		PrintDebug("%s, %s\n", MODLOADER_GIT_VERSION, MODLOADER_GIT_DESCRIBE);
#else /* !MODLOADER_GIT_DESCRIBE */
		PrintDebug("%s\n", MODLOADER_GIT_VERSION);
#endif /* MODLOADER_GIT_DESCRIBE */
#endif /* MODLOADER_GIT_VERSION */
	}

	// Set process DPI awareness for window resize on Vista and above
	if (IsWindowsVistaOrGreater())
	{
		if (!SetProcessDPIAware())
			PrintDebug("Unable to set process DPI awareness.\n");
	}

	WriteJump((void*)0x789E50, CreateSADXWindow_asm); // override window creation function
	// Other various settings.
	if (loaderSettings.DisableCDCheck)
		WriteJump((void*)0x402621, (void*)0x402664);

	// Custom resolution.
	WriteJump((void*)0x40297A, (void*)0x402A90);

	int hres = loaderSettings.HorizontalResolution;
	if (hres > 0)
	{
		HorizontalResolution = hres;
		HorizontalStretch = static_cast<float>(HorizontalResolution) / 640.0f;
	}

	int vres = loaderSettings.VerticalResolution;
	if (vres > 0)
	{
		VerticalResolution = vres;
		VerticalStretch = static_cast<float>(VerticalResolution) / 480.0f;
	}

	if (loaderSettings.FovFix)
		fov::initialize();

	voiceLanguage = loaderSettings.VoiceLanguage;
	textLanguage = loaderSettings.TextLanguage;
	borderlessWindow = loaderSettings.WindowedFullscreen;
	scaleScreen = loaderSettings.StretchFullscreen;
	screenNum = loaderSettings.ScreenNum;
	customWindowSize = loaderSettings.CustomWindowSize;
	customWindowWidth = loaderSettings.WindowWidth;
	customWindowHeight = loaderSettings.WindowHeight;
	windowResize = loaderSettings.ResizableWindow && !customWindowSize;
	textureFilter = loaderSettings.TextureFilter;

	if (!borderlessWindow)
	{
		vector<uint8_t> nop(5, 0x90);
		WriteData((void*)0x007943D0, nop.data(), nop.size());

		// SADX automatically corrects values greater than the number of adapters available.
		// DisplayAdapter is unsigned, so -1 will be greater than the number of adapters, and it will reset.
		DisplayAdapter = screenNum - 1;
	}

	// Causes significant performance drop on some systems.
	if (windowResize)
	{
		// MeshSetBuffer_CreateVertexBuffer: Change D3DPOOL_DEFAULT to D3DPOOL_MANAGED
		WriteData((char*)0x007853F3, (char)D3DPOOL_MANAGED);
		// MeshSetBuffer_CreateVertexBuffer: Remove D3DUSAGE_DYNAMIC
		WriteData((short*)0x007853F6, (short)D3DUSAGE_WRITEONLY);
		// PolyBuff_Init: Remove D3DUSAGE_DYNAMIC and set pool to D3DPOOL_MANAGED
		WriteJump((void*)0x0079455F, PolyBuff_Init_FixVBuffParams);
	}

	pauseWhenInactive = loaderSettings.PauseWhenInactive;
	if (!pauseWhenInactive)
	{
		WriteData((uint8_t*)0x00401914, (uint8_t)0xEBu);
		// Don't pause music and sounds when the window is inactive
		WriteData<5>(reinterpret_cast<void*>(0x00401939), 0x90u);
		WriteData<5>(reinterpret_cast<void*>(0x00401920), 0x90u);
	}

	if (loaderSettings.AutoMipmap)
		mipmap::enable_auto_mipmaps();

	// Hijack a ton of functions in SADX.
	*(void**)0x38A5DB8 = (void*)0x38A5D94; // depth buffer fix
	WriteCall((void*)0x402614, SetLanguage);
	WriteCall((void*)0x437547, FixEKey);

	InitAudio();

	WriteJump(LoadSoundList, LoadSoundList_r);

	InitPatches();

	texpack::init();

	// Unprotect the .rdata section.
	SetRDataWriteProtection(false);

	// Enables GUI texture filtering (D3DTEXF_POINT -> D3DTEXF_LINEAR)
	if (textureFilter == true)
	{
		WriteCall(reinterpret_cast<void*>(0x77DBCA), Direct3D_TextureFilterPoint_ForceLinear); //njDrawPolygon
		WriteCall(reinterpret_cast<void*>(0x77DC79), Direct3D_TextureFilterPoint_ForceLinear); //njDrawTextureMemList
		WriteCall(reinterpret_cast<void*>(0x77DD99), Direct3D_TextureFilterPoint_ForceLinear); //Direct3D_EnableHudAlpha
		WriteCall(reinterpret_cast<void*>(0x77DFA0), Direct3D_TextureFilterPoint_ForceLinear); //njDrawLine2D
		WriteCall(reinterpret_cast<void*>(0x77E032), Direct3D_TextureFilterPoint_ForceLinear); //njDrawCircle2D (?)
		WriteCall(reinterpret_cast<void*>(0x77EA7E), Direct3D_TextureFilterPoint_ForceLinear); //njDrawTriangle2D
		WriteCall(reinterpret_cast<void*>(0x78B074), Direct3D_TextureFilterPoint_ForceLinear); //DrawChaoHudThingB
		WriteCall(reinterpret_cast<void*>(0x78B2F4), Direct3D_TextureFilterPoint_ForceLinear); //Some other Chao thing
		WriteCall(reinterpret_cast<void*>(0x78B4C0), Direct3D_TextureFilterPoint_ForceLinear); //DisplayDebugShape_
		WriteCall(reinterpret_cast<void*>(0x793CDD), Direct3D_EnableHudAlpha_Point); // Debug text still uses point filtering
		WriteCall(reinterpret_cast<void*>(0x6FE9F8), njDrawTextureMemList_NoFilter); // Emulator plane shouldn't be filtered
	}

	direct3d::set_vsync(loaderSettings.EnableVsync);

	if (loaderSettings.ScaleHud)
	{
		uiscale::initialize();
		hudscale::initialize();
	}

	int bgFill = loaderSettings.BackgroundFillMode;
	if (bgFill >= 0 && bgFill <= 3)
	{
		uiscale::bg_fill = static_cast<uiscale::FillMode>(bgFill);
		uiscale::setup_background_scale();
		bgscale::initialize();
	}

	int fmvFill = loaderSettings.FmvFillMode;
	if (fmvFill >= 0 && fmvFill <= 3)
	{
		uiscale::fmv_fill = static_cast<uiscale::FillMode>(fmvFill);
		uiscale::setup_fmv_scale();
	}

	// This is different from the rewrite portion of the polybuff namespace!
		polybuff::init();

	if (loaderSettings.PolyBuff)
		polybuff::rewrite_init();

	if (loaderSettings.DebugCrashLog)
		initCrashDump();

	if (loaderSettings.MaterialColorFix)
		MaterialColorFixes_Init();

	if (loaderSettings.EnableBassSFX || !Exists("system\\sounddata\\se"))
		Sound_Init(loaderSettings.SEVolume);

	//init interpol fix for helperfunctions
	interpolation::init();
	sadx_fileMap.scanSoundFolder("system\\sounddata\\bgm\\wma");
	sadx_fileMap.scanSoundFolder("system\\sounddata\\voice_jp\\wma");
	sadx_fileMap.scanSoundFolder("system\\sounddata\\voice_us\\wma");

	if (loaderSettings.Chaos2CrashFix)
		MinorPatches_Init();

	// Map of files to replace.
	// This is done with a second map instead of sadx_fileMap directly
	// in order to handle multiple mods.
	unordered_map<string, string> filereplaces;
	vector<std::pair<string, string>> fileswaps;

	vector<std::pair<ModInitFunc, string>> initfuncs;
	vector<std::pair<wstring, wstring>> errors;

	string _mainsavepath, _chaosavepath;

	bool use_redirection = false; // Check whether save redirection is used by any mods

	// It's mod loading time!
	PrintDebug("Loading mods...\n");
	// Mod list
	json json_mods = json_config["EnabledMods"];
	for (unsigned int i = 1; i <= 999; i++)
	{
		if (i > json_mods.size())
			break;
		std::string mod_fname = json_mods.at(i - 1);

		int count_m = MultiByteToWideChar(CP_UTF8, 0, mod_fname.c_str(), mod_fname.length(), NULL, 0);
		std::wstring mod_fname_w(count_m, 0);
		MultiByteToWideChar(CP_UTF8, 0, mod_fname.c_str(), mod_fname.length(), &mod_fname_w[0], count_m);

		const string mod_dirA = "mods\\" + mod_fname;
		const wstring mod_dir = L"mods\\" + mod_fname_w;
		const wstring mod_inifile = mod_dir + L"\\mod.ini";

		FILE* f_mod_ini = _wfopen(mod_inifile.c_str(), L"r");
		
		if (!f_mod_ini)
		{
			PrintDebug("Could not open file mod.ini in \"%s\".\n", mod_dirA.c_str());
			errors.emplace_back(mod_dir, L"mod.ini missing");
			continue;
		}
		unique_ptr<IniFile> ini_mod(new IniFile(f_mod_ini));
		fclose(f_mod_ini);

		const IniGroup* const modinfo = ini_mod->getGroup("");

		const string mod_nameA = modinfo->getString("Name");
		const wstring mod_name = modinfo->getWString("Name");

		PrintDebug("%u. %s\n", i, mod_nameA.c_str());

		vector<ModDependency> moddeps;

		for (unsigned int j = 1; j <= 999; j++)
		{
			char key2[14];
			snprintf(key2, sizeof(key2), "Dependency%u", j);
			if (!modinfo->hasKey(key2))
				break;
			auto dep = split(modinfo->getString(key2), '|');
			moddeps.push_back({ strdup(dep[0].c_str()), strdup(dep[1].c_str()), strdup(dep[2].c_str()), strdup(dep[3].c_str()) });
		}

		ModDependency* deparr = new ModDependency[moddeps.size()];
		memcpy(deparr, moddeps.data(), moddeps.size() * sizeof(ModDependency));

		Mod modinf = {
			strdup(mod_nameA.c_str()),
			strdup(modinfo->getString("Author").c_str()),
			strdup(modinfo->getString("Description").c_str()),
			strdup(modinfo->getString("Version").c_str()),
			strdup(mod_dirA.c_str()),
			strdup(modinfo->getString("ModID", mod_dirA).c_str()),
			NULL,
			modinfo->getBool("RedirectMainSave"),
			modinfo->getBool("RedirectChaoSave"),
			{
				deparr,
				(int)moddeps.size()
			}
		};

		if (ini_mod->hasGroup("IgnoreFiles"))
		{
			const IniGroup* group = ini_mod->getGroup("IgnoreFiles");
			auto data = group->data();
			for (const auto& iter : *data)
			{
				sadx_fileMap.addIgnoreFile(iter.first, i);
				PrintDebug("Ignored file: %s\n", iter.first.c_str());
			}
		}

		if (ini_mod->hasGroup("ReplaceFiles"))
		{
			const IniGroup* group = ini_mod->getGroup("ReplaceFiles");
			auto data = group->data();
			for (const auto& iter : *data)
			{
				filereplaces[FileMap::normalizePath(iter.first)] =
					FileMap::normalizePath(iter.second);
			}
		}

		if (ini_mod->hasGroup("SwapFiles"))
		{
			const IniGroup* group = ini_mod->getGroup("SwapFiles");
			auto data = group->data();
			for (const auto& iter : *data)
			{
				fileswaps.emplace_back(FileMap::normalizePath(iter.first),
					FileMap::normalizePath(iter.second));
			}
		}

		if (ini_mod->hasGroup("CharacterWelds"))
		{
			const IniGroup* group = ini_mod->getGroup("CharacterWelds");
			auto data = group->data();
			for (const auto& iter : *data)
				RegisterCharacterWelds(ParseCharacter(iter.first), (mod_dirA + "\\" + iter.second).c_str());
		}

		// Check for SYSTEM replacements.
		// TODO: Convert to WString.
		const string modSysDirA = mod_dirA + "\\system";
		if (DirectoryExists(modSysDirA))
			sadx_fileMap.scanFolder(modSysDirA, i);

		const string modTexDir = mod_dirA + "\\textures";
		if (DirectoryExists(modTexDir))
			sadx_fileMap.scanTextureFolder(modTexDir, i);

		const string modRepTexDir = mod_dirA + "\\replacetex";
		if (DirectoryExists(modRepTexDir))
			ScanTextureReplaceFolder(modRepTexDir, i);

		// Check if a custom EXE is required.
		if (modinfo->hasKeyNonEmpty("EXEFile"))
		{
			wstring modexe = modinfo->getWString("EXEFile");
			transform(modexe.begin(), modexe.end(), modexe.begin(), ::towlower);

			// Are we using the correct EXE?
			if (modexe != exefilename)
			{
				wchar_t msg[4096];
				swprintf(msg, LengthOfArray(msg),
					L"Mod \"%s\" should be run from \"%s\", but you are running \"%s\".\n\n"
					L"Continue anyway?", mod_name.c_str(), modexe.c_str(), exefilename.c_str());
				if (MessageBox(nullptr, msg, L"SADX Mod Loader", MB_ICONWARNING | MB_YESNO) == IDNO)
					ExitProcess(1);
			}
		}

		// Check if the mod has a DLL file.
		if (modinfo->hasKeyNonEmpty("DLLFile"))
		{
			// Prepend the mod directory.
			// TODO: SetDllDirectory()?
			wstring dll_filename = mod_dir + L'\\' + modinfo->getWString("DLLFile");
			HMODULE module = LoadLibrary(dll_filename.c_str());

			if (module == nullptr)
			{
				DWORD error = GetLastError();
				LPWSTR buffer;
				size_t size = FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
					nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL), (LPWSTR)&buffer, 0, nullptr);
				bool allocated = (size != 0);

				if (!allocated)
				{
					static const wchar_t fmterr[] = L"Unable to format error message.";
					buffer = const_cast<LPWSTR>(fmterr);
					size = LengthOfArray(fmterr) - 1;
				}
				wstring message(buffer, size);
				if (allocated)
					LocalFree(buffer);

				const string dll_filenameA = UTF16toMBS(dll_filename, CP_ACP);
				const string messageA = UTF16toMBS(message, CP_ACP);
				PrintDebug("Failed loading mod DLL \"%s\": %s\n", dll_filenameA.c_str(), messageA.c_str());
				errors.emplace_back(mod_name, L"DLL error - " + message);
			}
			else
			{
				const auto info = (const ModInfo*)GetProcAddress(module, "SADXModInfo");
				if (info)
				{
					modinf.DLLHandle = module;
					if (info->Patches)
					{
						for (int j = 0; j < info->PatchCount; j++)
							WriteData(info->Patches[j].address, info->Patches[j].data, info->Patches[j].datasize);
					}

					if (info->Jumps)
					{
						for (int j = 0; j < info->JumpCount; j++)
							WriteJump(info->Jumps[j].address, info->Jumps[j].data);
					}

					if (info->Calls)
					{
						for (int j = 0; j < info->CallCount; j++)
							WriteCall(info->Calls[j].address, info->Calls[j].data);
					}

					if (info->Pointers)
					{
						for (int j = 0; j < info->PointerCount; j++)
							WriteData((void**)info->Pointers[j].address, info->Pointers[j].data);
					}

					if (info->Init)
					{
						// TODO: Convert to Unicode later. (Will require an API bump.)
						initfuncs.emplace_back(info->Init, mod_dirA);
					}

					const auto init = (const ModInitFunc)GetProcAddress(module, "Init");

					if (init)
					{
						initfuncs.emplace_back(init, mod_dirA);
					}

					const auto* const patches = (const PatchList*)GetProcAddress(module, "Patches");

					if (patches)
					{
						for (int j = 0; j < patches->Count; j++)
						{
							WriteData(patches->Patches[j].address, patches->Patches[j].data, patches->Patches[j].datasize);
						}
					}

					const auto* const jumps = (const PointerList*)GetProcAddress(module, "Jumps");

					if (jumps)
					{
						for (int j = 0; j < jumps->Count; j++)
						{
							WriteJump(jumps->Pointers[j].address, jumps->Pointers[j].data);
						}
					}

					const auto* const calls = (const PointerList*)GetProcAddress(module, "Calls");

					if (calls)
					{
						for (int j = 0; j < calls->Count; j++)
						{
							WriteCall(calls->Pointers[j].address, calls->Pointers[j].data);
						}
					}

					const auto* const pointers = (const PointerList*)GetProcAddress(module, "Pointers");

					if (pointers)
					{
						for (int j = 0; j < pointers->Count; j++)
						{
							WriteData((void**)pointers->Pointers[j].address, pointers->Pointers[j].data);
						}
					}

					RegisterEvent(modInitEndEvents, module, "OnInitEnd");
					RegisterEvent(modFrameEvents, module, "OnFrame");
					RegisterEvent(modInputEvents, module, "OnInput");
					RegisterEvent(modControlEvents, module, "OnControl");
					RegisterEvent(modExitEvents, module, "OnExit");
					RegisterEvent(modRenderDeviceLost, module, "OnRenderDeviceLost");
					RegisterEvent(modRenderDeviceReset, module, "OnRenderDeviceReset");
					RegisterEvent(onRenderSceneEnd, module, "OnRenderSceneEnd");
					RegisterEvent(onRenderSceneStart, module, "OnRenderSceneStart");

					auto customTextureLoad = reinterpret_cast<TextureLoadEvent>(GetProcAddress(module, "OnCustomTextureLoad"));
					if (customTextureLoad != nullptr)
					{
						modCustomTextureLoadEvents.push_back(customTextureLoad);
					}
				}
				else
				{
					const string dll_filenameA = UTF16toMBS(dll_filename, CP_ACP);
					PrintDebug("File \"%s\" is not a valid mod file.\n", dll_filenameA.c_str());
					errors.emplace_back(mod_name, L"Not a valid mod file.");
				}
			}
		}

		// Check if the mod has EXE data replacements.
		if (modinfo->hasKeyNonEmpty("EXEData"))
		{
			wchar_t filename[MAX_PATH];
			swprintf(filename, LengthOfArray(filename), L"%s\\%s",
				mod_dir.c_str(), modinfo->getWString("EXEData").c_str());
			ProcessEXEData(filename, mod_dir);
		}

		// Check if the mod has DLL data replacements.
		for (unsigned int j = 0; j < LengthOfArray(dlldatakeys); j++)
		{
			if (modinfo->hasKeyNonEmpty(dlldatakeys[j]))
			{
				wchar_t filename[MAX_PATH];
				swprintf(filename, LengthOfArray(filename), L"%s\\%s",
					mod_dir.c_str(), modinfo->getWString(dlldatakeys[j]).c_str());
				ProcessDLLData(filename, mod_dir);
			}
		}

		if (modinfo->getBool("RedirectMainSave")) {
			_mainsavepath = mod_dirA + "\\SAVEDATA";

			if (!IsPathExist(_mainsavepath))
			{
				_mkdir(_mainsavepath.c_str());
			}
			use_redirection = true;
		}

		if (modinfo->getBool("RedirectChaoSave")) {

			_chaosavepath = mod_dirA + "\\SAVEDATA";

			if (!IsPathExist(_chaosavepath))
			{
				_mkdir(_chaosavepath.c_str());
			}
			use_redirection = true;
		}

		if (modinfo->hasKeyNonEmpty("WindowTitle"))
			windowtitle = modinfo->getString("WindowTitle");

		if (modinfo->hasKeyNonEmpty("BorderImage"))
			borderimg = mod_dir + L'\\' + modinfo->getWString("BorderImage");
		modlist.push_back(modinf);
	}

	if (loaderSettings.InputMod)
		SDL2_Init();

	if (!errors.empty())
	{
		std::wstringstream message;
		message << L"The following mods didn't load correctly:" << std::endl;

		for (auto& i : errors)
		{
			message << std::endl << i.first << ": " << i.second;
		}

		MessageBox(nullptr, message.str().c_str(), L"Mods failed to load", MB_OK | MB_ICONERROR);

		// Clear the errors vector to free memory.
		errors.clear();
		errors.shrink_to_fit();
	}

	// Replace filenames. ("ReplaceFiles")
	for (const auto& filereplace : filereplaces)
	{
		sadx_fileMap.addReplaceFile(filereplace.first, filereplace.second);
	}
	for (const auto& fileswap : fileswaps)
	{
		sadx_fileMap.swapFiles(fileswap.first, fileswap.second);
	}

	for (auto& initfunc : initfuncs)
		initfunc.first(initfunc.second.c_str(), helperFunctions);

	ProcessTestSpawn(helperFunctions);

	for (const auto& i : StartPositions)
	{
		auto poslist = &i.second;
		auto newlist = new StartPosition[poslist->size() + 1];
		StartPosition* cur = newlist;

		for (const auto& j : *poslist)
		{
			*cur++ = j.second;
		}

		cur->LevelID = LevelIDs_Invalid;
		switch (i.first)
		{
		default:
		case Characters_Sonic:
			WriteData((StartPosition**)0x41491E, newlist);
			break;
		case Characters_Tails:
			WriteData((StartPosition**)0x414925, newlist);
			break;
		case Characters_Knuckles:
			WriteData((StartPosition**)0x41492C, newlist);
			break;
		case Characters_Amy:
			WriteData((StartPosition**)0x41493A, newlist);
			break;
		case Characters_Gamma:
			WriteData((StartPosition**)0x414941, newlist);
			break;
		case Characters_Big:
			WriteData((StartPosition**)0x414933, newlist);
			break;
		}
	}
	StartPositions.clear();

	for (const auto& i : FieldStartPositions)
	{
		auto poslist = &i.second;
		auto newlist = new FieldStartPosition[poslist->size() + 1];
		FieldStartPosition* cur = newlist;

		for (const auto& j : *poslist)
		{
			*cur++ = j.second;
		}

		cur->LevelID = LevelIDs_Invalid;
		StartPosList_FieldReturn[i.first] = newlist;
	}
	FieldStartPositions.clear();

	if (PathsInitialized)
	{
		auto newlist = new PathDataPtr[Paths.size() + 1];
		PathDataPtr* cur = newlist;

		for (const auto& path : Paths)
		{
			*cur++ = path.second;
		}

		cur->LevelAct = 0xFFFF;
		WriteData((PathDataPtr**)0x49C1A1, newlist);
		WriteData((PathDataPtr**)0x49C1AF, newlist);
	}
	Paths.clear();

	for (const auto& pvm : CharacterPVMs)
	{
		const vector<PVMEntry>* pvmlist = &pvm.second;
		auto size = pvmlist->size();
		auto newlist = new PVMEntry[size + 1];
		memcpy(newlist, pvmlist->data(), sizeof(PVMEntry) * size);
		newlist[size].TexList = nullptr;
		CharacterPVMEntries[pvm.first] = newlist;
	}
	CharacterPVMs.clear();

	if (CommonObjectPVMsInitialized)
	{
		auto size = CommonObjectPVMs.size();
		auto newlist = new PVMEntry[size + 1];
		//PVMEntry *cur = newlist;
		memcpy(newlist, CommonObjectPVMs.data(), sizeof(PVMEntry) * size);
		newlist[size].TexList = nullptr;
		TexLists_ObjRegular[0] = newlist;
		TexLists_ObjRegular[1] = newlist;
	}
	CommonObjectPVMs.clear();

	for (const auto& level : _TrialLevels)
	{
		const vector<TrialLevelListEntry>* levellist = &level.second;
		auto size = levellist->size();
		auto newlist = new TrialLevelListEntry[size];
		memcpy(newlist, levellist->data(), sizeof(TrialLevelListEntry) * size);
		TrialLevels[level.first].Levels = newlist;
		TrialLevels[level.first].Count = size;
	}
	_TrialLevels.clear();

	for (const auto& subgame : _TrialSubgames)
	{
		const vector<TrialLevelListEntry>* levellist = &subgame.second;
		auto size = levellist->size();
		auto newlist = new TrialLevelListEntry[size];
		memcpy(newlist, levellist->data(), sizeof(TrialLevelListEntry) * size);
		TrialSubgames[subgame.first].Levels = newlist;
		TrialSubgames[subgame.first].Count = size;
	}
	_TrialSubgames.clear();

	if (!_mainsavepath.empty())
	{
		char* buf = new char[_mainsavepath.size() + 1];
		strncpy(buf, _mainsavepath.c_str(), _mainsavepath.size() + 1);
		mainsavepath = buf;
		string tmp = "./" + _mainsavepath + "/";
		WriteData((char*)0x42213D, (char)(tmp.size() + 1)); // Write
		buf = new char[tmp.size() + 1];
		strncpy(buf, tmp.c_str(), tmp.size() + 1);
		WriteData((char**)0x422020, buf); // Write
		tmp = "./" + _mainsavepath + "/%s";
		buf = new char[tmp.size() + 1];
		strncpy(buf, tmp.c_str(), tmp.size() + 1);
		WriteData((char**)0x421E4E, buf); // Load
		WriteData((char**)0x421E6A, buf); // Load
		WriteData((char**)0x421F07, buf); // Delete
		WriteData((char**)0x42214E, buf); // Write
		WriteData((char**)0x5050E5, buf); // Count
		WriteData((char**)0x5051ED, buf); // Count
		tmp = "./" + _mainsavepath + "/SonicDX%02d.snc";
		WriteData((char*)0x422064, (char)(tmp.size() - 1)); // Write
		buf = new char[tmp.size() + 1];
		strncpy(buf, tmp.c_str(), tmp.size() + 1);
		WriteData((char**)0x42210F, buf); // Write
		tmp = "./" + _mainsavepath + "/SonicDX??.snc";
		buf = new char[tmp.size() + 1];
		strncpy(buf, tmp.c_str(), tmp.size() + 1);
		WriteData((char**)0x5050AB, buf); // Count
	}

	if (!_chaosavepath.empty())
	{
		char* buf = new char[_chaosavepath.size() + 1];
		strncpy(buf, _chaosavepath.c_str(), _chaosavepath.size() + 1);
		chaosavepath = buf;
		string tmp = "./" + _chaosavepath + "/SONICADVENTURE_DX_CHAOGARDEN.snc";
		buf = new char[tmp.size() + 1];
		strncpy(buf, tmp.c_str(), tmp.size() + 1);
		WriteData((char**)0x7163EF, buf); // ALMC_Read
		WriteData((char**)0x71AA6F, buf); // al_confirmsave
		WriteData((char**)0x71ACDB, buf); // al_confirmload
		WriteData((char**)0x71ADC5, buf); // al_confirmload
	}

	if (!windowtitle.empty())
	{
		char* buf = new char[windowtitle.size() + 1];
		strncpy(buf, windowtitle.c_str(), windowtitle.size() + 1);
		*(char**)0x892944 = buf;
	}

	if (!_MusicList.empty())
	{
		auto size = _MusicList.size();
		auto newlist = new MusicInfo[size];
		memcpy(newlist, _MusicList.data(), sizeof(MusicInfo) * size);
		WriteData((const char***)0x425424, &newlist->Name);
		WriteData((int**)0x425442, &newlist->Loop);
		WriteData((const char***)0x425460, &newlist->Name);
		WriteData((int**)0x42547E, &newlist->Loop);
	}
	_MusicList.clear();

	ProcessVoiceDurationRegisters();

	RaiseEvents(modInitEndEvents);
	PrintDebug("Finished loading mods\n");

	// Check for patches.
	ifstream patches_str("mods\\Patches.dat", ifstream::binary);
	if (patches_str.is_open())
	{
		CodeParser patchParser;
		static const char codemagic[6] = { 'c', 'o', 'd', 'e', 'v', '5' };
		char buf[sizeof(codemagic)];
		patches_str.read(buf, sizeof(buf));
		if (!memcmp(buf, codemagic, sizeof(codemagic)))
		{
			int codecount_header;
			patches_str.read((char*)&codecount_header, sizeof(codecount_header));
			PrintDebug("Loading %d patches...\n", codecount_header);
			patches_str.seekg(0);
			int codecount = patchParser.readCodes(patches_str);
			if (codecount >= 0)
			{
				PrintDebug("Loaded %d patches.\n", codecount);
				patchParser.processCodeList();
			}
			else
			{
				PrintDebug("ERROR loading patches: ");
				switch (codecount)
				{
				case -EINVAL:
					PrintDebug("Patch file is not in the correct format.\n");
					break;
				default:
					PrintDebug("%s\n", strerror(-codecount));
					break;
				}
			}
		}
		else
		{
			PrintDebug("Patch file is not in the correct format.\n");
		}
		patches_str.close();
	}


	// Check for codes.
	ifstream codes_str("mods\\Codes.dat", ifstream::binary);

	if (codes_str.is_open())
	{
		static const char codemagic[6] = { 'c', 'o', 'd', 'e', 'v', '5' };
		char buf[sizeof(codemagic)];
		codes_str.read(buf, sizeof(buf));

		if (!memcmp(buf, codemagic, sizeof(codemagic)))
		{
			int codecount_header;
			codes_str.read((char*)&codecount_header, sizeof(codecount_header));
			PrintDebug("Loading %d codes...\n", codecount_header);
			codes_str.seekg(0);
			int codecount = codeParser.readCodes(codes_str);

			if (codecount >= 0)
			{
				PrintDebug("Loaded %d codes.\n", codecount);
				codeParser.processCodeList();
			}
			else
			{
				PrintDebug("ERROR loading codes: ");
				switch (codecount)
				{
				case -EINVAL:
					PrintDebug("Code file is not in the correct format.\n");
					break;
				default:
					PrintDebug("%s\n", strerror(-codecount));
					break;
				}
			}
		}
		else
		{
			PrintDebug("Code file is not in the correct format.\n");
		}

		codes_str.close();
	}

	// Sets up code/event handling
	WriteJump((void*)0x00426063, (void*)ProcessCodes);
	WriteJump((void*)0x0040FDB3, (void*)OnInput);			// End of first chunk
	WriteJump((void*)0x0042F1C5, (void*)OnInput_MidJump);	// Cutscene stuff - Untested. Couldn't trigger ingame.
	WriteJump((void*)0x0042F1E9, (void*)OnInput);			// Cutscene stuff
	WriteJump((void*)0x0040FF00, (void*)OnControl);

	// Remove "Tails Adventure" gray filter
	WriteData(reinterpret_cast<float*>(0x87CBA8), 0.0f);
	WriteData(reinterpret_cast<float*>(0x87CBAC), 0.0f);

	ApplyTestSpawn();
	GVR_Init();
	if (loaderSettings.ExtendedSaveSupport)
		ExtendedSaveSupport_Init();
}

DataPointer(HMODULE, chrmodelshandle, 0x3AB9170);

static void __cdecl LoadChrmodels()
{
	chrmodelshandle = LoadLibrary(L".\\system\\CHRMODELS_orig.dll");
	if (!chrmodelshandle)
	{
		MessageBox(nullptr, L"CHRMODELS_orig.dll could not be loaded!\n\n"
			L"SADX will now proceed to abruptly exit.",
			L"SADX Mod Loader", MB_ICONERROR);
		ExitProcess(1);
	}
	SetDLLHandle(L"CHRMODELS", chrmodelshandle);
	SetDLLHandle(L"CHRMODELS_orig", chrmodelshandle);
	SetDLLHandle(L"CHAOSTGGARDEN02MR_DAYTIME", LoadLibrary(L".\\system\\CHAOSTGGARDEN02MR_DAYTIME.DLL"));
	SetDLLHandle(L"CHAOSTGGARDEN02MR_EVENING", LoadLibrary(L".\\system\\CHAOSTGGARDEN02MR_EVENING.DLL"));
	SetDLLHandle(L"CHAOSTGGARDEN02MR_NIGHT", LoadLibrary(L".\\system\\CHAOSTGGARDEN02MR_NIGHT.DLL"));
	WriteCall((void*)0x402513, (void*)InitMods);
}

/**
 * DLL entry point.
 * @param hinstDll DLL instance.
 * @param fdwReason Reason for calling DllMain.
 * @param lpvReserved Reserved.
 */
BOOL APIENTRY DllMain(HINSTANCE hinstDll, DWORD fdwReason, LPVOID lpvReserved)
{
	// US version check.
	static const void* const verchk_addr = (void*)0x789E50;
	static const uint8_t verchk_data[] = { 0x83, 0xEC, 0x28, 0x57, 0x33 };

	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		g_hinstDll = hinstDll;
		HookCreateFileA();

		// Make sure this is the correct version of SADX.
		if (memcmp(verchk_data, verchk_addr, sizeof(verchk_data)) != 0)
		{
			ShowNon2004USError();
			ExitProcess(1);
		}

		WriteData((unsigned char*)0x401AE1, (unsigned char)0x90);
		WriteCall((void*)0x401AE2, (void*)LoadChrmodels);

#if !defined(_MSC_VER) || defined(_DLL)
		// Disable thread library calls, since we don't
		// care about thread attachments.
		// NOTE: On MSVC, don't do this if using the static CRT.
		// Reference: https://msdn.microsoft.com/en-us/library/windows/desktop/ms682579(v=vs.85).aspx
		DisableThreadLibraryCalls(hinstDll);
#endif /* !defined(_MSC_VER) || defined(_DLL) */
		break;

	case DLL_PROCESS_DETACH:
		// Make sure the log file is closed.
		if (dbgFile)
		{
			fclose(dbgFile);
			dbgFile = nullptr;
		}
		break;

	default:
		break;
	}

	return TRUE;
}
