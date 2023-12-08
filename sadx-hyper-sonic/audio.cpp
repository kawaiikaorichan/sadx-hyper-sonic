#include "pch.h"

static constexpr int max_players = 8;
static bool HyperSonic[max_players]{}; // One per possible player

void __cdecl PlayVoice_r(int voice);
Trampoline PlayVoice_t((int)PlayVoice, (int)PlayVoice + 0x5, PlayVoice_r);
void __cdecl PlayVoice_r(int voice) {
	// If about to transform into Hyper Sonic OR in menu, replace the grunt voice
	if (CountEmblems(&SaveFile) >= 130 && (Rings >= 100 || GameState != 15) && voice == 396) {
		voice = 9500;
	}

	// If Hyper Sonic
	if (HyperSonic[0] == true)
	{
		switch (voice)
		{
			case 1849: voice = 9501; break; // 1UP, INV
			case 1850: voice = 9502; break; // Speed shoes
			case 388: voice = 9503; break; // Unused Super(?) Sonic dialogue
			case 1843: voice = 9504; break; // Sonic boss defeat dialogue
		}
	}

	((decltype(PlayVoice_r)*)PlayVoice_t.Target())(voice); // call original function
}