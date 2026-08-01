#include "main.h"
#include "./renderer/renderer.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    save_bg_animated_webp("output/bg.webp", WIDTH, HEIGHT);

    return 0;
}
