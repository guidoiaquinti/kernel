/* jobdemo: the libk job API, TempleOS style. Renders the Mandelbrot set
 * (integer fixed point: user programs build without FP) once as a single job
 * on core 0, then split into one job per online core, and reports both times
 * and the speedup. Job j renders every n-th row from row j, so the cheap and
 * the expensive rows share out evenly. Draws to the framebuffer when there is
 * one; either way each job returns the sum of its iteration counts, and the
 * two renderings must agree. Printing the result scrolls the screen terminal,
 * which redraws its text over the picture, so the picture is put back
 * afterwards and stays until Enter.
 *
 *   jobdemo [iterations]   iterations per pixel, 16 to 4096 (default 1024)
 */
#include "libk/koraos.h"

#define FRAC 28                   // fixed point: value * 2^28
#define FIX(x) ((long)((x) * (1L << FRAC)))
#define NOFB_WIDTH 640            // the picture's size without a framebuffer
#define NOFB_HEIGHT 480

struct picture {
    unsigned width;
    unsigned height;
    unsigned max_iter;
    long step;                    // the width of a pixel, fixed point
    long left;                    // the real part at x = 0
    long top;                     // the imaginary part at y = 0
    volatile unsigned *pixels;    // NULL: compute only
    unsigned stride;              // pixels per framebuffer row
};

struct band {
    const struct picture *pic;
    unsigned first;               // the first row
    unsigned every;               // then every n-th row
};

static unsigned color(unsigned i, unsigned max_iter) {
    if (i >= max_iter) {
        return 0;  // inside the set
    }
    unsigned r = (i * 9) & 255;
    unsigned g = (i * 4) & 255;
    unsigned b = (96 + i * 13) & 255;
    return (r << 16) | (g << 8) | b;
}

// Iterations until z = z^2 + c escapes |z| > 2, at most max_iter.
static unsigned escape(long cx, long cy, unsigned max_iter) {
    long zx = 0;
    long zy = 0;
    unsigned i = 0;
    for (; i < max_iter; i++) {
        long zx2 = (zx * zx) >> FRAC;
        long zy2 = (zy * zy) >> FRAC;
        if (zx2 + zy2 > FIX(4)) {
            break;
        }
        zy = ((zx * zy) >> (FRAC - 1)) + cy;
        zx = zx2 - zy2 + cx;
    }
    return i;
}

// One job: render its rows and return the iterations it spent.
static long render(void *arg) {
    const struct band *band = arg;
    const struct picture *pic = band->pic;
    long total = 0;
    for (unsigned y = band->first; y < pic->height; y += band->every) {
        long cy = pic->top + (long)y * pic->step;
        for (unsigned x = 0; x < pic->width; x++) {
            unsigned i = escape(pic->left + (long)x * pic->step, cy, pic->max_iter);
            total += i;
            if (pic->pixels != NULL) {
                pic->pixels[y * pic->stride + x] = color(i, pic->max_iter);
            }
        }
    }
    return total;
}

// Render the picture as `jobs` jobs on cores 0, 1, ...; returns the total
// iterations, or -1 if a job could not be queued. *ms gets the time taken.
static long run(const struct picture *pic, unsigned jobs, unsigned cpus, unsigned long *ms) {
    static struct band bands[16];
    job_t *queued[16];
    unsigned long start = uptime_us();
    for (unsigned j = 0; j < jobs; j++) {
        bands[j] = (struct band){pic, j, jobs};
        queued[j] = job_queue(render, &bands[j], (int)(j % cpus));
        if (queued[j] == NULL) {
            for (unsigned k = 0; k < j; k++) {
                job_result(queued[k]);
            }
            return -1;
        }
    }
    long total = 0;
    for (unsigned j = 0; j < jobs; j++) {
        total += job_result(queued[j]);
    }
    *ms = (uptime_us() - start + 500) / 1000;
    return total;
}

// Result lines go out in one write(), like every multi-core program's should.
static char line[160];
static unsigned line_len;

static void add(const char *s) {
    while (*s && line_len < sizeof(line) - 1) {
        line[line_len++] = *s++;
    }
}

static void add_num(unsigned long v) {
    char digits[24];
    unsigned n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    while (n > 0 && line_len < sizeof(line) - 1) {
        line[line_len++] = digits[--n];
    }
}

int main(int argc, char **argv) {
    unsigned max_iter = 1024;
    if (argc > 1) {
        max_iter = 0;
        for (const char *p = argv[1]; *p; p++) {
            max_iter = *p >= '0' && *p <= '9' && max_iter < 100000 ? max_iter * 10 + (unsigned)(*p - '0')
                                                                   : 100000;
        }
    }
    if (argc > 2 || max_iter < 16 || max_iter > 4096) {
        kputs("usage: jobdemo [iterations]   (16 to 4096, default 1024)\n");
        return 2;
    }
    struct sysinfo info;
    unsigned cpus = sysinfo(&info) == 0 && info.cpus > 0 ? info.cpus : 1;

    struct picture pic = {.width = NOFB_WIDTH, .height = NOFB_HEIGHT, .max_iter = max_iter};
    struct fb_info fb;
    if (fb_info(&fb) == 0 && fb.bpp == 32) {
        pic.width = fb.width;
        pic.height = fb.height;
        pic.pixels = (volatile unsigned *)fb.addr;
        pic.stride = fb.pitch / 4;
    }
    // Real parts -2.5 to 1.0 across the width; centred vertically.
    pic.step = FIX(3.5) / (long)pic.width;
    pic.left = FIX(-2.5);
    pic.top = -(long)pic.height / 2 * pic.step;

    unsigned long one_ms = 0;
    unsigned long all_ms = 0;
    long one = run(&pic, 1, cpus, &one_ms);
    long all = run(&pic, cpus, cpus, &all_ms);
    if (one < 0 || all < 0 || one != all) {
        kputs(one < 0 || all < 0 ? "jobdemo: FAILED to queue the jobs\n"
                                 : "jobdemo: FAILED, the renderings differ\n");
        return 1;
    }
    unsigned long speedup = all_ms != 0 ? one_ms * 100 / all_ms : 100;
    add("jobdemo: ");
    add_num(pic.width);
    add("x");
    add_num(pic.height);
    add(", ");
    add_num(max_iter);
    add(" iterations: 1 core ");
    add_num(one_ms);
    add(" ms, ");
    add_num(cpus);
    add(cpus == 1 ? " core " : " cores ");
    add_num(all_ms);
    add(" ms, speedup ");
    add_num(speedup / 100);
    add(".");
    add_num(speedup / 10 % 10);
    add_num(speedup % 10);
    add("x");
    // Keep the picture on screen: save it, print, put it back.
    unsigned *saved = NULL;
    size_t words = (size_t)pic.height * pic.stride;
    if (pic.pixels != NULL && (saved = malloc(words * sizeof(unsigned))) != NULL) {
        for (size_t i = 0; i < words; i++) {
            saved[i] = pic.pixels[i];
        }
        add(", press Enter");
    }
    add("\n");
    write(1, line, line_len);
    if (saved != NULL) {
        for (size_t i = 0; i < words; i++) {
            pic.pixels[i] = saved[i];
        }
        free(saved);
        char key[16];
        read(0, key, sizeof(key));
    }
    return 0;
}
