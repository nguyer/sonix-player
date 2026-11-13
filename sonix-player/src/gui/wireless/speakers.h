#ifndef SPEAKERS_PAGE_H
#define SPEAKERS_PAGE_H
#include "src/gui/shell/gui.h"
extern lv_obj_t *speakers_screen;
void speakers_page_init(gui_config_t *cfg);
void speakers_output_open(void);
#endif
