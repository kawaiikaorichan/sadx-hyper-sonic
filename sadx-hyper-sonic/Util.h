#pragma once

bool isPlayerOnSuperForm(int player);

bool isPerfectChaosLevel();
NJS_TEXLIST* getHyperSonicTex();

#define TWP_PNUM(twp) twp->counter.b[0]
#define TWP_CHAR(twp) twp->counter.b[1]