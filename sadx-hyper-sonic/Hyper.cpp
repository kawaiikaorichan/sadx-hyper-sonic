#include "pch.h"
#include "hypersonic.h"
#include "multiapi.h"

TaskHook Sonic_Main_t(SonicTheHedgehog);
TaskHook Sonic_Display_t(SonicDisplay);
TaskHook Sonic_Delete_t(SonicDestruct);
UsercallFunc(signed int, SuperSonicNAct_t, (motionwk2* a1, playerwk* a2, taskwk* a3), (a1, a2, a3), 0x494CD0, rEAX, rEAX, rECX, rESI);
UsercallFunc(signed int, SonicNAct_t, (taskwk* a1, playerwk* a2, motionwk2* a3), (a1, a2, a3), 0x496880, rEAX, rECX, rEAX, stack4);

static constexpr int max_players = 8;
static bool HyperSonic[max_players]{};
static ObjectMaster* InvincibilityObject[max_players]{};
static int RingTimer[8] = {};

bool IsSuperSonic(playerwk* pwp)
{
	return (pwp->equipment & Upgrades_SuperSonic);
}

bool IsSuperSonic(int pnum)
{
	return playerpwp[pnum] && IsSuperSonic(playerpwp[pnum]);
}

static void Sonic_Display_r(task* tsk)
{
	auto data = tsk->twp;

	isHyperSonic = isPlayerOnSuperForm(pNum) == true ? 1 : 0;

	Sonic_Display_t.Original(tsk);
}

static void TransformSuperSonic(taskwk* twp, playerwk* pwp)
{
	auto pnum = TWP_PNUM(twp);
	SetInputP(pnum, PL_OP_SUPER);
	RingTimer[pnum] = 0;
}

NJS_TEXLIST* getHyperSonicTex()
{
	switch (charType)
	{
	case Dreamcast:
		return &HyperSonic_TEXLIST;
	case DX:
		return &HyperSonicDC_TEXLIST;
	case none:
	default:
		return &SUPERSONIC_TEXLIST;
	}
}

Sint32 __cdecl setHyperSonicTexture(NJS_TEXLIST* texlist)
{
	if (isHyperSonic && charType != none)
	{
		texlist = getHyperSonicTex();
	}

	return njSetTexture(texlist);
}

void SubRings(unsigned char player, EntityData1* data)
{
	if (isHyperSonic && FrameCounterUnpaused % 60 == 0 && Rings > 0)
	{
		AddRings(-2);
	}

	return;
}

void SetHyperSonic(CharObj2* co2, EntityData1* data1)
{
	taskwk* taskw = (taskwk*)data1;

	co2->Upgrades |= Upgrades_SuperSonic;


}