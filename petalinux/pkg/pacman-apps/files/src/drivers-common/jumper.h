#ifndef __JUMPER_H__
#define __JUMPER_H__

#ifdef __cplusplus
extern "C" {
#endif

// J5: production jumper, on MIO9 (pull-up enabled in the block design).
// Installed in production systems: development tools refuse to run with it
// installed, production tools may require it.

#define JUMPER_J5_MIO 9

// jumper_installed() return values:
#define JUMPER_OUT          0
#define JUMPER_IN           1
#define JUMPER_UNREADABLE  -1

void init_jumper();
int  jumper_installed();

#ifdef __cplusplus
}
#endif

#endif // __JUMPER_H__
