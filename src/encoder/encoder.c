/*
 * encoder.c – MP4 output via openh264 (dynamic load) + barebone MP4 muxer.
 *
 * Requires: openh264.dll (v2.x) in lib/openh264/ or working directory.
 * The DLL is loaded at runtime via LoadLibrary/GetProcAddress — no .lib needed.
 *
 * File layout: ftyp | mdat | moov  (streaming-friendly: moov at end)
 */

#include "../renderer/renderer.h"
#include "encoder.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ====================================================================
 *  Minimal openh264 C API declarations
 * ==================================================================== */

/* EUsageType (from codec_def.h) */
typedef enum { CAMERA_VIDEO_REAL_TIME = 0, SCREEN_CONTENT_REAL_TIME } EUsageType;
/* RC_MODES */
typedef enum { RC_QUALITY_MODE = 0, RC_BITRATE_MODE, RC_TIMESTAMP_MODE, RC_OFF_MODE } RC_MODES;

typedef struct {
    EUsageType  iUsageType;
    int         iPicWidth;
    int         iPicHeight;
    int         iTargetBitrate;
    RC_MODES    iRCMode;
    float       fMaxFrameRate;
} SEncParamBase;

/*
 * SFrameBSInfo — matches openh264 2.x layout on x64 Windows.
 * EncodeFrame/EncodeParameterSets write into this struct.
 *
 * The real struct in openh264 has variable MAX_LAYER_NUM_OF_FRAME,
 * but we only need the first layer.
 */
typedef struct {
    int   iNalRefIdc;
    int   eFrameType;
    int   iTemporalId;
    int   iSpatialId;
    int   iFrameNumInGop;
    int   iPadding;
    /* SLayerBsInfo[0] */
    unsigned char *pBsBuf;        /* pointer to Annex-B bitstream */
    unsigned int    iNalIdx;
    unsigned int    iNalNum;
    unsigned int    uiBsBufLen;    /* total bytes in pBsBuf */
} SFrameBSInfoCompat;

typedef struct {
    int iColorFormat;
    int iPicWidth;
    int iPicHeight;
    int iStride[3];
    unsigned char *pBitmapPlane[3];
} SSourcePicture;

typedef enum {
    cmvEncSuccess           =  0,
    cmvEncInitFailed       = -1,
    cmvEncParameter        = -3,
    cmvEncodeFail         = -9,
} CMVRet;

typedef struct ISVCEncoder ISVCEncoder;

typedef struct ISVCEncoderVtbl {
    int (*Initialize)(ISVCEncoder*, const SEncParamBase*);
    int (*InitializeExt)(ISVCEncoder*, const void*); /* SEncParamExt* — we don't need it */
    int (*GetDefaultParams)(ISVCEncoder*, void*);
    int (*Uninitialize)(ISVCEncoder*);
    int (*EncodeFrame)(ISVCEncoder*, const SSourcePicture*, SFrameBSInfoCompat*);
    int (*EncodeParameterSets)(ISVCEncoder*, SFrameBSInfoCompat*);
    int (*ForceIntraFrame)(ISVCEncoder*, int);
    int (*SetOption)(ISVCEncoder*, int, void*);
    int (*GetOption)(ISVCEncoder*, int, void*);
} ISVCEncoderVtbl;

struct ISVCEncoder { ISVCEncoderVtbl *pVtbl; };

typedef int  (*pfn_CreateEncoder)(ISVCEncoder**);
typedef void (*pfn_DestroyEncoder)(ISVCEncoder*);

/* ====================================================================
 *  Growable byte buffer (for building MP4 boxes in memory)
 * ==================================================================== */

typedef struct { uint8_t *d; size_t n, c; } Buf;

static void b_init(Buf *b) { b->d = NULL; b->n = b->c = 0; }
static void b_free(Buf *b) { free(b->d); b_init(b); }
static void b_grow(Buf *b, size_t need) {
    if (b->n + need <= b->c) return;
    size_t nc = b->c ? b->c * 2 : 4096;
    while (nc < b->n + need) nc *= 2;
    b->d = realloc(b->d, nc);
    b->c = nc;
}
static void b_u8(Buf *b, uint8_t v)  { b_grow(b, 1); b->d[b->n++] = v; }
static void b_u16(Buf *b, uint16_t v) { b_grow(b, 2); b->d[b->n++] = (uint8_t)(v>>8); b->d[b->n++] = (uint8_t)v; }
static void b_u32(Buf *b, uint32_t v) { b_grow(b, 4); b->d[b->n++]=(uint8_t)(v>>24); b->d[b->n++]=(uint8_t)(v>>16); b->d[b->n++]=(uint8_t)(v>>8); b->d[b->n++]=(uint8_t)v; }
static void b_raw(Buf *b, const void *d, size_t n) { b_grow(b, n); memcpy(b->d+b->n, d, n); b->n += n; }
static void b_zero(Buf *b, size_t n) { b_grow(b, n); memset(b->d+b->n, 0, n); b->n += n; }

/* Begin a box: write placeholder size + 4-byte type, return offset of size field */
static size_t b_box(Buf *b, const char type[4]) {
    b_grow(b, 8);
    b_u32(b, 0);  /* placeholder */
    b_raw(b, type, 4);
    return b->n - 8;
}

/* End a box: patch size field at the given offset */
static void b_end(Buf *b, size_t off) {
    uint32_t sz = (uint32_t)(b->n - off);
    b->d[off]   = (uint8_t)(sz >> 24);
    b->d[off+1] = (uint8_t)(sz >> 16);
    b->d[off+2] = (uint8_t)(sz >> 8);
    b->d[off+3] = (uint8_t)(sz);
}

/* ====================================================================
 *  NAL helper: parse Annex-B bitstream into AVCC-format buffer
 *  (strip start codes, prepend 4-byte length)
 * ==================================================================== */

static uint8_t *nals_to_avcc(const uint8_t *bs, uint32_t bs_len, uint32_t *out_len) {
    Buf b; b_init(&b);
    const uint8_t *p = bs;
    uint32_t rem = bs_len;

    while (rem >= 4) {
        /* find start code */
        int skip = (p[0]==0 && p[1]==0 && p[2]==0 && p[3]==1) ? 4 :
                  (p[0]==0 && p[1]==0 && p[2]==1) ? 3 : 0;
        if (!skip) { p++; rem--; continue; }
        p += skip; rem -= skip;

        /* find end of this NAL */
        uint32_t nl = 0;
        for (uint32_t j = 0; j + 2 < rem; j++) {
            if (p[j]==0 && p[j+1]==0 && (p[j+2]==0||p[j+2]==1)) { nl = j; break; }
        }
        if (nl == 0) nl = rem;

        b_u32(&b, nl);
        b_raw(&b, p, nl);
        p += nl; rem -= nl;
    }

    *out_len = (uint32_t)b.n;
    return b.d; /* caller frees */
}

/* Extract a single NAL by type (stripped, no start code) */
static uint8_t *nal_by_type(const uint8_t *bs, uint32_t bs_len, int type,
                            uint32_t *out_len) {
    const uint8_t *p = bs;
    uint32_t rem = bs_len;
    while (rem >= 4) {
        int skip = (p[0]==0 && p[1]==0 && p[2]==0 && p[3]==1) ? 4 :
                  (p[0]==0 && p[1]==0 && p[2]==1) ? 3 : 0;
        if (!skip) { p++; rem--; continue; }
        p += skip; rem -= skip;
        uint32_t nl = 0;
        for (uint32_t j = 0; j + 2 < rem; j++) {
            if (p[j]==0 && p[j+1]==0 && (p[j+2]==0||p[j+2]==1)) { nl = j; break; }
        }
        if (nl == 0) nl = rem;
        if ((p[0] & 0x1F) == type) {
            uint8_t *r = malloc(nl);
            if (r) { memcpy(r, p, nl); *out_len = nl; }
            return r;
        }
        p += nl; rem -= nl;
    }
    return NULL;
}

/* ====================================================================
 *  Build moov box in memory
 * ==================================================================== */

static Buf build_moov(int width, int height,
                       const uint8_t *sps, uint32_t sps_len,
                       const uint8_t *pps, uint32_t pps_len,
                       int frame_count, const uint32_t *frame_sizes,
                       uint32_t mdat_data_off) {
    Buf m; b_init(&m);
    uint32_t dur_ms = frame_count * 1000 / ANIM_FPS; /* duration in timescale=1000 */

    /* moov */
    size_t moov = b_box(&m, "moov");

    /* mvhd */
    size_t mvhd = b_box(&m, "mvhd");
    b_u8(&m, 0); b_zero(&m, 3);       /* version + flags */
    b_zero(&m, 4); b_zero(&m, 4);     /* creation + modification */
    b_u32(&m, 1000);                   /* timescale */
    b_u32(&m, dur_ms);                /* duration */
    b_u32(&m, 0x00010000);            /* rate 1.0 */
    b_u16(&m, 0x0100);                /* volume 1.0 */
    b_zero(&m, 10);                   /* reserved */
    for (int i = 0; i < 9; i++)       /* identity matrix */
        b_u32(&m, (i==0||i==4||i==8) ? 0x00010000 : 0);
    b_zero(&m, 24);                   /* pre_defined */
    b_u32(&m, 2);                     /* next_track_ID */
    b_end(&m, mvhd);

    /* trak */
    size_t trak = b_box(&m, "trak");

    /* tkhd */
    size_t tkhd = b_box(&m, "tkhd");
    b_u8(&m, 0); b_raw(&m, "\0\0\0\x03", 4); /* version + flags */
    b_u32(&m, 1); b_zero(&m, 4);     /* track_ID + reserved */
    b_u32(&m, dur_ms);               /* duration */
    b_zero(&m, 8);                   /* reserved */
    b_u16(&m, 0); b_u16(&m, 0);     /* layer + alt_group */
    b_u16(&m, 0); b_u16(&m, 0);     /* volume + reserved */
    b_u32(&m, 0x00010000); b_u32(&m, 0); b_u32(&m, 0);
    b_u32(&m, 0); b_u32(&m, 0x00010000); b_u32(&m, 0);
    b_u32(&m, 0); b_u32(&m, 0); b_u32(&m, 0x40000000);
    b_u32(&m, (uint32_t)width  << 16);
    b_u32(&m, (uint32_t)height << 16);
    b_end(&m, tkhd);

    /* mdia */
    size_t mdia = b_box(&m, "mdia");

    /* mdhd */
    size_t mdhd = b_box(&m, "mdhd");
    b_u8(&m, 0); b_zero(&m, 3); b_zero(&m, 4); b_zero(&m, 4);
    b_u32(&m, 1000);
    b_u32(&m, dur_ms);
    b_u16(&m, 0x55C4); b_u16(&m, 0);
    b_end(&m, mdhd);

    /* hdlr */
    size_t hdlr = b_box(&m, "hdlr");
    b_zero(&m, 4);
    b_raw(&m, "vide", 4);
    b_zero(&m, 12);
    b_raw(&m, "\0", 1); /* name = empty */
    b_end(&m, hdlr);

    /* minf */
    size_t minf = b_box(&m, "minf");

    /* vmhd */
    size_t vmhd = b_box(&m, "vmhd");
    b_u8(&m, 0); b_raw(&m, "\0\0\0\x01", 4);
    b_u16(&m, 0); b_zero(&m, 6);
    b_end(&m, vmhd);

    /* dinf + dref */
    size_t dinf = b_box(&m, "dinf");
    size_t dref = b_box(&m, "dref");
    b_u8(&m, 0); b_raw(&m, "\0\0\0\x01", 4); /* version+flags, entry_count=1 */
    size_t url = b_box(&m, "url ");
    b_u8(&m, 0); b_raw(&m, "\0\0\x01", 4);   /* self-contained */
    b_end(&m, url);
    b_end(&m, dref);
    b_end(&m, dinf);

    /* stbl */
    size_t stbl = b_box(&m, "stbl");

    /* stsd */
    size_t stsd = b_box(&m, "stsd");
    b_u8(&m, 0); b_zero(&m, 3); b_u32(&m, 1); /* version+flags, entry_count */

    /* avc1 */
    size_t avc1 = b_box(&m, "avc1");
    b_zero(&m, 6);               /* reserved */
    b_u16(&m, 1);                /* data_ref_index */
    b_zero(&m, 2); b_u16(&m, 0); b_zero(&m, 2); /* pre_defined + reserved */
    b_zero(&m, 12); b_zero(&m, 4); /* pre_defined */
    b_u16(&m, (uint16_t)width);
    b_u16(&m, (uint16_t)height);
    b_u32(&m, 0x00480000); b_u32(&m, 0x00480000); /* 72 dpi */
    b_zero(&m, 4);               /* reserved */
    b_u16(&m, 1);                /* frame_count */
    b_zero(&m, 32);              /* compressorname */
    b_u16(&m, 0x0018);           /* depth */
    b_u16(&m, 0xFFFF);           /* pre_defined */

    /* avcC */
    size_t avcc = b_box(&m, "avcC");
    b_u8(&m, 1);                 /* configurationVersion */
    b_u8(&m, sps[1]);            /* profile_indication */
    b_u8(&m, sps[2]);            /* profile_compatibility */
    b_u8(&m, sps[3]);            /* level_indication */
    b_u8(&m, 0xFF);              /* lengthSizeMinusOne=3 | reserved */
    b_u8(&m, 0xE1);              /* numSPS=1 | reserved */
    b_u16(&m, sps_len); b_raw(&m, sps, sps_len);
    b_u8(&m, 1);                 /* numPPS=1 */
    b_u16(&m, pps_len); b_raw(&m, pps, pps_len);
    b_end(&m, avcc);

    b_end(&m, avc1);
    b_end(&m, stsd);

    /* stts */
    size_t stts = b_box(&m, "stts");
    b_u8(&m, 0); b_zero(&m, 3);
    b_u32(&m, 1);                /* entry_count */
    b_u32(&m, (uint32_t)frame_count);
    b_u32(&m, 1000 / ANIM_FPS);  /* sample_delta */
    b_end(&m, stts);

    /* stsz */
    size_t stsz = b_box(&m, "stsz");
    b_u8(&m, 0); b_zero(&m, 3);
    b_u32(&m, 0);                /* sample_size = 0 (variable) */
    b_u32(&m, (uint32_t)frame_count);
    for (int i = 0; i < frame_count; i++)
        b_u32(&m, frame_sizes[i]);
    b_end(&m, stsz);

    /* stsc */
    size_t stsc = b_box(&m, "stsc");
    b_u8(&m, 0); b_zero(&m, 3);
    b_u32(&m, 1);                /* entry_count */
    b_u32(&m, 1);                /* first_chunk */
    b_u32(&m, (uint32_t)frame_count); /* samples_per_chunk */
    b_u32(&m, 1);                /* sample_description_index */
    b_end(&m, stsc);

    /* stco */
    size_t stco = b_box(&m, "stco");
    b_u8(&m, 0); b_zero(&m, 3);
    b_u32(&m, (uint32_t)frame_count);
    uint32_t off = mdat_data_off;
    for (int i = 0; i < frame_count; i++) {
        b_u32(&m, off);
        off += frame_sizes[i];
    }
    b_end(&m, stco);

    b_end(&m, stbl);
    b_end(&m, minf);
    b_end(&m, mdia);
    b_end(&m, trak);
    b_end(&m, moov);

    return m;
}

/* ====================================================================
 *  save_bg_animated_mp4
 * ==================================================================== */

int save_bg_animated_mp4(const char *filepath, int width, int height) {
    if (!filepath || width <= 0 || height <= 0) return -1;

    /* Load openh264.dll — try multiple locations */
    static const char *dll_paths[] = {
        "openh264.dll",                                          /* cwd */
        "lib\\openh264\\openh264.dll",                          /* cwd relative */
        "..\\lib\\openh264\\openh264.dll",                      /* from build/ */
        "..\\..\\lib\\openh264\\openh264.dll",                  /* from deeper */
        NULL
    };
    HMODULE hDll = NULL;
    for (int pi = 0; dll_paths[pi] && !hDll; pi++)
        hDll = LoadLibraryA(dll_paths[pi]);

    /* Also try from exe directory */
    if (!hDll) {
        char exe_dir[MAX_PATH];
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
        char *sl = strrchr(exe_dir, '\\');
        if (sl) {
            *sl = 0;
            strcat(exe_dir, "\\..\\lib\\openh264\\openh264.dll");
            hDll = LoadLibraryA(exe_dir);
        }
    }
    if (!hDll) {
        fprintf(stderr, "save_bg_animated_mp4: cannot load openh264.dll\n");
        return -1;
    }

    pfn_CreateEncoder  createEnc  = (pfn_CreateEncoder) GetProcAddress(hDll, "WelsCreateSVCEncoder");
    pfn_DestroyEncoder destroyEnc = (pfn_DestroyEncoder) GetProcAddress(hDll, "WelsDestroySVCEncoder");
    if (!createEnc || !destroyEnc) {
        fprintf(stderr, "save_bg_animated_mp4: missing exports\n");
        FreeLibrary(hDll);
        return -1;
    }

    /* Create encoder */
    ISVCEncoder *enc = NULL;
    if (createEnc(&enc) != 0 || !enc) {
        fprintf(stderr, "save_bg_animated_mp4: WelsCreateSVCEncoder failed\n");
        FreeLibrary(hDll);
        return -1;
    }
    fprintf(stderr, "[mp4] encoder created, vtbl=%p\n", (void*)enc->pVtbl);

    /* Configure encoder using SEncParamBase (simple API) */
    SEncParamBase param;
    memset(&param, 0, sizeof(param));
    param.iUsageType     = CAMERA_VIDEO_REAL_TIME;
    param.iPicWidth      = width;
    param.iPicHeight     = height;
    param.iTargetBitrate = 2000000;
    param.iRCMode        = RC_OFF_MODE;   /* constant QP, no rate control */
    param.fMaxFrameRate  = (float)ANIM_FPS;

    fprintf(stderr, "[mp4] calling Initialize (sizeof SEncParamBase=%zu)\n", sizeof(param));
    int init_ret = enc->pVtbl->Initialize(enc, &param);
    fprintf(stderr, "[mp4] Initialize returned %d\n", init_ret);
    if (init_ret != cmvEncSuccess) {
        fprintf(stderr, "save_bg_animated_mp4: encoder init failed\n");
        destroyEnc(enc); FreeLibrary(hDll);
        return -1;
    }

    /* Generate layout + text */
    int num_squares = 0, num_text_sq = 0;
    Square *layout = generate_layout(width, height, ANIM_SEED, &num_squares);
    if (!layout) { destroyEnc(enc); FreeLibrary(hDll); return -1; }
    printf("layout: %d squares\n", num_squares);

    uint8_t *text_mask = generate_text_mask(width, height);
    if (text_mask) dilate_mask(text_mask, width, height, 6);

    Square *text_squares = NULL;
    if (text_mask)
        text_squares = mask_to_squares(text_mask, width, height, ANIM_TEXT_SEED, &num_text_sq);
    printf("text:   %d squares\n", num_text_sq);

    /* Allocate frame buffers */
    size_t yuv_size = (size_t)width * height * 3 / 2;
    uint8_t *yuv  = calloc(1, yuv_size);
    size_t rgba_size = (size_t)width * height * 4;
    uint8_t *rgba = calloc(1, rgba_size);
    if (!yuv || !rgba) {
        fprintf(stderr, "save_bg_animated_mp4: alloc failed\n");
        free(yuv); free(rgba); free(layout); free(text_squares); free(text_mask);
        destroyEnc(enc); FreeLibrary(hDll);
        return -1;
    }

    /* YUV420 plane pointers */
    uint8_t *Y = yuv;
    uint8_t *U = yuv + (size_t)width * height;
    uint8_t *V = U + (size_t)width * height / 4;

    /* Frame storage: one AVCC-formatted buffer per frame */
    uint8_t **frames    = calloc(sizeof(uint8_t*), ANIM_TOTAL_FRAMES);
    uint32_t *f_sizes   = calloc(sizeof(uint32_t), ANIM_TOTAL_FRAMES);
    if (!frames || !f_sizes) goto cleanup;

    /* SPS/PPS (extracted from EncodeParameterSets) */
    uint8_t *sps_data = NULL, *pps_data = NULL;
    uint32_t sps_len = 0, pps_len = 0;

    /* Encode parameter sets to get SPS/PPS */
    {
        SFrameBSInfoCompat param_info;
        memset(&param_info, 0, sizeof(param_info));
        CMVRet ret = enc->pVtbl->EncodeParameterSets(enc, &param_info);
        if (ret != cmvEncSuccess) {
            fprintf(stderr, "save_bg_animated_mp4: EncodeParameterSets failed (ret=%d)\n", ret);
            goto cleanup;
        }
        if (!param_info.pBsBuf || param_info.uiBsBufLen == 0) {
            fprintf(stderr, "save_bg_animated_mp4: EncodeParameterSets returned empty bitstream\n");
            goto cleanup;
        }
        sps_data = nal_by_type(param_info.pBsBuf, param_info.uiBsBufLen, 7, &sps_len);
        pps_data = nal_by_type(param_info.pBsBuf, param_info.uiBsBufLen, 8, &pps_len);
    }

    if (!sps_data || !pps_data) {
        fprintf(stderr, "save_bg_animated_mp4: failed to extract SPS/PPS\n");
        goto cleanup;
    }
    printf("SPS: %u bytes, PPS: %u bytes\n", sps_len, pps_len);

    /* Encode all frames */
    int total_shift = ANIM_SCROLL_MULT * height;
    for (int i = 0; i < ANIM_TOTAL_FRAMES; i++) {
        int offset_y = (i * total_shift) / ANIM_TOTAL_FRAMES;

        /* Render frame */
        render_frame(rgba, width, height, layout, num_squares, offset_y, text_mask);
        if (text_squares)
            render_text_squares(rgba, width, height, text_squares, num_text_sq);

        /* RGBA → YUV420 */
        for (int py = 0; py < height; py++) {
            for (int px = 0; px < width; px++) {
                size_t si = ((size_t)py * width + px) * 4;
                int r = rgba[si], g = rgba[si+1], b = rgba[si+2];
                Y[py * width + px] = (uint8_t)((66*r + 129*g + 25*b + 128) >> 8);
            }
        }
        for (int py = 0; py < height/2; py++) {
            for (int px = 0; px < width/2; px++) {
                int sx = px*2, sy = py*2;
                int r=0, g=0, b=0;
                for (int dy = 0; dy < 2; dy++) {
                    size_t si = ((size_t)(sy+dy)*width + sx) * 4;
                    for (int dx = 0; dx < 2; dx++) {
                        r += rgba[si]; g += rgba[si+1]; b += rgba[si+2];
                        si += 4;
                    }
                }
                U[py*(width/2)+px] = (uint8_t)((-38*(r/4) - 74*(g/4) + 112*(b/4) + 128) >> 8);
                V[py*(width/2)+px] = (uint8_t)((112*(r/4) - 94*(g/4) - 18*(b/4) + 128) >> 8);
            }
        }

        /* Encode */
        SSourcePicture pic;
        memset(&pic, 0, sizeof(pic));
        pic.iPicWidth = width;
        pic.iPicHeight = height;
        pic.iColorFormat = 1;
        pic.iStride[0] = width;
        pic.iStride[1] = width / 2;
        pic.iStride[2] = width / 2;
        pic.pBitmapPlane[0] = Y;
        pic.pBitmapPlane[1] = U;
        pic.pBitmapPlane[2] = V;

        SFrameBSInfoCompat frame_info;
        memset(&frame_info, 0, sizeof(frame_info));
        CMVRet ret = enc->pVtbl->EncodeFrame(enc, &pic, &frame_info);
        if (ret != cmvEncSuccess) {
            fprintf(stderr, "encode frame %d failed (ret=%d)\n", i, ret);
            continue;
        }

        /* Read encoded bitstream */
        if (!frame_info.pBsBuf || frame_info.uiBsBufLen == 0) {
            fprintf(stderr, "encode frame %d: empty bitstream\n", i);
            continue;
        }

        frames[i] = nals_to_avcc(frame_info.pBsBuf, frame_info.uiBsBufLen, &f_sizes[i]);

        if (i % 60 == 0 || i == ANIM_TOTAL_FRAMES - 1)
            printf("  encoded frame %d/%d (%u bytes)\n", i, ANIM_TOTAL_FRAMES-1, f_sizes[i]);
    }

    destroyEnc(enc);
    enc = NULL;
    FreeLibrary(hDll);
    hDll = NULL;
    free(yuv); yuv = NULL;
    free(rgba); rgba = NULL;
    free(layout); layout = NULL;
    free(text_squares); text_squares = NULL;
    free(text_mask); text_mask = NULL;

    /* Build moov box */
    uint32_t ftyp_size = 20;  /* size(4) + ftyp(4) + isom(4) + 0x200(4) + isom(4) = 20 */
    /* Actually ftyp = size(4) + "ftyp"(4) + "isom"(4) + 0x200(4) + "isom"(4) + "avc1"(4) + "mp41"(4) = 28 */
    ftyp_size = 28;
    uint32_t mdat_hdr_size = 8; /* size(4) + "mdat"(4) */
    uint32_t mdat_data_off = ftyp_size + mdat_hdr_size; /* where frame data starts */

    Buf moov = build_moov(width, height, sps_data, sps_len, pps_data, pps_len,
                          ANIM_TOTAL_FRAMES, f_sizes, mdat_data_off);

    /* Compute total mdat payload size */
    uint64_t mdat_payload = 0;
    for (int i = 0; i < ANIM_TOTAL_FRAMES; i++)
        mdat_payload += f_sizes[i];

    /* Write file: ftyp + mdat + moov */
    FILE *fp = fopen(filepath, "wb");
    if (!fp) {
        fprintf(stderr, "save_bg_animated_mp4: cannot open '%s'\n", filepath);
        b_free(&moov);
        goto cleanup;
    }

    /* ftyp */
    {
        uint8_t ftyp[28];
        memset(ftyp, 0, 28);
        *(uint32_t*)ftyp = 0x0000001C; /* 28 */
        memcpy(ftyp+4, "ftyp", 4);
        memcpy(ftyp+8, "isom", 4);
        *(uint32_t*)(ftyp+12) = 0x00000200;
        memcpy(ftyp+16, "isom", 4);
        memcpy(ftyp+20, "avc1", 4);
        memcpy(ftyp+24, "mp41", 4);
        fwrite(ftyp, 1, 28, fp);
    }

    /* mdat */
    {
        uint8_t hdr[8];
        uint64_t mdat_box_size = 8 + mdat_payload;
        hdr[0] = (uint8_t)(mdat_box_size >> 24);
        hdr[1] = (uint8_t)(mdat_box_size >> 16);
        hdr[2] = (uint8_t)(mdat_box_size >> 8);
        hdr[3] = (uint8_t)(mdat_box_size);
        memcpy(hdr+4, "mdat", 4);
        fwrite(hdr, 1, 8, fp);
    }
    for (int i = 0; i < ANIM_TOTAL_FRAMES; i++) {
        if (frames[i])
            fwrite(frames[i], 1, f_sizes[i], fp);
    }

    /* moov */
    fwrite(moov.d, 1, moov.n, fp);
    fclose(fp);

    {
        size_t total = 28 + 8 + (size_t)mdat_payload + moov.n;
        printf("saved %s (%zu bytes, %dx%d, %d frames)\n",
               filepath, total, width, height, ANIM_TOTAL_FRAMES);
    }

    b_free(&moov);
    goto success;

cleanup:
    if (enc) { destroyEnc(enc); enc = NULL; }
    if (hDll) { FreeLibrary(hDll); hDll = NULL; }
    free(yuv); free(rgba); free(layout); free(text_squares); free(text_mask);
    for (int i = 0; i < ANIM_TOTAL_FRAMES; i++) free(frames[i]);
    free(frames); free(f_sizes);
    free(sps_data); free(pps_data);
    return -1;

success:
    for (int i = 0; i < ANIM_TOTAL_FRAMES; i++) free(frames[i]);
    free(frames); free(f_sizes);
    free(sps_data); free(pps_data);
    return 0;
}
