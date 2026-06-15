#include "renderer.h"
#include <stdio.h>

static int failed;

static void expect_viewport(int drawable_w, int drawable_h,
                            int expected_x, int expected_y,
                            int expected_w, int expected_h)
{
    int x, y, w, h;
    renderer_compute_viewport(drawable_w, drawable_h, &x, &y, &w, &h);
    if (x != expected_x || y != expected_y ||
        w != expected_w || h != expected_h)
    {
        fprintf(stderr,
                "%dx%d: got %d,%d %dx%d; expected %d,%d %dx%d\n",
                drawable_w, drawable_h, x, y, w, h,
                expected_x, expected_y, expected_w, expected_h);
        failed++;
    }
}

int main(void)
{
    expect_viewport(960, 720, 0, 0, 960, 720);
    expect_viewport(1024, 512, 171, 0, 682, 512);
    expect_viewport(800, 800, 0, 100, 800, 600);
    expect_viewport(1920, 1080, 240, 0, 1440, 1080);

    printf("renderer viewport tests: pass=%d fail=%d\n",
           4 - failed, failed);
    return failed ? 1 : 0;
}
