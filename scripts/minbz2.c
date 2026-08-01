/*
 * minbz2.c – Minimal standalone bzip2 decompressor.
 * Reads a .bz2 file from stdin, writes decompressed data to stdout.
 * Compile: zig cc -target x86_64-windows-msvc minbz2.c -o minbz2.exe
 * Usage:   minbz2.exe < input.bz2 > output
 *
 * Implements enough of bzip2 to decompress the openh264.dll.bz2.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ======== Bitstream reader ======== */
static FILE *g_in;
static uint32_t g_buf = 0;
static int      g_cnt = 0;

static void refill_bits(void) {
    int c = fgetc(g_in);
    if (c == EOF) return;
    g_buf = (g_buf << 8) | (uint32_t)c;
    g_cnt++;
}

static uint32_t rb(int n) {
    while (g_cnt < n) refill_bits();
    uint32_t v = (g_buf >> (32 - n)) & ((1u << n) - 1);
    g_buf <<= n;
    g_cnt -= n;
    return v;
}

/* ======== Huffman table ======== */
#define MAX_SYMS   256
#define MAX_ALPHA  258
#define MAX_HUFF   20

typedef struct {
    int min_len, max_len;
    int limit[MAX_HUFF - 1];   /* limit[len] = max code of length len */
    int base[MAX_HUFF - 1];    /* base[len] = first symbol index for length len */
    int perm[MAX_ALPHA];       /* symbols in code order */
} HuffTab;

static int huff_decode(HuffTab *h) {
    int code = 0;
    for (int len = h->min_len; len <= h->max_len; len++) {
        code = (code << 1) | (int)rb(1);
        int ci = len - h->min_len;
        if (code <= h->limit[ci]) {
            return h->perm[h->base[ci] + code - (ci > 0 ? h->limit[ci-1] + 1 : 0)];
        }
    }
    return -1;
}

static void build_huff(HuffTab *h, const uint8_t *lens, int n) {
    h->min_len = 99; h->max_len = 0;
    int count[MAX_HUFF] = {0};
    for (int i = 0; i < n; i++) {
        if (lens[i] == 0) continue;
        if (lens[i] < h->min_len) h->min_len = lens[i];
        if (lens[i] > h->max_len) h->max_len = lens[i];
        count[lens[i]]++;
    }
    if (h->min_len > h->max_len) { h->min_len = h->max_len = 1; }

    /* Build perm: symbols sorted by (length, symbol) */
    int idx = 0;
    for (int len = h->min_len; len <= h->max_len; len++)
        for (int s = 0; s < n; s++)
            if (lens[s] == len)
                h->perm[idx++] = s;

    /* Compute limit[]: cumulative counts → canonical codes */
    /* For canonical Huffman: code for length L starts at sum(count[l]*2^(max_len-l) for l<L) */
    uint32_t next_code[MAX_HUFF] = {0};
    uint32_t code = 0;
    for (int len = h->min_len; len <= h->max_len; len++) {
        next_code[len] = code;
        code += (uint32_t)count[len];
        code <<= 1;
    }

    for (int len = h->min_len; len <= h->max_len; len++) {
        int ci = len - h->min_len;
        h->base[ci] = (ci > 0) ? h->base[ci-1] + count[len - 1] : 0;
        h->limit[ci] = (int)next_code[len] + count[len] - 1;
    }
    /* For decoding: code starts at 0, for each bit:
     * code = code*2 + bit
     * if code > limit[len-min_len] → increment len
     * symbol = perm[base[len-min_len] + code - start_of_length]
     */
    /* Actually let's use a simpler approach for decoding */
}

/* Simple Huffman decode using a lookup approach */
static int huff_decode_simple(HuffTab *h) {
    int code = 0;
    int start_code = 0; /* first code of current length */
    for (int len = h->min_len; len <= h->max_len; len++) {
        code = (code << 1) | (int)rb(1);
        int ci = len - h->min_len;
        /* At this length, codes range from start_code to start_code + count-1 */
        int count_at_len = h->base[ci + 1 < MAX_HUFF-1 ? ci+1 : ci] - h->base[ci];
        if (code < start_code + count_at_len) {
            return h->perm[h->base[ci] + code - start_code];
        }
        start_code = (start_code + count_at_len) * 2;
    }
    return -1;
}

/* ======== MTF inverse ======== */
static uint8_t mtf_inverse(int sym, uint8_t *mtf_arr, int n) {
    uint8_t ch = mtf_arr[sym];
    memmove(mtf_arr + 1, mtf_arr, sym);
    mtf_arr[0] = ch;
    return ch;
}

/* ======== BWT inverse ======== */
static void bwt_decode(const uint8_t *bwt, int n, int orig_ptr, uint8_t *out) {
    int *T = (int*)malloc(sizeof(int) * n);
    int C[256] = {0};
    for (int i = 0; i < n; i++) C[bwt[i]]++;
    int sum = 0;
    for (int i = 0; i < 256; i++) { int c = C[i]; C[i] = sum; sum += c; }
    for (int i = 0; i < n; i++) T[C[bwt[i]]++] = i;
    
    int idx = orig_ptr;
    for (int i = n - 1; i >= 0; i--) {
        out[i] = bwt[T[idx]];
        idx = T[idx];
    }
    free(T);
}

/* ======== RLE decoding (bzip2 initial) ======== */
static int rle_decode_a(const uint8_t *in, int in_len, uint8_t *out, int max_out) {
    int oi = 0;
    for (int i = 0; i < in_len && oi < max_out; ) {
        uint8_t ch = in[i++];
        out[oi++] = ch;
        if (ch == 0 && i < in_len) {
            int run = (unsigned)in[i] + 1; i++;
            if (i < in_len) run += (unsigned)in[i] * 256; i++;
            if (i < in_len) run += (unsigned)in[i] * 65536; i++;
            for (int j = 0; j < run && oi < max_out; j++)
                out[oi++] = 0;
        }
    }
    return oi;
}

/* ======== Block decoder ======== */
static uint8_t *g_block_buf = NULL;
static int      g_block_used = 0;

static void emit_byte(uint8_t b) {
    g_block_buf[g_block_used++] = b;
}

static void emit_run(uint8_t b, int n) {
    for (int i = 0; i < n; i++) emit_byte(b);
}

static int decode_block(int block_size) {
    /* Block header */
    if (rb(24) != 0x31415926) { /* "1ry" + more */ return -1; }
    rb(32); /* block CRC */
    int randomized = (int)rb(1);
    (void)randomized;
    int orig_ptr = (int)rb(24);
    
    /* Read in-use map */
    int in_use[256] = {0};
    uint16_t map16 = (uint16_t)rb(16);
    for (int i = 0; i < 16; i++)
        if (map16 & (1 << (15 - i)))
            for (int j = 0; j < 16; j++)
                in_use[i * 16 + j] = 1;
    
    int n_in_use = 0;
    uint8_t seq_to_unseq[256];
    for (int i = 0; i < 256; i++)
        if (in_use[i])
            seq_to_unseq[n_in_use++] = (uint8_t)i;
    
    if (n_in_use == 0) return 0;
    
    /* Number of Huffman tables */
    int n_groups = (int)rb(3);
    int n_selectors = (int)rb(15);
    
    /* Read selectors */
    uint8_t *selectors = (uint8_t*)malloc(n_selectors);
    {
        uint8_t mtf_sel[6];
        for (int i = 0; i < n_groups; i++) mtf_sel[i] = (uint8_t)i;
        for (int i = 0; i < n_selectors; i++) {
            int j = 0;
            for (;;) {
                if ((int)rb(1) == 0) break;
                j++;
            }
            if (j >= n_groups) { free(selectors); return -1; }
            uint8_t v = mtf_sel[j];
            memmove(mtf_sel + 1, mtf_sel, j);
            mtf_sel[0] = v;
            selectors[i] = v;
        }
    }
    
    /* Read Huffman tables */
    HuffTab *tables = (HuffTab*)calloc(n_groups, sizeof(HuffTab));
    for (int g = 0; g < n_groups; g++) {
        int curr = (int)rb(5);
        uint8_t lens[MAX_ALPHA];
        for (int i = 0; i < n_in_use; i++) {
            while ((int)rb(1)) {
                curr--;
                if (curr < 1) { free(selectors); free(tables); return -1; }
            }
            lens[i] = (uint8_t)curr;
            curr++;
        }
        /* Build table */
        tables[g].min_len = 99; tables[g].max_len = 0;
        int count[MAX_HUFF] = {0};
        for (int i = 0; i < n_in_use; i++) {
            if (lens[i] < tables[g].min_len) tables[g].min_len = lens[i];
            if (lens[i] > tables[g].max_len) tables[g].max_len = lens[i];
            count[lens[i]]++;
        }
        if (tables[g].min_len > tables[g].max_len) {
            tables[g].min_len = tables[g].max_len = 1;
        }
        /* Build perm */
        int idx = 0;
        for (int len = tables[g].min_len; len <= tables[g].max_len; len++)
            for (int s = 0; s < n_in_use; s++)
                if (lens[s] == len)
                    tables[g].perm[idx++] = s;
        
        /* Build base/limit */
        {
            uint32_t code = 0;
            int start[MAX_HUFF];
            for (int len = tables[g].min_len; len <= tables[g].max_len; len++) {
                start[len] = (int)code;
                code += (uint32_t)count[len];
                code <<= 1;
            }
            int cum = 0;
            for (int len = tables[g].min_len; len <= tables[g].max_len; len++) {
                int ci = len - tables[g].min_len;
                tables[g].base[ci] = cum;
                tables[g].limit[ci] = start[len] + count[len] - 1;
                cum += count[len];
            }
        }
    }
    
    /* Decode data using Huffman tables + MTF */
    g_block_used = 0;
    g_block_buf = (uint8_t*)malloc(block_size + 100);
    
    uint8_t mtf_arr[256];
    for (int i = 0; i < 256; i++) mtf_arr[i] = (uint8_t)i;
    
    int sel_idx = 0, sel_bits = 0;
    HuffTab *cur_tab = &tables[selectors[0]];
    
    while (g_block_used < block_size) {
        /* Check if we need new selector */
        if (sel_idx >= n_selectors) break;
        if (sel_bits == 0) {
            sel_bits = 50;
            sel_idx++;
            if (sel_idx >= n_selectors) break;
            cur_tab = &tables[selectors[sel_idx]];
        }
        sel_bits--;
        
        /* Decode next symbol */
        int sym = huff_decode_simple(cur_tab);
        if (sym < 0 || sym >= n_in_use) break;
        
        if (sym == 0) {
            /* RUNA */
            emit_run(seq_to_unseq[mtf_inverse(0, mtf_arr, n_in_use)], 1);
        } else if (sym == 1) {
            /* RUNB */
            emit_run(seq_to_unseq[mtf_inverse(1, mtf_arr, n_in_use)], 1);
            /* Actually RUNA/RUNB encode run-length:
             * sym=0 means add 1 to run, sym=1 means add 2 to run
             * When next sym is not 0 or 1, flush run.
             */
        } else if (sym > 1) {
            emit_byte(seq_to_unseq[mtf_inverse(sym, mtf_arr, n_in_use)]);
        }
    }
    
    /* ... this approach is getting the RLE wrong. Let me re-think. */
    /* In bzip2, symbols 0 (RUNA) and 1 (RUNB) are a binary representation
     * of run lengths. The run is terminated by any symbol >= 2.
     * RUNA=1, RUNB=2, RUNA RUNA=3, RUNB=4, RUNA RUNB=5, RUNB RUNA=6, etc.
     * This is basically a binary number where RUNA=bit 0, RUNB=bit 1.
     * The actual run length = binary_value + 1.
     */
    
    /* Let me redo the decode loop properly */
    g_block_used = 0;
    for (int i = 0; i < 256; i++) mtf_arr[i] = (uint8_t)i;
    
    sel_idx = 0; sel_bits = 0;
    cur_tab = &tables[selectors[0]];
    
    int run = 0; /* accumulated run for RUNA/RUNB */
    int run_char = -1; /* which character is being run */
    
    while (g_block_used < block_size) {
        if (sel_idx >= n_selectors) break;
        if (sel_bits == 0) {
            sel_bits = 50;
            sel_idx++;
            if (sel_idx >= n_selectors) break;
            cur_tab = &tables[selectors[sel_idx]];
        }
        sel_bits--;
        
        int sym = huff_decode_simple(cur_tab);
        if (sym < 0 || sym >= n_in_use) break;
        
        if (sym == 0 || sym == 1) {
            /* RUNA or RUNB: accumulate binary run */
            if (run == 0) {
                run_char = seq_to_unseq[0]; /* RUNA and RUNB both map to seq 0 after MTF */
            }
            run = run * 2 + sym + 1;
        } else {
            /* Flush any pending run */
            if (run > 0) {
                emit_run((uint8_t)run_char, run);
                run = 0;
            }
            emit_byte(seq_to_unseq[mtf_inverse(sym, mtf_arr, n_in_use)]);
        }
    }
    if (run > 0) {
        emit_run((uint8_t)run_char, run);
        run = 0;
    }
    
    /* Apply BWT inverse */
    uint8_t *decoded = (uint8_t*)malloc(block_size);
    bwt_decode(g_block_buf, g_block_used, orig_ptr, decoded);
    
    /* Apply RLE(0) decoding (zero-run-length: 0x00 followed by 2 bytes of count) */
    int out_len = 0;
    uint8_t *final_buf = (uint8_t*)malloc(block_size * 2);
    for (int i = 0; i < block_size; ) {
        if (decoded[i] == 0 && i + 2 < block_size) {
            int r = (unsigned)decoded[i+1] + (unsigned)decoded[i+2] * 256;
            i += 3;
            for (int j = 0; j < r; j++)
                final_buf[out_len++] = 0;
        } else {
            final_buf[out_len++] = decoded[i++];
        }
    }
    
    /* Write to stdout */
    fwrite(final_buf, 1, out_len, stdout);
    
    free(final_buf);
    free(decoded);
    free(g_block_buf);
    g_block_buf = NULL;
    free(selectors);
    free(tables);
    
    return out_len;
}

int main(void) {
    g_in = stdin;
    /* Set stdin to binary mode on Windows */
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    
    /* Read magic */
    char magic[3];
    if (fread(magic, 1, 3, stdin) != 3) {
        fprintf(stderr, "read error\n");
        return 1;
    }
    if (magic[0] != 'B' || magic[1] != 'Z') {
        fprintf(stderr, "not bzip2\n");
        return 1;
    }
    
    /* Read block size ('1'-'9') */
    int block_size_100k = magic[2] - '0';
    if (block_size_100k < 1 || block_size_100k > 9) {
        fprintf(stderr, "bad block size char: %c\n", magic[2]);
        return 1;
    }
    int block_size = block_size_100k * 100000;
    
    /* Process stream */
    uint64_t total = 0;
    for (;;) {
        /* Read 48-bit block magic: 0x314159265359 (pi) */
        uint64_t bm = ((uint64_t)rb(24) << 24) | rb(24);
        if (bm == 0x314159265359ULL) {
            int n = decode_block(block_size);
            if (n < 0) { fprintf(stderr, "decode error\n"); return 1; }
            total += n;
        } else if (bm == 0x177245385090ULL) {
            /* Footer magic (sqrt(pi)) — end of stream */
            rb(32); /* stream CRC */
            break;
        } else {
            fprintf(stderr, "bad block magic: 0x%012llx\n", (unsigned long long)bm);
            return 1;
        }
    }
    
    fprintf(stderr, "decompressed %llu bytes\n", (unsigned long long)total);
    return 0;
}
