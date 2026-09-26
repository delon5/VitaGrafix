#ifndef _OSD_H_
#define _OSD_H_

void osd_update_fb(const SceDisplayFrameBuf *fb);
void osd_set_back_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void osd_set_text_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a);

uint32_t osd_get_text_width(const char *str);
uint32_t osd_get_text_height();
uint32_t osd_get_text_width_small(const char *str);
uint32_t osd_get_text_height_small();
int osd_get_text_end_x(int x, const char *str);

void osd_draw_rounded_rectangle(int x, int y, int width, int height, int radius);

void osd_draw_string(int x, int y, const char *str);
void osd_draw_string_small(int x, int y, const char *str);
void osd_draw_log(int x, int y, int maxy, const char *str);
void osd_draw_logo(int x, int y);

#endif
