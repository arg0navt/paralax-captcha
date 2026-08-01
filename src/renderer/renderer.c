#include "renderer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <webp/encode.h>

/* ── Static buffer that holds the prepared background pixels ───────── */
static uint8_t *bg_buffer = NULL;
static int bg_w = 0;
static int bg_h = 0;

/* ── prepare_bg ────────────────────────────────────────────────────── *
 *  Fills a RGBA pixel buffer with a grid of 2×2 px squares separated
 *  by 3 px gaps.  The buffer can later be fed straight into libwebp.
 *
 *  Layout of one cell (5×5 px):
 *
 *      XX...   (X = square pixel, . = background)
 *      XX...
 *      .....
 *      .....
 *      .....
 *
 *  Call destroy_bg() when the buffer is no longer needed.           */
void prepare_bg(int width, int height) {
    if (width <= 0 || height <= 0)
        return;

    bg_w = width;
    bg_h = height;

    /* RGBA: 4 bytes per pixel */
    size_t size = (size_t)width * height * 4;
    bg_buffer = (uint8_t *)calloc(1, size);
    if (!bg_buffer)
        return;

    /* First, fill the entire buffer with the background colour */
    for (size_t i = 0; i < size; i += 4) {
        bg_buffer[i + 0] = BG_COLOR_R;
        bg_buffer[i + 1] = BG_COLOR_G;
        bg_buffer[i + 2] = BG_COLOR_B;
        bg_buffer[i + 3] = BG_COLOR_A;
    }

    /* Then stamp the 2×2 squares on top */
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            /* Check whether (x, y) falls inside a square */
            if ((x % CELL) < SQUARE_SIZE && (y % CELL) < SQUARE_SIZE) {
                size_t off = ((size_t)y * width + x) * 4;
                bg_buffer[off + 0] = SQ_COLOR_R;
                bg_buffer[off + 1] = SQ_COLOR_G;
                bg_buffer[off + 2] = SQ_COLOR_B;
                bg_buffer[off + 3] = SQ_COLOR_A;
            }
        }
    }
}

/* ── Accessors (used by the rest of the renderer / libwebp layer) ──── */
const uint8_t *get_bg_buffer(void) { return bg_buffer; }
int             get_bg_width(void)  { return bg_w; }
int             get_bg_height(void) { return bg_h; }

/* ── Cleanup ──────────────────────────────────────────────────────── */
void destroy_bg(void) {
    free(bg_buffer);
    bg_buffer = NULL;
    bg_w = 0;
    bg_h = 0;
}

/* ── save_bg_webp ────────────────────────────────────────────────── *
 *  Encodes the current bg_buffer as a lossless WebP image and writes
 *  it to the given file path.  Returns 0 on success, -1 on failure. */
int save_bg_webp(const char *filepath) {
    if (!bg_buffer || bg_w <= 0 || bg_h <= 0 || !filepath)
        return -1;

    uint8_t *out_data = NULL;
    size_t out_size = WebPEncodeLosslessRGBA(
        bg_buffer, bg_w, bg_h, bg_w * 4, &out_data);

    if (out_size == 0 || !out_data) {
        fprintf(stderr, "save_bg_webp: WebPEncodeLosslessRGBA failed\n");
        return -1;
    }

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        WebPFree(out_data);
        fprintf(stderr, "save_bg_webp: cannot open '%s'\n", filepath);
        return -1;
    }

    fwrite(out_data, 1, out_size, fp);
    fclose(fp);
    WebPFree(out_data);

    printf("saved %s (%zu bytes, %dx%d)\n", filepath, out_size, bg_w, bg_h);
    return 0;
}