/*
 * lunecast-input - inject touch input into webOS (EXPERIMENTAL)
 *
 * Usage:
 *   lunecast-input tap  X Y                 a tap
 *   lunecast-input hold X Y [MS]            press and hold (default 1200ms)
 *   lunecast-input drag X1 Y1 X2 Y2 [MS]    drag/swipe (default 300ms)
 *   lunecast-input X Y                      shorthand for "tap X Y"
 *
 * Coordinates are the panel's own, 0..1023 x 0..767.
 *
 * Sends synthetic touch directly to LunaSysMgr. Nothing is installed and
 * nothing is modified: this binary ships in the app directory, so
 * palm-uninstall removes it completely.
 *
 * HOW THIS WORKS
 *
 * The TouchPad's touchscreen is not an evdev device - it arrives over
 * /dev/ctp_uart into hidd, which forwards to LunaSysMgr. Critically,
 * LunaSysMgr is the one that BINDS /var/run/hidd/TouchpanelEventSocket
 * (verified by finding the socket's inode in its fd table); hidd is merely a
 * client that sends datagrams to it. So we can send the same datagrams.
 *
 * That is why this needs no hidd plugin, no file in /usr/lib, no edit to
 * /etc/hidd/HidPlugins.xml and no hidd restart. See
 * experimental/remote-input/README.md for the plugin route that was tried
 * first, and for how the wire format below was recovered.
 *
 * WIRE FORMAT - captured from a real finger, not guessed:
 *
 *   DOWN (5 events): type7/0/id, BTN_TOUCH=1, ABS_X, ABS_Y, SYN_REPORT
 *   MOVE (4 events): type7/0/id,              ABS_X, ABS_Y, SYN_REPORT
 *   UP   (5 events): type7/0/id, ABS_X, ABS_Y, BTN_TOUCH=0, SYN_REPORT
 *
 * Each batch is one datagram of 16-byte Linux struct input_event. type 7 is a
 * webOS-specific contact id. Real moves arrive about 10ms apart.
 *
 * Timestamps MUST be CLOCK_MONOTONIC. LunaSysMgr does gesture timing (tap vs
 * hold, flick velocity) on them, and wall-clock values are ~1.7 billion
 * seconds adrift of the stream they join, so they are discarded in silence.
 *
 * WHY GESTURES ARE GENERATED HERE rather than streamed from the host: a drag
 * is a burst of move events 10ms apart. Sending each one as its own
 * `novacom run` would contend badly with the frame stream and the timing would
 * be at the mercy of USB latency - and LunaSysMgr derives flick velocity from
 * those timestamps. Emitting the whole gesture on-device costs one novacom
 * round trip and keeps the cadence honest.
 *
 * THE GESTURE AREA is not a separate coordinate space. Captured from a real
 * swipe-up off the bezel below the screen: it reports x 554..587, y 419..763,
 * starting at y=763. Nothing ever reported y > 767. So the strip below the
 * display maps into the very bottom rows of the same 1024x768 space, and the
 * webOS back/minimise gesture is just
 *
 *     drag X 763  X 420
 *
 * That also means the gesture area sits INSIDE the streamed image, so a user
 * can perform it by dragging up from the bottom edge of the picture.
 *
 * Must run as root - the socket is srwxr-xr-x root, so the jailed app (uid
 * 5003) cannot use this. novacom runs as root, which is where taps come from.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/input.h>

#define TOUCH_SOCKET "/var/run/hidd/TouchpanelEventSocket"
#define EV_FINGERID  7          /* webOS-specific contact id event */
#define SCREEN_W     1024
#define SCREEN_H     768
#define STEP_MS      10         /* the real panel reports at ~100Hz */
#define TAP_HOLD_MS  30         /* contact time for a plain tap */
#define DEF_HOLD_MS  1200       /* comfortably past the long-press threshold */
#define DEF_DRAG_MS  300
#define FINGER_ID    0

static int g_fd = -1;
static struct sockaddr_un g_addr;

static void fill(struct input_event *e, int type, int code, int value) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    e->time.tv_sec  = ts.tv_sec;
    e->time.tv_usec = ts.tv_nsec / 1000;
    e->type  = (unsigned short)type;
    e->code  = (unsigned short)code;
    e->value = value;
}

static int send_batch(struct input_event *ev, int n) {
    if (sendto(g_fd, ev, n * sizeof(*ev), 0,
               (struct sockaddr *)&g_addr, sizeof(g_addr)) < 0) {
        fprintf(stderr, "sendto %s: %s\n", TOUCH_SOCKET, strerror(errno));
        return -1;
    }
    return 0;
}

static int clamp(int v, int hi) {
    return v < 0 ? 0 : (v > hi ? hi : v);
}

static int touch_down(int x, int y) {
    struct input_event ev[5];
    fill(&ev[0], EV_FINGERID, 0, FINGER_ID);
    fill(&ev[1], EV_KEY, BTN_TOUCH, 1);
    fill(&ev[2], EV_ABS, ABS_X, x);
    fill(&ev[3], EV_ABS, ABS_Y, y);
    fill(&ev[4], EV_SYN, SYN_REPORT, 0);
    return send_batch(ev, 5);
}

static int touch_move(int x, int y) {
    struct input_event ev[4];
    fill(&ev[0], EV_FINGERID, 0, FINGER_ID);
    fill(&ev[1], EV_ABS, ABS_X, x);
    fill(&ev[2], EV_ABS, ABS_Y, y);
    fill(&ev[3], EV_SYN, SYN_REPORT, 0);
    return send_batch(ev, 4);
}

static int touch_up(int x, int y) {
    struct input_event ev[5];
    fill(&ev[0], EV_FINGERID, 0, FINGER_ID);
    fill(&ev[1], EV_ABS, ABS_X, x);
    fill(&ev[2], EV_ABS, ABS_Y, y);
    fill(&ev[3], EV_KEY, BTN_TOUCH, 0);
    fill(&ev[4], EV_SYN, SYN_REPORT, 0);
    return send_batch(ev, 5);
}

/* Hold a stationary contact for duration_ms, reporting at the panel's rate.
 * A finger resting on glass keeps producing move events; going silent between
 * down and up would not look like a real press to the gesture handling. */
static int gesture_hold(int x, int y, int duration_ms) {
    int steps = duration_ms / STEP_MS;

    if (touch_down(x, y) != 0) return -1;
    for (int i = 0; i < steps; i++) {
        usleep(STEP_MS * 1000);
        if (touch_move(x, y) != 0) return -1;
    }
    usleep(STEP_MS * 1000);
    return touch_up(x, y);
}

/* Linear drag. The intermediate points matter: LunaSysMgr computes flick
 * velocity from the last few, so a down-then-jump-then-up produces either
 * nothing or an enormous accidental flick. */
static int gesture_drag(int x1, int y1, int x2, int y2, int duration_ms) {
    int steps = duration_ms / STEP_MS;
    if (steps < 2) steps = 2;

    if (touch_down(x1, y1) != 0) return -1;

    for (int i = 1; i <= steps; i++) {
        int x = x1 + (x2 - x1) * i / steps;
        int y = y1 + (y2 - y1) * i / steps;
        usleep(STEP_MS * 1000);
        if (touch_move(x, y) != 0) return -1;
    }

    usleep(STEP_MS * 1000);
    return touch_up(x2, y2);
}

static void usage(const char *prog) {
    fprintf(stderr,
        "usage:\n"
        "  %s tap  X Y\n"
        "  %s hold X Y [MS]            (default %d)\n"
        "  %s drag X1 Y1 X2 Y2 [MS]    (default %d)\n"
        "  %s X Y                      (shorthand for tap)\n"
        "coordinates: 0..%d x 0..%d\n",
        prog, prog, DEF_HOLD_MS, prog, DEF_DRAG_MS, prog,
        SCREEN_W - 1, SCREEN_H - 1);
}

int main(int argc, char *argv[]) {
    if (argc < 3) { usage(argv[0]); return 2; }

    g_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (g_fd < 0) { perror("socket"); return 1; }
    memset(&g_addr, 0, sizeof(g_addr));
    g_addr.sun_family = AF_UNIX;
    snprintf(g_addr.sun_path, sizeof(g_addr.sun_path), "%s", TOUCH_SOCKET);

    const char *cmd = argv[1];
    int rc;

    if (strcmp(cmd, "drag") == 0) {
        if (argc < 6) { usage(argv[0]); return 2; }
        int x1 = clamp(atoi(argv[2]), SCREEN_W - 1);
        int y1 = clamp(atoi(argv[3]), SCREEN_H - 1);
        int x2 = clamp(atoi(argv[4]), SCREEN_W - 1);
        int y2 = clamp(atoi(argv[5]), SCREEN_H - 1);
        int ms = (argc > 6) ? atoi(argv[6]) : DEF_DRAG_MS;
        rc = gesture_drag(x1, y1, x2, y2, ms);
    } else if (strcmp(cmd, "hold") == 0) {
        if (argc < 4) { usage(argv[0]); return 2; }
        int x = clamp(atoi(argv[2]), SCREEN_W - 1);
        int y = clamp(atoi(argv[3]), SCREEN_H - 1);
        int ms = (argc > 4) ? atoi(argv[4]) : DEF_HOLD_MS;
        rc = gesture_hold(x, y, ms);
    } else {
        /* "tap X Y" or the bare "X Y" shorthand */
        int base = (strcmp(cmd, "tap") == 0) ? 2 : 1;
        if (argc < base + 2) { usage(argv[0]); return 2; }
        int x = clamp(atoi(argv[base]), SCREEN_W - 1);
        int y = clamp(atoi(argv[base + 1]), SCREEN_H - 1);
        rc = gesture_hold(x, y, TAP_HOLD_MS);
    }

    close(g_fd);
    return rc == 0 ? 0 : 1;
}
