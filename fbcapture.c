/*
 * fbcapture - Framebuffer capture utility for webOS (HP TouchPad)
 *
 * Captures the visible framebuffer and saves as JPEG.
 * Handles triple-buffered framebuffer with dynamic pan offset.
 * Composites fb0 (base layer) and fb1 (overlay) for fullscreen apps.
 *
 * Usage:
 *   fbcapture [options]
 *
 * Options:
 *   -o FILE    Output file (default: /media/internal/screen.jpg)
 *   -q QUAL    JPEG quality 1-100 (default: 75)
 *   -d         Daemon mode: continuous capture
 *   -D         Daemon mode + fork to background
 *   -i MS      Interval in milliseconds for daemon mode (default: 100)
 *   -p FILE    PID file for daemon mode
 *   -1         Single capture and exit (default)
 *   -h         Show help
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <jpeglib.h>

/* Framebuffer parameters for HP TouchPad */
#define FB_WIDTH      1024
#define FB_HEIGHT     768
#define FB_BPP        4       /* Bytes per pixel (32-bit BGRA) */
#define FB_STRIDE     4096    /* Bytes per line */
#define FB_TOTAL_HEIGHT 2304  /* Total height including triple buffer */

#define FB0_DEVICE    "/dev/fb0"
#define FB1_DEVICE    "/dev/fb1"
#define PAN_SYSFS     "/sys/class/graphics/fb0/pan"

#define DEFAULT_OUTPUT   "/media/internal/screen.jpg"
#define DEFAULT_QUALITY  75
#define DEFAULT_INTERVAL 100
#define DEFAULT_PIDFILE  "/tmp/fbcapture.pid"

/* Threshold for detecting active overlay content */
#define OVERLAY_SAMPLE_PIXELS 1000
#define OVERLAY_ACTIVE_THRESHOLD 10  /* At least this many non-black pixels */

static volatile int running = 1;
static const char *pidfile_path = NULL;
static int g_fb1_fd = -1;  /* Global fb1 file descriptor for overlay state checks */
static int g_use_overlay = 1;  /* Whether to composite fb1 overlay (0=fb0 only) */

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
}

static void cleanup_pidfile(void) {
    if (pidfile_path) {
        unlink(pidfile_path);
    }
}

static int write_pidfile(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("Failed to create PID file");
        return -1;
    }
    fprintf(f, "%d\n", getpid());
    fclose(f);
    pidfile_path = path;
    atexit(cleanup_pidfile);
    return 0;
}

static int daemonize(void) {
    pid_t pid;

    /* First fork */
    pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return -1;
    }
    if (pid > 0) {
        /* Parent exits */
        _exit(0);
    }

    /* Create new session */
    if (setsid() < 0) {
        perror("setsid failed");
        return -1;
    }

    /* Second fork to prevent acquiring a controlling terminal */
    pid = fork();
    if (pid < 0) {
        perror("fork failed");
        return -1;
    }
    if (pid > 0) {
        _exit(0);
    }

    /* Change working directory */
    if (chdir("/") < 0) {
        perror("chdir failed");
    }

    /* Redirect standard file descriptors to /dev/null */
    int null_fd = open("/dev/null", O_RDWR);
    if (null_fd >= 0) {
        dup2(null_fd, STDIN_FILENO);
        dup2(null_fd, STDOUT_FILENO);
        dup2(null_fd, STDERR_FILENO);
        if (null_fd > STDERR_FILENO) {
            close(null_fd);
        }
    }

    return 0;
}

/* Read the current pan offset from sysfs */
static int get_pan_offset(const char *path, int *x, int *y) {
    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    if (fscanf(f, "%d,%d", x, y) != 2) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}

#define FB1_PAN_SYSFS "/sys/class/graphics/fb1/pan"

/* Check if fb1 overlay has meaningful content by sampling pixels */
static int overlay_has_content(const unsigned char *fb1_map) {
    int non_black_count = 0;
    int step = (FB_WIDTH * FB_HEIGHT) / OVERLAY_SAMPLE_PIXELS;

    for (int i = 0; i < FB_WIDTH * FB_HEIGHT && non_black_count < OVERLAY_ACTIVE_THRESHOLD; i += step) {
        const unsigned char *pixel = fb1_map + (i * FB_BPP);
        unsigned char b = pixel[0];
        unsigned char g = pixel[1];
        unsigned char r = pixel[2];

        if (r > 0 || g > 0 || b > 0) {
            non_black_count++;
        }
    }

    return non_black_count >= OVERLAY_ACTIVE_THRESHOLD;
}

/* Composite fb0 and fb1 to RGB buffer
 * fb1 (overlay) is drawn on top of fb0 (base) where fb1 has non-black content
 */
static void composite_to_rgb(const unsigned char *fb0, const unsigned char *fb1,
                              unsigned char *rgb, int width, int height,
                              int src_stride, int use_overlay) {
    for (int y = 0; y < height; y++) {
        const unsigned char *src0 = fb0 + (y * src_stride);
        const unsigned char *src1 = fb1 + (y * src_stride);
        unsigned char *dst = rgb + (y * width * 3);

        for (int x = 0; x < width; x++) {
            unsigned char r, g, b;

            if (use_overlay) {
                /* Check if overlay pixel has content (non-black) */
                unsigned char b1 = src1[0];
                unsigned char g1 = src1[1];
                unsigned char r1 = src1[2];
                unsigned char a1 = src1[3];

                if (a1 > 0 && (r1 > 0 || g1 > 0 || b1 > 0)) {
                    /* Use overlay pixel */
                    r = r1;
                    g = g1;
                    b = b1;
                } else {
                    /* Use base layer pixel */
                    b = src0[0];
                    g = src0[1];
                    r = src0[2];
                }
            } else {
                /* No overlay, just use fb0 */
                b = src0[0];
                g = src0[1];
                r = src0[2];
            }

            dst[0] = r;
            dst[1] = g;
            dst[2] = b;

            src0 += 4;
            src1 += 4;
            dst += 3;
        }
    }
}

/* Save RGB buffer as JPEG */
static int save_jpeg(const char *filename, unsigned char *rgb,
                     int width, int height, int quality) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    FILE *outfile;
    JSAMPROW row_pointer[1];
    int row_stride;

    /* Use temp file and rename for atomic write */
    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "%s.tmp", filename);

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    outfile = fopen(tmpfile, "wb");
    if (!outfile) {
        fprintf(stderr, "Failed to open output file: %s\n", tmpfile);
        jpeg_destroy_compress(&cinfo);
        return -1;
    }

    jpeg_stdio_dest(&cinfo, outfile);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    row_stride = width * 3;

    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = &rgb[cinfo.next_scanline * row_stride];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    fclose(outfile);
    jpeg_destroy_compress(&cinfo);

    /* Atomic rename */
    if (rename(tmpfile, filename) != 0) {
        perror("Failed to rename temp file");
        unlink(tmpfile);
        return -1;
    }

    return 0;
}

/* Capture screen by compositing fb0 and fb1 */
static int capture_screen(unsigned char *fb0_map, unsigned char *fb1_map,
                          unsigned char *rgb_buf, const char *output,
                          int quality) {
    int pan_x, pan_y;

    /* Get current pan offset for fb0 */
    if (get_pan_offset(PAN_SYSFS, &pan_x, &pan_y) != 0) {
        /* Default to first buffer if we can't read pan */
        pan_y = 0;
    }

    /* Calculate offset into fb0 */
    size_t fb0_offset = pan_y * FB_STRIDE;

    /* Ensure we don't read past buffer */
    if (pan_y + FB_HEIGHT > FB_TOTAL_HEIGHT) {
        fb0_offset = 0;
    }

    /* Get fb1 pan offset - it also uses triple buffering! */
    int fb1_pan_x, fb1_pan_y;
    size_t fb1_offset = 0;
    if (get_pan_offset(FB1_PAN_SYSFS, &fb1_pan_x, &fb1_pan_y) == 0) {
        if (fb1_pan_y + FB_HEIGHT <= FB_TOTAL_HEIGHT) {
            fb1_offset = fb1_pan_y * FB_STRIDE;
        }
    }

    /* Check if overlay should be used:
     * - g_use_overlay=0: fb0 only mode (for launcher capture)
     * - g_use_overlay=1: composite fb1 if it has content (for app capture)
     */
    int use_overlay = 0;
    if (fb1_map && g_use_overlay) {
        use_overlay = overlay_has_content(fb1_map + fb1_offset);
    }

    /* Composite to RGB using correct offsets for both buffers */
    composite_to_rgb(fb0_map + fb0_offset,
                     fb1_map ? fb1_map + fb1_offset : fb0_map + fb0_offset,
                     rgb_buf, FB_WIDTH, FB_HEIGHT, FB_STRIDE, use_overlay);

    /* Save as JPEG */
    return save_jpeg(output, rgb_buf, FB_WIDTH, FB_HEIGHT, quality);
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  -o FILE    Output file (default: %s)\n", DEFAULT_OUTPUT);
    fprintf(stderr, "  -q QUAL    JPEG quality 1-100 (default: %d)\n", DEFAULT_QUALITY);
    fprintf(stderr, "  -d         Daemon mode: continuous capture (foreground)\n");
    fprintf(stderr, "  -D         Daemon mode: fork to background\n");
    fprintf(stderr, "  -i MS      Interval in ms for daemon mode (default: %d)\n", DEFAULT_INTERVAL);
    fprintf(stderr, "  -p FILE    PID file (default: %s)\n", DEFAULT_PIDFILE);
    fprintf(stderr, "  -0         fb0 only mode (no overlay, for launcher capture)\n");
    fprintf(stderr, "  -1         Single capture and exit (default)\n");
    fprintf(stderr, "  -h         Show this help\n");
    fprintf(stderr, "\nBy default captures fb0+fb1 for fullscreen app support.\n");
    fprintf(stderr, "Use -0 to capture launcher/app switcher (disables fb1 overlay).\n");
}

static long long current_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int main(int argc, char *argv[]) {
    const char *output = DEFAULT_OUTPUT;
    const char *pidfile = DEFAULT_PIDFILE;
    int quality = DEFAULT_QUALITY;
    int daemon_mode = 0;
    int fork_to_background = 0;
    int interval_ms = DEFAULT_INTERVAL;
    int opt;

    while ((opt = getopt(argc, argv, "o:q:dDi:p:01h")) != -1) {
        switch (opt) {
            case 'o':
                output = optarg;
                break;
            case 'q':
                quality = atoi(optarg);
                if (quality < 1 || quality > 100) {
                    fprintf(stderr, "Quality must be 1-100\n");
                    return 1;
                }
                break;
            case 'd':
                daemon_mode = 1;
                break;
            case 'D':
                daemon_mode = 1;
                fork_to_background = 1;
                break;
            case 'i':
                interval_ms = atoi(optarg);
                if (interval_ms < 10) {
                    fprintf(stderr, "Interval must be at least 10ms\n");
                    return 1;
                }
                break;
            case 'p':
                pidfile = optarg;
                break;
            case '0':
                g_use_overlay = 0;  /* fb0 only mode */
                break;
            case '1':
                daemon_mode = 0;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    /* Fork to background if requested */
    if (fork_to_background) {
        fprintf(stderr, "Starting daemon in background...\n");
        if (daemonize() != 0) {
            return 1;
        }
    }

    /* Open fb0 (base layer / compositor) */
    int fb0_fd = open(FB0_DEVICE, O_RDONLY);
    if (fb0_fd < 0) {
        perror("Failed to open fb0");
        return 1;
    }

    /* Open fb1 (overlay layer) - optional, don't fail if unavailable */
    int fb1_fd = open(FB1_DEVICE, O_RDONLY);
    if (fb1_fd < 0) {
        fprintf(stderr, "Warning: Could not open fb1 (overlay), fullscreen apps may not be captured\n");
    } else {
        g_fb1_fd = fb1_fd;  /* Store for overlay state checks */
    }

    /* Memory map fb0 */
    size_t fb_size = FB_STRIDE * FB_TOTAL_HEIGHT;
    unsigned char *fb0_map = mmap(NULL, fb_size, PROT_READ, MAP_SHARED, fb0_fd, 0);
    if (fb0_map == MAP_FAILED) {
        perror("Failed to mmap fb0");
        close(fb0_fd);
        if (fb1_fd >= 0) close(fb1_fd);
        return 1;
    }

    /* Memory map fb1 if available */
    unsigned char *fb1_map = NULL;
    if (fb1_fd >= 0) {
        fb1_map = mmap(NULL, fb_size, PROT_READ, MAP_SHARED, fb1_fd, 0);
        if (fb1_map == MAP_FAILED) {
            fprintf(stderr, "Warning: Could not mmap fb1\n");
            fb1_map = NULL;
        }
    }

    /* Allocate RGB buffer */
    unsigned char *rgb_buf = malloc(FB_WIDTH * FB_HEIGHT * 3);
    if (!rgb_buf) {
        perror("Failed to allocate RGB buffer");
        munmap(fb0_map, fb_size);
        if (fb1_map) munmap(fb1_map, fb_size);
        close(fb0_fd);
        if (fb1_fd >= 0) close(fb1_fd);
        return 1;
    }

    /* Setup signal handlers for daemon mode */
    if (daemon_mode) {
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        /* Write PID file */
        if (write_pidfile(pidfile) != 0) {
            fprintf(stderr, "Warning: could not write PID file\n");
        }

        if (!fork_to_background) {
            fprintf(stderr, "Starting capture daemon (interval: %dms, quality: %d)\n",
                    interval_ms, quality);
            fprintf(stderr, "Output: %s\n", output);
            fprintf(stderr, "fb0: mapped, fb1: %s\n", fb1_map ? "mapped" : "not available");
            fprintf(stderr, "Press Ctrl+C to stop\n");
        }
    }

    int ret = 0;
    int frame_count = 0;
    long long start_time = current_time_ms();

    do {
        long long frame_start = current_time_ms();

        if (capture_screen(fb0_map, fb1_map, rgb_buf, output, quality) != 0) {
            fprintf(stderr, "Capture failed\n");
            ret = 1;
            break;
        }

        frame_count++;

        if (daemon_mode) {
            /* Calculate time to sleep */
            long long elapsed = current_time_ms() - frame_start;
            long long sleep_ms = interval_ms - elapsed;

            if (sleep_ms > 0) {
                usleep(sleep_ms * 1000);
            }

            /* Print stats every 5 seconds */
            long long total_elapsed = current_time_ms() - start_time;
            if (total_elapsed >= 5000) {
                double fps = (double)frame_count * 1000.0 / total_elapsed;
                fprintf(stderr, "FPS: %.1f, frames: %d\n", fps, frame_count);
                frame_count = 0;
                start_time = current_time_ms();
            }
        }
    } while (daemon_mode && running);

    if (daemon_mode) {
        fprintf(stderr, "\nStopping...\n");
    }

    free(rgb_buf);
    munmap(fb0_map, fb_size);
    if (fb1_map) munmap(fb1_map, fb_size);
    close(fb0_fd);
    if (fb1_fd >= 0) close(fb1_fd);

    return ret;
}
