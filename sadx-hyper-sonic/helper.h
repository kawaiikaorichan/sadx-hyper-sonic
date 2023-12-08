#pragma once

extern bool isHyperSonic;
extern int charType;
extern bool MultiModEnabled;
extern bool lifeIcon;

extern bool isDCConv;
extern bool hudPlus;

#define TaskHook static FunctionHook<void, task*>
#define pNum data->counter.b[0]

HelperFunctions help;

void __cdecl HyperSonic_Init(const char* path, const HelperFunctions& helperFunctions);
void initConfig(const char* path);

enum charTypeE
{
	none,
	Dreamcast,
	DX,
};