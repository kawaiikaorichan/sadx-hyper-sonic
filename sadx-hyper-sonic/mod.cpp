#include "SADXModLoader.h"
#include "stdafx.h"

HelperFunctions helperFunctionsGlobal;
#define ReplacePVM(a, b) helperFunctionsGlobal.ReplaceFile("system\\" a ".PVM", "system\\" b ".PVM");

static constexpr int max_players = 8;

static bool DCChars = false;

DataPointer(char, byte_3B2A2FA, 0x3B2A2FA);

static bool HyperSonic[max_players]{}; // One per possible player
static ObjectMaster* InvincibilityObject[max_players]{};

void __cdecl PlayVoice_r(int voice);
Trampoline PlayVoice_t((int)PlayVoice, (int)PlayVoice + 0x5, PlayVoice_r);
void __cdecl PlayVoice_r(int voice) {
	// If about to transform into Hyper Sonic OR in menu, replace the grunt voice
	if (CountEmblems(&SaveFile) >= 130 && (Rings >= 100 || GameState != 15) && voice == 396) {
		voice = 9500;
	}

	// If Hyper Sonic
	if (HyperSonic[0] == true) {
		switch (voice) {
		case 1849: voice = 9501; break; // 1UP, INV
		case 1850: voice = 9502; break; // Speed shoes
		case 388: voice = 9503; break; // Unused Super(?) Sonic dialogue
		case 1843: voice = 9504; break; // Sonic boss defeat dialogue
		}
	}

	((decltype(PlayVoice_r)*)PlayVoice_t.Target())(voice); // call original function
}

void __cdecl Sonic_Act1_r(EntityData1* data, EntityData2* data2, CharObj2* co2);
Trampoline Sonic_Act1_t(0x496F50, 0x496F59, Sonic_Act1_r);
void __cdecl Sonic_Act1_r(EntityData1* data, EntityData2* data2, CharObj2* co2) {
	if (co2 == nullptr) {
		return;
	}

	int id = data->CharIndex;
	
	if (HyperSonic[id] == true) {
		// This prevents Hyper Sonic from drowning
		co2->UnderwaterTime = 0;

		// This prevent the object from deleting itself, the timer is in "Object".
		if (InvincibilityObject[id] != nullptr && InvincibilityObject[id]->Data1 != nullptr) InvincibilityObject[id]->Data1->Object = nullptr;
		
		if ((data->Status & Status_DoNextAction && data->NextAction == 47) || (co2->Upgrades & Upgrades_SuperSonic) != Upgrades_SuperSonic) {
			HyperSonic[id] = false;
			
			if (InvincibilityObject[id] != nullptr && InvincibilityObject[id]->Data1 != nullptr) {
				DeleteObject_(InvincibilityObject[id]);
				InvincibilityObject[id] = nullptr;
			}

			// Restore physics
			co2->PhysicsData.GroundAccel = PhysicsArray[data->CharID].GroundAccel;
			co2->PhysicsData.MaxAccel = PhysicsArray[data->CharID].MaxAccel;
			co2->PhysicsData.field_68 = PhysicsArray[data->CharID].field_68;

			co2->Powerups &= ~Powerups_Invincibility;
		}
	}
	else {
		if (data->Status & Status_DoNextAction && data->NextAction == 46) {
			njReleaseTexture(&SUPERSONIC_TEXLIST);

			if (CountEmblems(&SaveFile) >= 130 && (Rings >=100 || LastStoryFlag ==1)){
				HyperSonic[id] = true;

				if (DCChars == true) {
					LoadPVM("HYPERSONIC_DC", &SUPERSONIC_TEXLIST);
				}
				else {
					LoadPVM("HYPERSONIC", &SUPERSONIC_TEXLIST);
				}
				
				InvincibilityObject[id] = LoadObject(LoadObj_Data1, 2, Invincibility_Load);
				InvincibilityObject[id]->Data1->CharIndex = id;

				// Speed boost
				co2->PhysicsData.GroundAccel = PhysicsArray[data->CharID].GroundAccel * 2;
				co2->PhysicsData.MaxAccel = PhysicsArray[data->CharID].MaxAccel * 2;
				co2->PhysicsData.field_68 = PhysicsArray[data->CharID].field_68 * 2;
			}
			else {
				LoadPVM("SUPERSONIC", &SUPERSONIC_TEXLIST);
			}
		}
	}

	((decltype(Sonic_Act1_r)*)Sonic_Act1_t.Target())(data, data2, co2); // call original function
}

void swapSS()
{
	SetDefaultAlphaBlend();

	if ((CountEmblems(&SaveFile) >= 130))
	{
		ReplacePVM("M_CHNAM", "M_CHNAM_hyper");
		ReplacePVM("B_CHNAM_E", "B_CHNAM_E_hyper");
		ReplacePVM("B_CHNAM", "B_CHNAM_hyper");
	}
}

extern "C"
{
	__declspec(dllexport) ModInfo SADXModInfo = { ModLoaderVer, nullptr, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0 };

	__declspec(dllexport) void __cdecl Init(const char* path, const HelperFunctions &helperFunctions)
	{
		helperFunctionsGlobal = helperFunctions;
		WriteCall((void*)0x00512460, (void*)swapSS);
	}
	
	void __declspec(dllexport) OnInitEnd(const char* path, HelperFunctions* helper)
	{
		DCChars = GetModuleHandle(L"SA1_Chars") != NULL;
	}

}