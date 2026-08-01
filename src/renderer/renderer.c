#include "renderer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <webp/encode.h>
#include <webp/mux.h>

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

    /* Then stamp the 4×4 squares on top (legacy grid: 6px gap → cell=10) */
    const int cell = SQUARE_SIZE + 6;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if ((x % cell) < SQUARE_SIZE && (y % cell) < SQUARE_SIZE) {
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

/* ── Xorshift32 PRNG ──────────────────────────────────────────────── */
static uint32_t rng_state;

static void rng_seed(uint32_t seed) {
    rng_state = seed ? seed : 1; /* avoid zero state */
}

static uint32_t rng_next(void) {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* Returns random int in [min, max] inclusive */
static int rng_range(int min, int max) {
    return min + (int)(rng_next() % (uint32_t)(max - min + 1));
}

/* ── generate_layout ────────────────────────────────────────────── *
 *  Places 2×2 squares on a fixed 6px grid with per-square random
 *  jitter (0-1 px) and staggered odd rows (+3px).  This gives
 *  effective gaps of 3-5px in both axes while breaking up any
 *  visible row/column patterns.  One pixel per square is randomly
 *  made transparent.                                             */
Square *generate_layout(int width, int height, uint32_t seed, int *out_count) {
    rng_seed(seed);

    /* Base cell: 4 (square) + 6 = 10px → jitter 0..4 → gap = 4..10px */
    const int cell = SQUARE_SIZE + 6;

    /* Upper bound: ceil(w/cell) * ceil(h/cell) */
    int cols = (width  + cell - 1) / cell;
    int rows = (height + cell - 1) / cell;
    int capacity = cols * rows + 64;
    Square *sq = (Square *)malloc(sizeof(Square) * (size_t)capacity);
    if (!sq) { *out_count = 0; return NULL; }

    int count = 0;
    for (int gy = 0; gy * cell < height; gy++) {
        /* Stagger every other row by half a cell to break horizontal lines */
        int row_shift = (gy & 1) ? (cell / 2) : 0;
        for (int gx = 0; gx * cell < width; gx++) {
            int jx = rng_range(0, 4);
            int jy = rng_range(0, 4);
            int sx = gx * cell + row_shift + jx;
            int sy = gy * cell + jy;
            /* Clamp so the square stays inside the canvas */
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            if (sx + SQUARE_SIZE > width)  sx = width  - SQUARE_SIZE;
            if (sy + SQUARE_SIZE > height) sy = height - SQUARE_SIZE;
            if (count < capacity) {
                sq[count].x = (int16_t)sx;
                sq[count].y = (int16_t)sy;
                sq[count].transparent_idx = (uint8_t)(rng_next() % 16);
                count++;
            }
        }
    }

    *out_count = count;
    return sq;
}

/* ── render_frame ──────────────────────────────────────────────── *
 *  Renders the layout into buf, shifting all squares down by offset_y
 *  with wrap-around at canvas height.  If text_mask is not NULL,
 *  bg squares overlapping the text are skipped.                    */
void render_frame(uint8_t *buf, int width, int height,
                  const Square *layout, int num_squares, int offset_y,
                  const uint8_t *text_mask) {
    size_t size = (size_t)width * height * 4;

    /* Fill background */
    for (size_t i = 0; i < size; i += 4) {
        buf[i + 0] = BG_COLOR_R;
        buf[i + 1] = BG_COLOR_G;
        buf[i + 2] = BG_COLOR_B;
        buf[i + 3] = BG_COLOR_A;
    }

    /* Draw each square, shifted */
    for (int i = 0; i < num_squares; i++) {
        int sy = (layout[i].y + offset_y) % height;
        int sx = layout[i].x;
        int ti = layout[i].transparent_idx;

        /* Skip if this square overlaps the text mask */
        if (text_mask) {
            int overlaps = 0;
            for (int dy = 0; dy < SQUARE_SIZE && !overlaps; dy++)
                for (int dx = 0; dx < SQUARE_SIZE && !overlaps; dx++)
                    if (text_mask[sy * width + sx])
                        overlaps = 1;
            if (overlaps) continue;
        }

        for (int dy = 0; dy < SQUARE_SIZE; dy++) {
            int py = sy + dy;
            if (py >= height) continue; /* safety for edge cases */
            for (int dx = 0; dx < SQUARE_SIZE; dx++) {
                int px = sx + dx;
                if (px >= width) continue;

                size_t off = ((size_t)py * width + px) * 4;
                if (dy * SQUARE_SIZE + dx == ti) {
                    /* One pixel per square is black instead of transparent */
                    buf[off + 0] = 0x00;
                    buf[off + 1] = 0x00;
                    buf[off + 2] = 0x00;
                    buf[off + 3] = 0xFF;
                } else {
                    buf[off + 0] = SQ_COLOR_R;
                    buf[off + 1] = SQ_COLOR_G;
                    buf[off + 2] = SQ_COLOR_B;
                    buf[off + 3] = SQ_COLOR_A;
                }
            }
        }
    }
}

/* ── generate_text_mask ──────────────────────────────────────────── *
 *  Uses Windows GDI to render "Привет" in bold 30px and returns
 *  a binary mask (1 = text pixel, 0 = empty). Caller frees with free(). */
uint8_t *generate_text_mask(int width, int height) {
    uint8_t *mask = (uint8_t *)calloc(1, (size_t)width * height);
    if (!mask) return NULL;

    HDC hdc = GetDC(NULL);
    if (!hdc) { free(mask); return NULL; }

    HDC mem_dc = CreateCompatibleDC(hdc);
    if (!mem_dc) { ReleaseDC(NULL, hdc); free(mask); return NULL; }

    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = width;
    bmi.bmiHeader.biHeight      = -height; /* top-down */
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) { DeleteDC(mem_dc); ReleaseDC(NULL, hdc); free(mask); return NULL; }

    HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, bmp);

    /* Clear to black */
    RECT rc = {0, 0, width, height};
    FillRect(mem_dc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));

    /* Create bold Arial */
    HFONT font = CreateFontW(-ANIM_FONT_SIZE, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Arial");
    if (font) {
        HFONT old_font = (HFONT)SelectObject(mem_dc, font);

        const wchar_t *text = L"HELLO";
        int text_len = 5;

        /* Measure total width with ANIM_LETTER_GAP between letters */
        int total_w = 0;
        SIZE char_sz;
        for (int c = 0; c < text_len; c++) {
            GetTextExtentPoint32W(mem_dc, &text[c], 1, &char_sz);
            total_w += char_sz.cx;
            if (c < text_len - 1) total_w += ANIM_LETTER_GAP;
        }

        /* Draw each letter individually with gaps */
        SetTextColor(mem_dc, RGB(255, 255, 255));
        SetBkMode(mem_dc, TRANSPARENT);
        int x_pos = (width - total_w) / 2;
        int y_pos = (height - char_sz.cy) / 2;
        for (int c = 0; c < text_len; c++) {
            GetTextExtentPoint32W(mem_dc, &text[c], 1, &char_sz);
            TextOutW(mem_dc, x_pos, y_pos, &text[c], 1);
            x_pos += char_sz.cx + ANIM_LETTER_GAP;
        }

        SelectObject(mem_dc, old_font);
        DeleteObject(font);
    }

    /* Convert to binary mask: any non-black pixel → 1 */
    uint32_t *pixels = (uint32_t *)bits;
    for (int i = 0; i < width * height; i++) {
        if (pixels[i] & 0x00FFFFFF)
            mask[i] = 1;
    }

    SelectObject(mem_dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem_dc);
    ReleaseDC(NULL, hdc);
    return mask;
}

/* ── dilate_mask ────────────────────────────────────────────────── *
 *  Expands the mask by `radius` pixels: any pixel within `radius`
 *  of a set pixel also becomes set.  Done in-place.               */
void dilate_mask(uint8_t *mask, int width, int height, int radius) {
    uint8_t *src = (uint8_t *)malloc((size_t)width * height);
    if (!src) return;
    memcpy(src, mask, (size_t)width * height);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            if (src[y * width + x]) continue; /* already set */
            int found = 0;
            for (int dy = -radius; dy <= radius && !found; dy++) {
                for (int dx = -radius; dx <= radius && !found; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                        if (src[ny * width + nx]) found = 1;
                    }
                }
            }
            if (found) mask[y * width + x] = 1;
        }
    }
    free(src);
}

/* ── mask_to_squares ────────────────────────────────────────────── *
 *  Converts a binary text mask into an array of 2×2 Squares using
 *  the same grid + jitter + stagger as the background layout.
 *  One pixel per square is randomly made transparent.               */
Square *mask_to_squares(const uint8_t *mask, int width, int height,
                               uint32_t seed, int *out_count) {
    rng_seed(seed);

    /* Same cell as background: 2 + 3 = 5px, jitter 0..2 → gap 2..5px */
    /* Same cell as background: 4 + 6 = 10px, jitter 0..4 → gap 4..10px */
    const int cell = SQUARE_SIZE + 6;
    int capacity = (width / SQUARE_SIZE) * (height / SQUARE_SIZE) + 64;
    Square *sq = (Square *)malloc(sizeof(Square) * (size_t)capacity);
    if (!sq) { *out_count = 0; return NULL; }

    int count = 0;
    for (int gy = 0; gy * cell < height; gy++) {
        int row_shift = (gy & 1) ? (cell / 2) : 0;
        for (int gx = 0; gx * cell < width; gx++) {
            int jx = rng_range(0, 4);
            int jy = rng_range(0, 4);
            int sx = gx * cell + row_shift + jx;
            int sy = gy * cell + jy;
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
            if (sx + SQUARE_SIZE > width)  sx = width  - SQUARE_SIZE;
            if (sy + SQUARE_SIZE > height) sy = height - SQUARE_SIZE;

            /* Check if any pixel of this square falls on text */
            int has_text = 0;
            for (int dy = 0; dy < SQUARE_SIZE && !has_text; dy++)
                for (int dx = 0; dx < SQUARE_SIZE && !has_text; dx++)
                    if (mask[(sy + dy) * width + (sx + dx)])
                        has_text = 1;

            if (has_text && count < capacity) {
                sq[count].x = (int16_t)sx;
                sq[count].y = (int16_t)sy;
                sq[count].transparent_idx = (uint8_t)(rng_next() % 16);
                count++;
            }
        }
    }
    *out_count = count;
    return sq;
}

/* ── render_text_squares ───────────────────────────────────────── *
 *  Draws stationary text squares on top of the frame buffer.
 *  Uses a brighter colour so text stands out against moving bg.     */
void render_text_squares(uint8_t *buf, int width, int height,
                                 const Square *text_sq, int num_text_sq) {
    for (int i = 0; i < num_text_sq; i++) {
        int sx = text_sq[i].x;
        int sy = text_sq[i].y;
        int ti = text_sq[i].transparent_idx;
        for (int dy = 0; dy < SQUARE_SIZE; dy++) {
            int py = sy + dy;
            if (py >= height) continue;
            for (int dx = 0; dx < SQUARE_SIZE; dx++) {
                int px = sx + dx;
                if (px >= width) continue;
                size_t off = ((size_t)py * width + px) * 4;
                if (dy * SQUARE_SIZE + dx == ti) {
                    buf[off + 0] = 0x00;
                    buf[off + 1] = 0x00;
                    buf[off + 2] = 0x00;
                    buf[off + 3] = 0xFF;
                } else {
                    buf[off + 0] = SQ_COLOR_R;
                    buf[off + 1] = SQ_COLOR_G;
                    buf[off + 2] = SQ_COLOR_B;
                    buf[off + 3] = SQ_COLOR_A;
                }
            }
        }
    }
}

/* ── save_bg_animated_webp ───────────────────────────────────────── *
 *  Creates an animated WebP with chaotic squares scrolling down
 *  cyclically.  The total shift per loop = ANIM_SCROLL_MULT × height,
 *  guaranteeing a seamless loop.                                    */
int save_bg_animated_webp(const char *filepath, int width, int height) {
    if (!filepath || width <= 0 || height <= 0)
        return -1;

    /* Generate the chaotic layout once */
    int num_squares = 0;
    Square *layout = generate_layout(width, height, ANIM_SEED, &num_squares);
    if (!layout) {
        fprintf(stderr, "save_bg_animated_webp: generate_layout failed\n");
        return -1;
    }
    printf("layout: %d squares\n", num_squares);

    /* Generate text mask via GDI, dilate it, and convert to stationary squares */
    int num_text_sq = 0;
    Square *text_squares = NULL;
    uint8_t *text_mask = generate_text_mask(width, height);
    if (text_mask) {
        /* Dilate mask by 6px to create clear zone around text */
        dilate_mask(text_mask, width, height, 6);

        text_squares = mask_to_squares(text_mask, width, height,
                                       ANIM_TEXT_SEED, &num_text_sq);
        printf("text:   %d squares\n", num_text_sq);
    } else {
        fprintf(stderr, "warning: text mask generation failed, skipping text\n");
    }

    WebPMux *mux = WebPMuxNew();
    if (!mux) {
        fprintf(stderr, "save_bg_animated_webp: WebPMuxNew failed\n");
        free(layout);
        free(text_squares);
        free(text_mask);
        return -1;
    }

    if (WebPMuxSetCanvasSize(mux, width, height) != WEBP_MUX_OK) {
        fprintf(stderr, "save_bg_animated_webp: WebPMuxSetCanvasSize failed\n");
        WebPMuxDelete(mux);
        free(layout);
        return -1;
    }

    WebPMuxAnimParams anim_params;
    anim_params.loop_count = 0; /* infinite loop */
    anim_params.bgcolor = ((uint32_t)BG_COLOR_A << 24) |
                          ((uint32_t)BG_COLOR_R << 16) |
                          ((uint32_t)BG_COLOR_G << 8)  |
                          (uint32_t)BG_COLOR_B;

    if (WebPMuxSetAnimationParams(mux, &anim_params) != WEBP_MUX_OK) {
        fprintf(stderr, "save_bg_animated_webp: WebPMuxSetAnimationParams failed\n");
        WebPMuxDelete(mux);
        free(layout);
        return -1;
    }

    int frame_duration_ms = 17; /* ~60 fps */
    size_t buf_size = (size_t)width * height * 4;
    uint8_t *frame_buf = (uint8_t *)calloc(1, buf_size);
    if (!frame_buf) {
        fprintf(stderr, "save_bg_animated_webp: calloc failed\n");
        WebPMuxDelete(mux);
        free(layout);
        return -1;
    }

    int total_shift = ANIM_SCROLL_MULT * height;

    for (int i = 0; i < ANIM_TOTAL_FRAMES; i++) {
        int offset_y = (i * total_shift) / ANIM_TOTAL_FRAMES;

        render_frame(frame_buf, width, height, layout, num_squares, offset_y,
                    text_mask);

        /* Draw stationary text squares on top */
        if (text_squares)
            render_text_squares(frame_buf, width, height,
                                text_squares, num_text_sq);

        uint8_t *webp_data = NULL;
        size_t webp_size = WebPEncodeLosslessRGBA(
            frame_buf, width, height, width * 4, &webp_data);
        if (webp_size == 0 || !webp_data) {
            fprintf(stderr, "save_bg_animated_webp: WebPEncodeLosslessRGBA failed (frame %d)\n", i);
            free(frame_buf);
            WebPMuxDelete(mux);
            free(layout);
            return -1;
        }

        WebPMuxFrameInfo frame;
        frame.bitstream.bytes = webp_data;
        frame.bitstream.size  = webp_size;
        frame.x_offset   = 0;
        frame.y_offset   = 0;
        frame.duration   = frame_duration_ms;
        frame.id         = WEBP_CHUNK_ANMF;
        frame.dispose_method = WEBP_MUX_DISPOSE_NONE;
        frame.blend_method   = WEBP_MUX_BLEND;
        memset(frame.pad, 0, sizeof(frame.pad));

        if (WebPMuxPushFrame(mux, &frame, 1) != WEBP_MUX_OK) {
            fprintf(stderr, "save_bg_animated_webp: WebPMuxPushFrame failed (frame %d)\n", i);
            WebPFree(webp_data);
            free(frame_buf);
            WebPMuxDelete(mux);
            free(layout);
            return -1;
        }

        WebPFree(webp_data);

        if (i % 60 == 0 || i == ANIM_TOTAL_FRAMES - 1) {
            printf("  encoded frame %d / %d\n", i, ANIM_TOTAL_FRAMES - 1);
        }
    }

    free(frame_buf);
    free(layout);
    free(text_squares);
    free(text_mask);

    WebPData output;
    WebPDataInit(&output);
    if (WebPMuxAssemble(mux, &output) != WEBP_MUX_OK) {
        fprintf(stderr, "save_bg_animated_webp: WebPMuxAssemble failed\n");
        WebPMuxDelete(mux);
        return -1;
    }
    WebPMuxDelete(mux);

    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        WebPDataClear(&output);
        fprintf(stderr, "save_bg_animated_webp: cannot open '%s'\n", filepath);
        return -1;
    }
    fwrite(output.bytes, 1, output.size, fp);
    fclose(fp);

    printf("saved %s (%zu bytes, %dx%d, %d frames, %d s loop)\n",
           filepath, output.size, width, height, ANIM_TOTAL_FRAMES, ANIM_DURATION_SEC);

    WebPDataClear(&output);
    return 0;
}