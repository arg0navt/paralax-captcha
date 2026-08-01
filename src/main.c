#include "main.h"
#include "./renderer/renderer.h"
#include "./encoder/encoder.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    save_bg_animated_webp("output/bg.webp", WIDTH, HEIGHT);
    save_bg_animated_mp4("output/bg.mp4", WIDTH, HEIGHT);

    return 0;
}
