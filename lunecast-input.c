/*
 * lunecast-input - inject a tap into webOS (EXPERIMENTAL)
 *
 * Usage: lunecast-input X Y        (device coordinates, 1024x768)
 *
 * Sends a synthetic touch directly to LunaSysMgr. Nothing is installed and
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
 * This one detail cost three rounds of otherwise-correct attempts.
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
#define MOVE_STEPS   2          /* stationary updates between down and up */
#define STEP_US      10000      /* ~ the real panel's 100Hz report rate */

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

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s X Y   (0-%d, 0-%d)\n",
                argv[0], SCREEN_W - 1, SCREEN_H - 1);
        return 2;
    }

    int x = atoi(argv[1]);
    int y = atoi(argv[2]);
    const int id = 0;
    struct input_event ev[5];

    if (x < 0) x = 0;
    if (x >= SCREEN_W) x = SCREEN_W - 1;
    if (y < 0) y = 0;
    if (y >= SCREEN_H) y = SCREEN_H - 1;

    g_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (g_fd < 0) {
        perror("socket");
        return 1;
    }
    memset(&g_addr, 0, sizeof(g_addr));
    g_addr.sun_family = AF_UNIX;
    snprintf(g_addr.sun_path, sizeof(g_addr.sun_path), "%s", TOUCH_SOCKET);

    /* down */
    fill(&ev[0], EV_FINGERID, 0, id);
    fill(&ev[1], EV_KEY, BTN_TOUCH, 1);
    fill(&ev[2], EV_ABS, ABS_X, x);
    fill(&ev[3], EV_ABS, ABS_Y, y);
    fill(&ev[4], EV_SYN, SYN_REPORT, 0);
    if (send_batch(ev, 5) != 0) return 1;
    usleep(STEP_US);

    /* a couple of stationary updates, as a resting finger produces */
    for (int i = 0; i < MOVE_STEPS; i++) {
        fill(&ev[0], EV_FINGERID, 0, id);
        fill(&ev[1], EV_ABS, ABS_X, x);
        fill(&ev[2], EV_ABS, ABS_Y, y);
        fill(&ev[3], EV_SYN, SYN_REPORT, 0);
        if (send_batch(ev, 4) != 0) return 1;
        usleep(STEP_US);
    }

    /* up */
    fill(&ev[0], EV_FINGERID, 0, id);
    fill(&ev[1], EV_ABS, ABS_X, x);
    fill(&ev[2], EV_ABS, ABS_Y, y);
    fill(&ev[3], EV_KEY, BTN_TOUCH, 0);
    fill(&ev[4], EV_SYN, SYN_REPORT, 0);
    if (send_batch(ev, 5) != 0) return 1;

    close(g_fd);
    return 0;
}
