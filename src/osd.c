#include <vitasdk.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "osd.h"
#include "osd_font.h"
#include "osd_logo.h"

#define OSD_RESCALE_X(x) (int)((x) * (g_framebuf.width / 960.0f))
#define OSD_RESCALE_Y(y) (int)((y) * (g_framebuf.height / 544.0f))

typedef union {
    struct {
        uint8_t r;
        uint8_t g;
        uint8_t b;
        uint8_t a;
    } rgba;
    uint32_t uint32;
} rgba_t;

static SceDisplayFrameBuf g_framebuf;
static const bitmap_font_t *g_font = &g_fonts[0];

static rgba_t g_color_text = {.rgba = {255, 255, 255, 255}};
static rgba_t g_color_bg   = {.rgba = {  0,   0,   0, 255}};


// Clips a rectangle in framebuffer pixels, returns false when nothing is left
static bool osd_clip(int *x, int *y, int *width, int *height) {
    if (*x < 0) {
        *width += *x;
        *x = 0;
    }
    if (*y < 0) {
        *height += *y;
        *y = 0;
    }
    if (*x + *width > (int)g_framebuf.width) {
        *width = (int)g_framebuf.width - *x;
    }
    if (*y + *height > (int)g_framebuf.height) {
        *height = (int)g_framebuf.height - *y;
    }
    return *width > 0 && *height > 0;
}

static rgba_t osd_blend_color(rgba_t fg, rgba_t bg) {
    uint8_t inv_alpha = 255 - fg.rgba.a;

    rgba_t result;
    result.rgba.b = ((fg.rgba.a * fg.rgba.b + inv_alpha * bg.rgba.b) >> 8); // B
    result.rgba.g = ((fg.rgba.a * fg.rgba.g + inv_alpha * bg.rgba.g) >> 8); // G
    result.rgba.r = ((fg.rgba.a * fg.rgba.r + inv_alpha * bg.rgba.r) >> 8); // R
    result.rgba.a = 0xFF;                                                   // A
    return result;
}

static void osd_fill_color(rgba_t *pixels, int count) {
    if (g_color_bg.rgba.r == g_color_bg.rgba.g
            && g_color_bg.rgba.r == g_color_bg.rgba.b
            && g_color_bg.rgba.r == g_color_bg.rgba.a) {
        memset(pixels, g_color_bg.rgba.r, sizeof(rgba_t) * count);
        return;
    }

    for (int i = 0; i < count; i++) {
        pixels[i] = g_color_bg;
    }
}

void osd_update_fb(const SceDisplayFrameBuf *fb) {
    memcpy(&g_framebuf, fb, sizeof(SceDisplayFrameBuf));

    if (fb->width <= 480) {
        g_font = &g_fonts[3];
    } else if (fb->width <= 640) {
        g_font = &g_fonts[2];
    } else if (fb->width <= 720) {
        g_font = &g_fonts[1];
    } else {
        g_font = &g_fonts[0];
    }
}

void osd_set_text_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    g_color_text.rgba.r = r;
    g_color_text.rgba.g = g;
    g_color_text.rgba.b = b;
    g_color_text.rgba.a = a;
}

void osd_set_back_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    g_color_bg.rgba.r = r;
    g_color_bg.rgba.g = g;
    g_color_bg.rgba.b = b;
    g_color_bg.rgba.a = a;
}

static uint32_t osd_get_text_width_abs(const char *str) {
    return strlen(str) * g_font->width;
}

uint32_t osd_get_text_width(const char *str) {
    return osd_get_text_width_abs(str) * 960.0f / g_framebuf.width;
}

uint32_t osd_get_text_height() {
    // Rounded up, so lines this far apart never overlap in the framebuffer
    return (g_font->height * 544 + g_framebuf.height - 1) / g_framebuf.height;
}

int osd_get_text_end_x(int x, const char *str) {
    int end_x = OSD_RESCALE_X(x) + osd_get_text_width_abs(str);
    return (end_x * 960 + g_framebuf.width - 1) / g_framebuf.width;
}

void osd_draw_rounded_rectangle(int x, int y, int width, int height, int radius) {
    x = OSD_RESCALE_X(x);
    y = OSD_RESCALE_Y(y);
    width = OSD_RESCALE_X(width);
    height = OSD_RESCALE_Y(height);
    int radius_x = OSD_RESCALE_X(radius);
    int radius_y = OSD_RESCALE_Y(radius);

    if (g_color_bg.rgba.a == 0)
        return;

    if (radius_x > width / 2)
        radius_x = width / 2;
    if (radius_y > height / 2)
        radius_y = height / 2;

    for (int yy = 0; yy < height; yy++) {
        int inset = 0;

        if (yy < radius_y || yy >= height - radius_y) {
            int dy = yy < radius_y ? radius_y - yy : yy - (height - radius_y - 1);

            while (inset < radius_x) {
                int dx = radius_x - inset;
                if (dx * dx * radius_y * radius_y + dy * dy * radius_x * radius_x
                        <= radius_x * radius_x * radius_y * radius_y)
                    break;
                inset++;
            }
        }

        int row_x = x + inset;
        int row_y = y + yy;
        int count = width - inset * 2;
        int row_height = 1;
        if (!osd_clip(&row_x, &row_y, &count, &row_height))
            continue;

        rgba_t *pixels = (rgba_t *)g_framebuf.base + row_y * g_framebuf.pitch + row_x;
        if (g_color_bg.rgba.a == 255) {
            osd_fill_color(pixels, count);
        } else {
            for (int xx = 0; xx < count; xx++) {
                pixels[xx] = osd_blend_color(g_color_bg, pixels[xx]);
            }
        }
    }
}

static void osd_draw_char_abs(char character, int x, int y) {
    if (character < g_font->first_char || character > g_font->last_char)
        character = '?'; // invalid char

    const uint8_t *glyph = &g_font->data[(character - g_font->first_char) * g_font->bytes_per_glyph];
    int height = g_font->height;
    int width = g_font->width;
    int x_start = x < 0 ? -x : 0;
    int y_start = y < 0 ? -y : 0;
    int x_end = x + width > (int)g_framebuf.width ? (int)g_framebuf.width - x : width;
    int y_end = y + height > (int)g_framebuf.height ? (int)g_framebuf.height - y : height;

    if (x_start >= x_end || y_start >= y_end)
        return;

    if (g_color_text.rgba.a == 255 && g_color_bg.rgba.a == 0) {
        for (int yy = y_start; yy < y_end; yy++) {
            rgba_t *screen_rgb = (rgba_t *)g_framebuf.base + (y + yy) * g_framebuf.pitch + x + x_start;
            const uint8_t *glyph_row = glyph + yy * g_font->bytes_per_row;

            for (int byte = x_start / 8; byte <= (x_end - 1) / 8; byte++) {
                uint8_t char_byte = glyph_row[byte];
                int bit_start = byte == x_start / 8 ? x_start % 8 : 0;
                int bit_end = byte == (x_end - 1) / 8 ? (x_end - 1) % 8 + 1 : 8;

                if (char_byte == 0)
                    continue;

                for (int bit = bit_start; bit < bit_end; bit++) {
                    if (char_byte & (0x80 >> bit))
                        screen_rgb[byte * 8 + bit - x_start] = g_color_text;
                }
            }
        }
        return;
    }

    for (int yy = y_start; yy < y_end; yy++) {
        rgba_t *screen_rgb = (rgba_t *)g_framebuf.base + (y + yy) * g_framebuf.pitch + x + x_start;

        for (int xx = x_start; xx < x_end; xx++) {
            uint8_t char_byte = glyph[yy * g_font->bytes_per_row + xx / 8];

            rgba_t clr = ((char_byte >> (7 - (xx % 8))) & 1) ? g_color_text : g_color_bg;

            if (clr.rgba.a) { // alpha != 0
                if (clr.rgba.a != 0xFF) { // alpha < 255
                    screen_rgb[xx - x_start] = osd_blend_color(clr, screen_rgb[xx - x_start]); // blend FG/BG color
                } else {
                    screen_rgb[xx - x_start] = clr;
                }
            }
        }
    }
}

void osd_draw_string(int x, int y, const char *str) {
    x = OSD_RESCALE_X(x);
    y = OSD_RESCALE_Y(y);

    size_t i_cur_line = 0;

    size_t slen = strlen(str);
    for (size_t i = 0; i < slen; i++) {
        if (str[i] == '\n') {
            i_cur_line = 0;
            y += g_font->height;
            continue;
        }

        osd_draw_char_abs(str[i], x + (i_cur_line * g_font->width), y);
        i_cur_line++;
    }
}

uint32_t osd_get_text_width_small(const char *str) {
    return strlen(str) * g_fonts[FONT_COUNT - 1].width * 960.0f / g_framebuf.width;
}

uint32_t osd_get_text_height_small() {
    return g_fonts[FONT_COUNT - 1].height * 544.0f / g_framebuf.height;
}

// Like osd_draw_string, in the smallest font (the version under the logo)
void osd_draw_string_small(int x, int y, const char *str) {
    const bitmap_font_t *font = g_font;
    g_font = &g_fonts[FONT_COUNT - 1];
    osd_draw_string(x, y, str);
    g_font = font;
}

// The end of the log, bottom up: from maxy (in framebuffer pixels) to y
void osd_draw_log(int x, int y, int maxy, const char *str) {
    size_t slen = strlen(str);
    if (slen <= 3)
        return;

    x = OSD_RESCALE_X(x);
    y = OSD_RESCALE_Y(y);

    size_t line_end = slen - 1;

    for (int i = slen - 2; i >= 0; i--) {
        if (i == 0 || str[i - 1] == '\n') {
            maxy -= g_font->height;
            if (maxy - g_font->height < y)
                break;

            for (size_t i_cur = 0; i_cur < line_end - i; i_cur++) {
                osd_draw_char_abs(str[i + i_cur], x + (i_cur * g_font->width),
                                  maxy - g_font->height);
            }

            line_end = i - 1;
        }
    }
}

// The 60x38 VitaGrafix logo (an alpha mask), scaled like everything else
void osd_draw_logo(int x, int y) {
    int x0 = OSD_RESCALE_X(x);
    int y0 = OSD_RESCALE_Y(y);
    int width = OSD_RESCALE_X(LOGO_WIDTH);
    int height = OSD_RESCALE_Y(LOGO_HEIGHT);
    int x1 = x0, y1 = y0, clipped_width = width, clipped_height = height;
    if (width <= 0 || height <= 0 || !osd_clip(&x1, &y1, &clipped_width, &clipped_height))
        return;

    rgba_t logo = {.rgba = {255, 255, 255, 255}};
    for (int yy = y1; yy < y1 + clipped_height; yy++) {
        const unsigned char *row = g_logo + (yy - y0) * LOGO_HEIGHT / height * LOGO_WIDTH;
        rgba_t *pixels = (rgba_t *)g_framebuf.base + yy * g_framebuf.pitch;

        for (int xx = x1; xx < x1 + clipped_width; xx++) {
            logo.rgba.a = row[(xx - x0) * LOGO_WIDTH / width];
            if (logo.rgba.a == 0xFF) {
                pixels[xx] = logo;
            } else if (logo.rgba.a) {
                pixels[xx] = osd_blend_color(logo, pixels[xx]);
            }
        }
    }
}
