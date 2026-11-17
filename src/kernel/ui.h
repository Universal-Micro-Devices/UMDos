/* ui.h */
#ifndef UI_H
#define UI_H

#include "bootinfo.h"

void ui_splash(const bootinfo_t *bi);
int  ui_login(const bootinfo_t *bi);
void ui_desktop(const bootinfo_t *bi);

#endif
