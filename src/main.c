#include "main.h"
#include "./renderer/renderer.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    prepare_bg(WIDTH, HEIGHT);
    save_bg_webp("output/bg.webp");
    destroy_bg();

    return 0;
}
