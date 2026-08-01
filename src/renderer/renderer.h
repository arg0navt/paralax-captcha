#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>

/* ── Grid constants ──────────────────────────────────────────────── */
#define SQUARE_SIZE 2        /* 2×2 px square                          */
#define GAP          3        /* 3 px gap between squares               */
#define CELL         (SQUARE_SIZE + GAP) /* one grid cell = 5 px        */

/* RGBA background color */
#define BG_COLOR_R 0x18
#define BG_COLOR_G 0x18
#define BG_COLOR_B 0x22
#define BG_COLOR_A 0xFF

/* RGBA square color */
#define SQ_COLOR_R 0x44
#define SQ_COLOR_G 0x44
#define SQ_COLOR_B 0x5A
#define SQ_COLOR_A 0xFF

/* ── Public API ───────────────────────────────────────────────────── */
void  prepare_bg(int width, int height);
const uint8_t *get_bg_buffer(void);
int   get_bg_width(void);
int   get_bg_height(void);
void  destroy_bg(void);
int   save_bg_webp(const char *filepath);

#endif /* RENDERER_H */
