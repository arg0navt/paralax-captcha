#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>

/* ── Square / background constants ──────────────────────────────── */
#define SQUARE_SIZE 2        /* 2×2 px square                          */

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

/* ── Animation constants ─────────────────────────────────────────── */
#define ANIM_DURATION_SEC   5      /* 5 seconds */
#define ANIM_FPS            60     /* 60 fps */
#define ANIM_TOTAL_FRAMES   (ANIM_FPS * ANIM_DURATION_SEC) /* 300 */
#define ANIM_SCROLL_MULT    3      /* full canvas scrolls per loop       */
#define ANIM_SEED           12345  /* deterministic RNG seed             */
#define ANIM_FONT_SIZE      70     /* text font size in pixels           */
#define ANIM_TEXT_SEED      54321  /* RNG seed for text transparency      */

/* ── Square layout entry ────────────────────────────────────────── */
typedef struct {
    int16_t x;                /* pixel x of top-left corner            */
    int16_t y;                /* pixel y of top-left corner            */
    uint8_t transparent_idx; /* 0-3: which pixel in 2×2 is clear      */
} Square;

/* ── Public API ───────────────────────────────────────────────────── */
void  prepare_bg(int width, int height);
const uint8_t *get_bg_buffer(void);
int   get_bg_width(void);
int   get_bg_height(void);
void  destroy_bg(void);
int   save_bg_webp(const char *filepath);

/* Generates a chaotic random layout of squares on a canvas.
 * Returns heap-allocated array and sets *out_count. Free with free(). */
Square *generate_layout(int width, int height, uint32_t seed, int *out_count);

/* Renders one frame into an existing RGBA buffer (width*height*4).
 * Layout squares are shifted down by offset_y (wraps at height).
 * If text_mask is not NULL, bg squares overlapping text are skipped. */
void render_frame(uint8_t *buf, int width, int height,
                  const Square *layout, int num_squares, int offset_y,
                  const uint8_t *text_mask);

/* Creates an animated WebP with chaotic squares scrolling down.
 * Returns 0 on success, -1 on failure. */
int save_bg_animated_webp(const char *filepath, int width, int height);

#endif /* RENDERER_H */
