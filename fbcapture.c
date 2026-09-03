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
#include <stdint.h>
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


static volatile int running = 1;
static int g_timing = 0;   /* -T: print per-stage timings to stderr */
static const char *pidfile_path = NULL;

static long now_usec(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000000L + tv.tv_usec;
}

/* Checksum of the composited frame, used to skip redundant encodes.
 * FNV-1a over 32-bit words: one pass, no table, and it reads the RGB buffer
 * (normal cached memory), so it costs a few ms against a ~137ms encode. */
static unsigned int frame_checksum(const unsigned char *buf, size_t len) {
    const uint32_t *w = (const uint32_t *)buf;
    size_t words = len / 4;
    unsigned int h = 2166136261u;
    for (size_t i = 0; i < words; i++) {
        h = (h ^ w[i]) * 16777619u;
    }
    for (size_t i = words * 4; i < len; i++) {
        h = (h ^ buf[i]) * 16777619u;
    }
    return h;
}

static unsigned int g_last_sum = 0;
static int g_have_last_sum = 0;
static int g_frame_changed = 0;  /* set by capture_screen: did this frame differ? */

/* Trailing frames.
 *
 * A capture can land mid-redraw: tapping the launcher's tab strip updates the
 * tabs immediately, but the icons underneath are drawn a moment later. If we
 * sample once, see a change, send it, and then go quiet because the next
 * sample is identical, the far end is left holding that half-drawn frame.
 *
 * So after the last detected change we keep emitting a couple more frames and
 * keep polling at full rate for a settle window, rather than backing off the
 * instant nothing appears to be moving. Same reasoning as the tap ripple: the
 * frames that matter most are the ones right after motion appears to stop.
 */
#define TRAILING_FRAMES  2
#define SETTLE_HOLD_MS   750

/* Periodic keyframe.
 *
 * Change detection only sends when the composited frame differs from the last
 * one sent. If a change is ever missed - a redraw landing in a framebuffer
 * page we did not sample, say - the far end would hold a stale image
 * indefinitely, because nothing would ever mark it dirty again.
 *
 * Until 2026-09-03 a bug hid this: the 5-second stats block zeroed
 * frame_count, which is what drives the first-frame force_write, so the daemon
 * happened to re-send the current screen every 5s. That accident was the only
 * thing bounding how long a stale frame could persist. Fixing the counter
 * removed the safety net, so here it is deliberately, and cheaper: one frame
 * every 2s while idle, ~14KB/s, still far below the ~620KB/s the per-frame
 * `novacom get` transport used on a completely static screen.
 */
#define KEYFRAME_INTERVAL_MS 2000

static int g_trailing = 0;
static long long g_last_sent_ms = 0;
static int g_fb1_fd = -1;  /* Global fb1 file descriptor for overlay state checks */
static int g_use_overlay = 1;  /* Whether to composite fb1 overlay (0=fb0 only) */
static int g_stream_mode = 0;  /* -S: emit framed JPEGs on stdout instead of files */
static int g_fast_dct = 0;     /* -F: trade DCT accuracy for ~15ms/frame */

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

/* Composite fb0 and fb1 to RGB buffer.
 *
 * LAYER ORDER: fb0 is the TOP layer, fb1 is underneath it.
 *
 * This is the opposite of what the old code (and CLAUDE.md) assumed, but it is
 * what the hardware actually reports. Measured on device with a card app open:
 *
 *   fb0 ("lcdc panel")     alpha=0 on 96.4% of pixels, alpha=255 on 3.6%
 *                          - and that 3.6% is exactly the status bar strip.
 *   fb1 ("msmfb40_30001")  alpha=255 on 93%, plus ~3% partial alpha.
 *
 * A layer that is transparent over 96% of the panel cannot be the base: if it
 * were, almost the whole screen would composite to nothing. fb0 is the system
 * chrome plane (status bar / notifications), drawn OVER the app plane in fb1.
 *
 * So the correct operation is a per-pixel source-over blend of fb0 onto fb1,
 * driven by fb0's alpha - not a colour-key pick between the two.
 *
 * Why the old colour-key broke: it selected fb1 wherever fb1's RGB was
 * non-black, else fell back to fb0. That accidentally looked right for a bright
 * app (fb1 non-black -> app shows; status strip black in fb1 -> falls back to
 * fb0's chrome). But for a DARK app - a game, video, any dark UI - fb1's own
 * content failed the "non-black" test, so the frame fell back to fb0, which is
 * transparent-black over 96% of the panel. The result was a black screen with
 * only the status bar on it: the "only one layer is being captured" symptom.
 *
 * Integer math only - this target is softfp, so float blending is slow here.
 */
static void composite_to_rgb(const unsigned char *fb0, const unsigned char *fb1,
                              unsigned char *rgb, int width, int height,
                              int src_stride, int use_overlay) {
    for (int y = 0; y < height; y++) {
        /* Word-wide reads. Framebuffer memory is mapped uncached, so each byte
         * access is its own bus transaction; pulling the whole BGRA pixel in
         * one aligned 32-bit load cuts those transactions 4x. Stride is 4096
         * and pixels are 4 bytes, so both pointers stay naturally aligned. */
        const uint32_t *src0 = (const uint32_t *)(fb0 + (y * src_stride));
        const uint32_t *src1 = (const uint32_t *)(fb1 + (y * src_stride));
        unsigned char *dst = rgb + (y * width * 3);

        for (int x = 0; x < width; x++) {
            uint32_t p0 = *src0++;
            unsigned char r, g, b;

            if (use_overlay) {
                /* fb0 (chrome) over fb1 (app), keyed on fb0's alpha */
                unsigned int a0 = p0 >> 24;

                if (a0 == 0) {
                    /* Chrome fully transparent - app layer shows through */
                    uint32_t p1 = *src1;
                    b = (unsigned char)(p1);
                    g = (unsigned char)(p1 >> 8);
                    r = (unsigned char)(p1 >> 16);
                } else if (a0 == 255) {
                    /* Chrome fully opaque - take it as-is */
                    b = (unsigned char)(p0);
                    g = (unsigned char)(p0 >> 8);
                    r = (unsigned char)(p0 >> 16);
                } else {
                    /* Partial alpha - source-over blend, rounded */
                    uint32_t p1 = *src1;
                    unsigned int ia = 255u - a0;
                    b = (unsigned char)((((p0)       & 0xFFu) * a0 + ((p1)       & 0xFFu) * ia + 127u) / 255u);
                    g = (unsigned char)((((p0 >> 8)  & 0xFFu) * a0 + ((p1 >> 8)  & 0xFFu) * ia + 127u) / 255u);
                    r = (unsigned char)((((p0 >> 16) & 0xFFu) * a0 + ((p1 >> 16) & 0xFFu) * ia + 127u) / 255u);
                }
            } else {
                /* fb0 only mode (-0) */
                b = (unsigned char)(p0);
                g = (unsigned char)(p0 >> 8);
                r = (unsigned char)(p0 >> 16);
            }

            src1++;

            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            dst += 3;
        }
    }
}

/* Encode RGB to JPEG into any already-open FILE*.
 *
 * Shared by the file writer and the stream writer so the encoder settings
 * cannot drift apart between them. */
static void encode_jpeg(FILE *out, unsigned char *rgb,
                        int width, int height, int quality) {
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, out);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    /* This is a 2011 libjpeg62 with no NEON/SIMD path, and the DCT dominates
     * encode time on this Cortex-A8. JDCT_IFAST saves roughly 15ms a frame,
     * but it is an approximation and rings on hard edges - which is most of
     * what a screen capture contains, text especially. Accuracy wins by
     * default here; -F opts into the faster transform. */
    cinfo.dct_method = g_fast_dct ? JDCT_IFAST : JDCT_ISLOW;

    jpeg_start_compress(&cinfo, TRUE);

    int row_stride = width * 3;

    /* Hand libjpeg many scanlines per call instead of one - fewer calls through
     * the compressor's per-row bookkeeping. */
    while (cinfo.next_scanline < cinfo.image_height) {
        JSAMPROW rows[16];
        unsigned int n = 0;
        while (n < 16 && cinfo.next_scanline + n < cinfo.image_height) {
            rows[n] = &rgb[(cinfo.next_scanline + n) * row_stride];
            n++;
        }
        jpeg_write_scanlines(&cinfo, rows, n);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
}

/* Save RGB buffer as JPEG, written to a temp file and renamed into place. */
static int save_jpeg(const char *filename, unsigned char *rgb,
                     int width, int height, int quality) {
    char tmpfile[256];
    snprintf(tmpfile, sizeof(tmpfile), "%s.tmp", filename);

    FILE *outfile = fopen(tmpfile, "wb");
    if (!outfile) {
        fprintf(stderr, "Failed to open output file: %s\n", tmpfile);
        return -1;
    }

    encode_jpeg(outfile, rgb, width, height, quality);
    fclose(outfile);

    /* Atomic rename */
    if (rename(tmpfile, filename) != 0) {
        perror("Failed to rename temp file");
        unlink(tmpfile);
        return -1;
    }

    return 0;
}

/* ---- Stream mode ---------------------------------------------------------
 *
 * Instead of writing a JPEG to /media/internal every frame and having the host
 * pull it with a fresh `novacom get` each time (~53ms plus a process spawn per
 * frame, and a VFAT write per frame on the device), the daemon encodes to
 * memory and writes framed JPEGs to stdout. The host runs ONE
 * `novacom run ... -S` and reads frames as they arrive.
 *
 * Frame format, repeated:
 *
 *     "LCF1"                4 bytes  magic, lets a reader resync
 *     length                4 bytes  big-endian, payload size
 *     JPEG                  <length> bytes
 *
 * libjpeg 6.2 (what the device ships) has no jpeg_mem_dest, so the memory
 * destination is an fmemopen() stream - verified present and working on the
 * device's glibc 2.8.
 */
#define STREAM_MAGIC "LCF1"
#define STREAM_BUF_CAP (1024 * 1024)   /* a 1024x768 q75 frame is ~50-120KB */

static unsigned char *g_stream_buf = NULL;

static int encode_jpeg_mem(unsigned char *rgb, int width, int height,
                           int quality, size_t *out_len) {
    FILE *mem = fmemopen(g_stream_buf, STREAM_BUF_CAP, "wb");
    if (!mem) {
        perror("fmemopen");
        return -1;
    }

    encode_jpeg(mem, rgb, width, height, quality);

    long n = ftell(mem);
    fclose(mem);

    if (n <= 0) {
        return -1;
    }
    *out_len = (size_t)n;
    return 0;
}

/* Write one framed JPEG to stdout. Returns -1 if the host went away. */
static int write_stream_frame(const unsigned char *jpeg, size_t len) {
    unsigned char hdr[8];

    memcpy(hdr, STREAM_MAGIC, 4);
    hdr[4] = (unsigned char)((len >> 24) & 0xFF);
    hdr[5] = (unsigned char)((len >> 16) & 0xFF);
    hdr[6] = (unsigned char)((len >> 8) & 0xFF);
    hdr[7] = (unsigned char)(len & 0xFF);

    if (fwrite(hdr, 1, sizeof(hdr), stdout) != sizeof(hdr)) return -1;
    if (fwrite(jpeg, 1, len, stdout) != len) return -1;
    if (fflush(stdout) != 0) return -1;

    return 0;
}

/* Capture screen by compositing fb0 and fb1 */
static int capture_screen(unsigned char *fb0_map, unsigned char *fb1_map,
                          unsigned char *rgb_buf, const char *output,
                          int quality, int force_write) {
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
     * - g_use_overlay=0: fb0 only mode (-0, manual override)
     * - g_use_overlay=1: alpha-composite fb1 over fb0
     *
     * There is deliberately no content heuristic here any more. The old code
     * sampled fb1 for non-black pixels and, if too few were found, dropped the
     * whole frame back to fb0 alone. Since fb1 is the layer holding the actual
     * app/launcher content, any dark screen (a dark app, a game, video) tripped
     * that threshold and the capture collapsed to fb0 - which is transparent
     * over 96% of the panel, leaving just the status bar on black. That is the
     * "only one layer is being captured" symptom. Per-pixel alpha makes the
     * heuristic unnecessary: a genuinely empty fb1 is alpha=0 and composites
     * away on its own.
     */
    int use_overlay = (fb1_map != NULL) && g_use_overlay;

    /* Composite to RGB using correct offsets for both buffers */
    long t0 = now_usec();
    composite_to_rgb(fb0_map + fb0_offset,
                     fb1_map ? fb1_map + fb1_offset : fb0_map + fb0_offset,
                     rgb_buf, FB_WIDTH, FB_HEIGHT, FB_STRIDE, use_overlay);
    long t1 = now_usec();

    /* Skip the encode when the composited frame is byte-identical to the last
     * one we encoded. The encode is ~4x the cost of the composite, so an idle
     * screen drops from ~170ms to ~40ms per iteration - which means the loop
     * comes back around far sooner and catches the NEXT change quickly.
     *
     * This is a full-frame checksum over the composited RGB, not a sparse
     * sample of the framebuffer. That matters: a sparse sample can miss a small
     * localised change - exactly the tail frames of a tap ripple - and skipping
     * those is what leaves the far end stuck mid-animation. Every frame that
     * differs in any pixel gets encoded, so the final settled frame is always
     * transmitted.
     */
    int rc = 0;
    unsigned int sum = frame_checksum(rgb_buf, FB_WIDTH * FB_HEIGHT * 3);
    int changed = (!g_have_last_sum || sum != g_last_sum);
    g_frame_changed = changed;

    /* Top the trailing counter back up on every change, so a continuous
     * animation keeps streaming and the tail is only spent once it stops. */
    if (changed) {
        g_trailing = TRAILING_FRAMES;
    } else if (g_trailing > 0) {
        g_trailing--;
    }

    long long now_ms = (long long)(now_usec() / 1000);
    int keyframe_due = (g_last_sent_ms != 0) &&
                       ((now_ms - g_last_sent_ms) >= KEYFRAME_INTERVAL_MS);

    if (changed || force_write || g_trailing > 0 || keyframe_due) {
        if (g_stream_mode) {
            size_t len = 0;
            rc = encode_jpeg_mem(rgb_buf, FB_WIDTH, FB_HEIGHT, quality, &len);
            if (rc == 0) {
                rc = write_stream_frame(g_stream_buf, len);
                if (rc != 0) {
                    /* Host closed the pipe - stop cleanly rather than spin. */
                    running = 0;
                }
            }
        } else {
            rc = save_jpeg(output, rgb_buf, FB_WIDTH, FB_HEIGHT, quality);
        }
        if (rc == 0) {
            g_last_sum = sum;
            g_have_last_sum = 1;
            g_last_sent_ms = now_ms;
        }
    }
    long t2 = now_usec();

    if (g_timing) {
        fprintf(stderr, "[timing] composite=%ld.%01ld ms  encode+write=%ld.%01ld ms  total=%ld.%01ld ms%s\n",
                (t1 - t0) / 1000, ((t1 - t0) % 1000) / 100,
                (t2 - t1) / 1000, ((t2 - t1) % 1000) / 100,
                (t2 - t0) / 1000, ((t2 - t0) % 1000) / 100,
                (changed || force_write) ? "" : "  [unchanged, encode skipped]");
    }

    return rc;
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
    fprintf(stderr, "  -T         Print per-stage timings to stderr\n");
    fprintf(stderr, "  -S         Stream framed JPEGs on stdout (implies -d)\n");
    fprintf(stderr, "  -F         Faster, less accurate DCT (~15ms/frame)\n");
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

    while ((opt = getopt(argc, argv, "o:q:dDi:p:01hTSF")) != -1) {
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
            case 'T':
                g_timing = 1;
                break;
            case 'F':
                g_fast_dct = 1;
                break;
            case 'S':
                /* Stream mode implies continuous capture. */
                g_stream_mode = 1;
                daemon_mode = 1;
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

    /* Stream mode: buffer for the in-memory JPEG destination. */
    if (g_stream_mode) {
        g_stream_buf = malloc(STREAM_BUF_CAP);
        if (!g_stream_buf) {
            perror("Failed to allocate stream buffer");
            free(rgb_buf);
            munmap(fb0_map, fb_size);
            if (fb1_map) munmap(fb1_map, fb_size);
            close(fb0_fd);
            if (fb1_fd >= 0) close(fb1_fd);
            return 1;
        }
    }

    /* Setup signal handlers for daemon mode */
    if (daemon_mode) {
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        /* Write PID file. Skipped when streaming: the host owns the process
         * lifetime through the pipe, and a stale pidfile would only confuse
         * the app's "is a daemon running" check. */
        if (!g_stream_mode && write_pidfile(pidfile) != 0) {
            fprintf(stderr, "Warning: could not write PID file\n");
        }

        if (!fork_to_background && !g_stream_mode) {
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
    /* Adaptive polling state: start at the requested rate, back off when idle. */
    int cur_interval_ms = interval_ms;
    long long last_change_ms = current_time_ms();
    int stats_frames = 0;
    int idle_max_ms = interval_ms * 5;
    if (idle_max_ms > 250) idle_max_ms = 250;

    do {
        long long frame_start = current_time_ms();

        /* force_write on the very first frame so the output file always exists */
        if (capture_screen(fb0_map, fb1_map, rgb_buf, output, quality,
                           frame_count == 0) != 0) {
            fprintf(stderr, "Capture failed\n");
            ret = 1;
            break;
        }

        frame_count++;
        stats_frames++;

        if (daemon_mode) {
            /* Adaptive poll interval.
             *
             * Even when nothing changes, an iteration still costs ~20ms: the
             * change check has to composite, and that means reading 6MB of
             * UNCACHED framebuffer. At a flat 40ms interval that is ~60% of a
             * core burned on a completely static screen - which is not just a
             * battery problem. It steals CPU from LunaSysMgr, so the very
             * animations we are trying to capture get jankier.
             *
             * So: poll fast while the screen is changing (that is when frames
             * matter), and back off geometrically when it is idle. Any change
             * snaps the interval straight back to the base rate, so the tail of
             * an animation is still sampled at full speed.
             *
             * Note this only delays noticing that a NEW change has STARTED, by
             * at most idle_max_ms. It cannot cause a stale final frame: every
             * frame that differs is still encoded (full-frame checksum), so the
             * settled frame is always transmitted.
             */
            if (g_frame_changed) {
                last_change_ms = current_time_ms();
                cur_interval_ms = interval_ms;          /* activity: full rate */
            } else if (current_time_ms() - last_change_ms < SETTLE_HOLD_MS) {
                cur_interval_ms = interval_ms;          /* settle window: stay fast */
            } else if (cur_interval_ms < idle_max_ms) {
                cur_interval_ms += cur_interval_ms / 2; /* idle: back off 1.5x */
                if (cur_interval_ms > idle_max_ms) {
                    cur_interval_ms = idle_max_ms;
                }
            }

            /* Calculate time to sleep */
            long long elapsed = current_time_ms() - frame_start;
            long long sleep_ms = cur_interval_ms - elapsed;

            if (sleep_ms > 0) {
                usleep(sleep_ms * 1000);
            }

            /* Print stats every 5 seconds */
            long long total_elapsed = current_time_ms() - start_time;
            if (total_elapsed >= 5000) {
                /* Use a separate counter: frame_count drives the first-frame
                 * force_write, and zeroing it here made the daemon re-send an
                 * identical frame every 5 seconds. */
                double fps = (double)stats_frames * 1000.0 / total_elapsed;
                fprintf(stderr, "FPS: %.1f, frames: %d\n", fps, stats_frames);
                stats_frames = 0;
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
