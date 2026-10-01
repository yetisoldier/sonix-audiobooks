#ifndef AUDIOBOOKEXTRAS_H
#define AUDIOBOOKEXTRAS_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

extern lv_obj_t *audiobookfolders_screen;
extern lv_obj_t *audiobookmarks_screen;
extern lv_obj_t *audiobookmarklist_screen;
extern lv_obj_t *audiobooksummary_screen;

void audiobookextras_init(gui_config_t *cfg);

void audiobookfolders_open(void);
void audiobookmarks_open(void);
void audiobookmarks_open_current(void);
void audiobookmarks_add_current(void);
void audiobooksummary_open_current(void);
void audiobooksummary_open(const char *book, bool from_player);

#endif /* AUDIOBOOKEXTRAS_H */
