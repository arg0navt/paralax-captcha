#ifndef ENCODER_H
#define ENCODER_H

/* ── MP4 output via openh264 + barebone mux ────────────────────── *
 *  Creates an H.264 MP4 with the chaotic squares scrolling down.
 *  Requires openh264.dll in the working directory or lib/openh264/.
 *  Returns 0 on success, -1 on failure.                       */

int save_bg_animated_mp4(const char *filepath, int width, int height);

#endif /* ENCODER_H */
