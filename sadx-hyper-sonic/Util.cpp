#include "pch.h"

bool isPlayerOnSuperForm(int player)
{
	if (CharObj2Ptrs[player] && CharObj2Ptrs[player]->Upgrades & Upgrades_SuperSonic)
		return true;

	return false;
}

bool isPerfectChaosLevel()
{
	return CurrentLevel == LevelIDs_PerfectChaos && CurrentAct == 0;
}

void njRotateX_(Angle x)
{
	if (x)
	{
		njRotateX(_nj_current_matrix_ptr_, x);
	}
}

void njRotateY_(Angle y)
{
	if (y)
	{
		njRotateY(_nj_current_matrix_ptr_, y);
	}
}

void njRotateZ_(Angle z)
{
	if (z)
	{
		njRotateZ(_nj_current_matrix_ptr_, z);
	}
}