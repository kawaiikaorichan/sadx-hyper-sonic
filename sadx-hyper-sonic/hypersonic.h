#pragma once

NJS_TEXNAME HYPERSONIC[73];
NJS_TEXNAME HYPERSONIC_DC[80];

NJS_TEXLIST HyperSonic_TEXLIST = { arrayptrandlength(HYPERSONIC) };
NJS_TEXLIST HyperSonicDC_TEXLIST = { arrayptrandlength(HYPERSONIC_DC) };

PVMEntry HyperSonicEntry[] =
{
	{"SONIC", &SONIC_TEXLIST},
	{"SON_EFF", &SON_EFF_TEXLIST  },
	{"SUPERSONIC", &SUPERSONIC_TEXLIST},
	{"HYPERSONIC", &HyperSonic_TEXLIST}
};

PVMEntry HyperSonicEntry[] =
{
	{"SONIC", &SONIC_TEXLIST},
	{"SON_EFF", &SON_EFF_TEXLIST  },
	{"SUPERSONIC", &SUPERSONIC_TEXLIST},
	{"HYPERSONIC_DC", &HyperSonicDC_TEXLIST}
};

void InitHS();