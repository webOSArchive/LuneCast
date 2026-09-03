/*
 * lunecast_inject - a hidd plugin that injects synthetic touch events.
 *
 * WHY A PLUGIN: the TouchPad's touchscreen is not an evdev device. It arrives
 * over /dev/ctp_uart into hidd's HidTouchpanel plugin, which forwards to
 * LunaSysMgr over /var/run/hidd/TouchpanelEventSocket. A uinput device is
 * picked up by hidd's InputDev plugin instead, and lands on a socket
 * LunaSysMgr treats as HID/keyboard - which is why synthetic taps did nothing.
 * hidd's plugin interface is the designed way in, and HidPlugins.xml already
 * shows two plugins (HidKeypad, HidAvrcp) sharing one event socket - so a new
 * plugin can publish onto TouchpanelEventSocket alongside the real one.
 *
 * ABI, recovered from hidd (its symbols are intact):
 *
 *   hidd dlopen()s the .so and dlsym()s "PluginTable", a table of 6 function
 *   pointers (offsets confirmed from the call sites):
 *
 *     +0   SetReportCallback(ReportEventFn)   main() passes &ReportEvent
 *     +4   Init(ctx)                          ctx[1] = our plugin index;
 *                                             returns 0 for success
 *     +8   Exit()                             ExitPlugin  -> [table+8]
 *     +12  Suspend()                          SuspendPlugin -> [table+12]
 *     +16  Resume()                           ResumePlugin  -> [table+16]
 *     +20  Command()                          _PluginCommandCallback -> [+20]
 *
 *   ReportEvent(void *events, int count, int type, int pluginIndex)
 *     type 0 tail-calls _ReportStandardEvent, which reads each element as
 *     ldm {tv_sec,tv_usec}; ldrh +8; ldrh +10; ldr +12  -- i.e. a plain
 *     Linux struct input_event, 16 bytes.
 *
 * Taps arrive as datagrams on /var/run/hidd/LuneCastInject: two int32s, x and
 * y, in device coordinates (1024x768).
 *
 * Logs to /media/internal/lunecast-plugin.log - a plugin's stdout goes nowhere.
 */

#include <stdio.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <linux/input.h>

#define INJECT_SOCKET "/var/run/hidd/LuneCastInject"
#define LOG_PATH      "/media/internal/lunecast-plugin.log"

typedef int (*ReportEventFn)(void *events, int count, int type, int pluginIndex);

static ReportEventFn g_report;
static int           g_plugin_index;
static pthread_t     g_thread;
static int           g_running;

static void plog(const char *fmt, ...) {
    va_list ap;
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Timestamps MUST be CLOCK_MONOTONIC, not wall clock.
 *
 * The captured real stream carries tv_sec around 47 while the wall clock read
 * 1788459443 - a different time domain entirely. libhid's HidGetTimeStamp is
 * clock_gettime(CLOCK_MONOTONIC), and LunaSysMgr does gesture timing (tap vs
 * hold, flick velocity) on these values, so events stamped 1.7 billion seconds
 * "later" than the stream they join are nonsense to it. */
static void fill(struct input_event *e, int type, int code, int value) {
    struct timespec ts;
    struct timeval tv;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    tv.tv_sec  = ts.tv_sec;
    tv.tv_usec = ts.tv_nsec / 1000;
    e->time = tv;
    e->type = (unsigned short)type;
    e->code = (unsigned short)code;
    e->value = value;
}

/* One tap, matching what the REAL touchpanel plugin emits.
 *
 * Recovered from libhidtouchpanel.so: CreateFingerUpEvent and
 * GestureStateMachineFinger both build their batches as
 *
 *     type=7,  code=0,   value=fingerId    <- webOS-specific contact id
 *     type=3,  code=0,   value=X           <- EV_ABS / ABS_X
 *     type=3,  code=1,   value=Y           <- EV_ABS / ABS_Y
 *     type=1,  code=330, value=1 or 0      <- EV_KEY / BTN_TOUCH
 *
 * Note there is NO SYN_REPORT: hidd batches events itself via the count
 * argument to ReportEvent. My first attempt sent a SYN and omitted the
 * finger-id event, and LunaSysMgr ignored it entirely.
 */
#define EV_FINGERID 7

/* EXACT sequence a real finger produces, captured off the wire by binding
 * HidTouchpanel's event socket while a human tapped the screen:
 *
 *   DOWN (5 events): type7/0/id, BTN_TOUCH=1, ABS_X, ABS_Y, SYN_REPORT
 *   MOVE (4 events): type7/0/id,              ABS_X, ABS_Y, SYN_REPORT
 *   UP   (5 events): type7/0/id, ABS_X, ABS_Y, BTN_TOUCH=0, SYN_REPORT
 *
 * Two things I had wrong from static analysis alone. There IS a trailing
 * SYN_REPORT on every batch - CreateFingerUpEvent does not add it, so reading
 * that function had me drop it. And on DOWN the BTN_TOUCH comes BEFORE the
 * coordinates, while on UP it comes after. Real moves arrive ~10ms apart.
 */
static void send_down(int id, int x, int y) {
    struct input_event ev[5];
    fill(&ev[0], EV_FINGERID, 0, id);
    fill(&ev[1], EV_KEY, BTN_TOUCH, 1);
    fill(&ev[2], EV_ABS, ABS_X, x);
    fill(&ev[3], EV_ABS, ABS_Y, y);
    fill(&ev[4], EV_SYN, SYN_REPORT, 0);
    g_report(ev, 5, 0, g_plugin_index);
}

static void send_move(int id, int x, int y) {
    struct input_event ev[4];
    fill(&ev[0], EV_FINGERID, 0, id);
    fill(&ev[1], EV_ABS, ABS_X, x);
    fill(&ev[2], EV_ABS, ABS_Y, y);
    fill(&ev[3], EV_SYN, SYN_REPORT, 0);
    g_report(ev, 4, 0, g_plugin_index);
}

static void send_up(int id, int x, int y) {
    struct input_event ev[5];
    fill(&ev[0], EV_FINGERID, 0, id);
    fill(&ev[1], EV_ABS, ABS_X, x);
    fill(&ev[2], EV_ABS, ABS_Y, y);
    fill(&ev[3], EV_KEY, BTN_TOUCH, 0);
    fill(&ev[4], EV_SYN, SYN_REPORT, 0);
    g_report(ev, 5, 0, g_plugin_index);
}

static void emit_tap(int x, int y) {
    const int id = 0;          /* the captured stream uses finger id 0 */
    if (!g_report) return;

    send_down(id, x, y);
    usleep(10000);
    send_move(id, x, y);       /* a couple of stationary updates, as a real  */
    usleep(10000);             /* finger produces while resting on the glass */
    send_move(id, x, y);
    usleep(10000);
    send_up(id, x, y);

    plog("tap (%d,%d) captured-format sequence via plugin %d", x, y, g_plugin_index);
}

/* Experiment 2: emit a KEY event instead of a touch.
 *
 * Purpose is diagnostic. If a synthetic keypress reaches the UI while
 * synthetic touches do not, then hidd IS routing our plugin's events and only
 * the touch event shape is wrong. If neither arrives, the problem is upstream
 * of the format - our plugin's events are being dropped before they ever reach
 * LunaSysMgr, and the format work is moot until that is fixed. */
static void emit_key(int code, int unused) {
    struct input_event ev[1];
    (void)unused;
    if (!g_report) return;

    fill(&ev[0], EV_KEY, code, 1);      /* press   */
    g_report(ev, 1, 0, g_plugin_index);
    usleep(60000);
    fill(&ev[0], EV_KEY, code, 0);      /* release */
    g_report(ev, 1, 0, g_plugin_index);

    plog("key %d press+release sent via plugin %d", code, g_plugin_index);
}

static void *inject_thread(void *arg) {
    struct sockaddr_un addr;
    int fd;
    (void)arg;

    unlink(INJECT_SOCKET);
    fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        plog("socket failed: %s", strerror(errno));
        return NULL;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", INJECT_SOCKET);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        plog("bind failed: %s", strerror(errno));
        close(fd);
        return NULL;
    }
    chmod(INJECT_SOCKET, 0666);
    plog("listening on %s", INJECT_SOCKET);

    /* Self-test: one tap shortly after start, so the plugin proves itself even
     * with nothing sending to the socket. Coordinates chosen by the caller via
     * the LUNECAST_SELFTEST env var, else skipped. */
    const char *st = getenv("LUNECAST_SELFTEST");
    if (st) {
        int sx = 0, sy = 0;
        if (sscanf(st, "%d,%d", &sx, &sy) == 2) {
            sleep(8);
            plog("self-test tap at (%d,%d)", sx, sy);
            emit_tap(sx, sy);
        }
    }

    while (g_running) {
        int buf[3];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == (ssize_t)sizeof(buf)) {
            if (buf[0] == 1) emit_key(buf[1], buf[2]);
            else             emit_tap(buf[1], buf[2]);
        } else if (n < 0 && errno != EINTR) {
            plog("recv failed: %s", strerror(errno));
            break;
        }
    }
    close(fd);
    unlink(INJECT_SOCKET);
    return NULL;
}

/* --- PluginTable entries ------------------------------------------------- */

static void pt_set_report(ReportEventFn fn) {
    g_report = fn;
    plog("SetReportCallback(%p)", (void *)fn);
}

static int pt_init(void *ctx) {
    /* ctx is &plugin_info[248]: word 0 is hidd's own, word 1 is our index. */
    g_plugin_index = ctx ? ((int *)ctx)[1] : 0;
    g_running = 1;
    plog("--- lunecast_inject init, plugin index %d ---", g_plugin_index);
    if (pthread_create(&g_thread, NULL, inject_thread, NULL) != 0) {
        plog("pthread_create failed");
        return -1;
    }
    return 0;   /* hidd treats 0 as success */
}

static int pt_exit(void)    { g_running = 0; plog("exit"); return 0; }
static int pt_suspend(void) { plog("suspend"); return 0; }
static int pt_resume(void)  { plog("resume"); return 0; }
static int pt_command(void *a, void *b) { (void)a; (void)b; return 0; }

void *PluginTable[6] = {
    (void *)pt_set_report,
    (void *)pt_init,
    (void *)pt_exit,
    (void *)pt_suspend,
    (void *)pt_resume,
    (void *)pt_command,
};
