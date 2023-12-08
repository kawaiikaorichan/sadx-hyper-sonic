#include "pch.h"

HelperFunctions help;

bool MultiModEnabled = false;
bool isDCConv = false;
bool hudPlus = false;

extern "C"
{

	__declspec(dllexport) void __cdecl Init(const char* path, const HelperFunctions& helperFunctions)
	{
		if (helperFunctions.Version < 13)
		{
			MessageBox(WindowHandle,
				L"Please update SADX Mod Loader. Hyper Sonic mod requires API version 13 or newer.",
				L"Hyper Sonic Error", MB_OK | MB_ICONERROR);
		}

		help = helperFunctions;

		initConfig(path);

		HyperSonic_Init(path, helperFunctions); //Main Code to Load Hyper Sonic
		
		MultiModEnabled = GetModuleHandle(L"sadx-multiplayer") != nullptr;
		isDCConv = GetModuleHandle(L"DCMods_Main") != NULL;
		hudPlus = GetModuleHandleW(L"sadx-hud-plus") != nullptr;
	}


	__declspec(dllexport) void __cdecl OnFrame()
	{
		return;
	}

	__declspec(dllexport) ModInfo SADXModInfo = { ModLoaderVer };

}