/*
 * touchcap - record what the REAL HidTouchpanel plugin sends to its consumer.
 *
 * Three informed guesses at the touch event format all failed while a
 * synthetic KEY event went straight through, so the encoding is not the
 * problem - something about the touch stream's shape is. Rather than guess a
 * fourth time, bind the address the touchpanel plugin publishes to and write
 * down exactly what a real finger produces.
 *
 * hidd sends to the event socket as a CLIENT (proved by inode/mtime: the cmd
 * sockets are recreated on a hidd restart, the event sockets are not - so the
 * consumer owns them). So we bind the path, point HidTouchpanel at us in
 * HidPlugins.xml, and hidd's datagrams land here instead of LunaSysMgr.
 *
 * While this runs, real touches do NOT reach the UI - they are being recorded
 * instead. Restore the original XML to undo.
 *
 * Detaches so `novacom run` returns immediately. Log: /media/internal/touchcap.log
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/time.h>

#define CAP_SOCKET "/var/run/hidd/CaptureSocket"
#define CAP_LOG    "/media/internal/touchcap.log"
#define MAXDG      65536

int main(void) {
    /* detach */
    if (fork() > 0) _exit(0);
    setsid();
    if (fork() > 0) _exit(0);
    chdir("/");

    FILE *log = fopen(CAP_LOG, "w");
    if (!log) return 1;
    setvbuf(log, NULL, _IOLBF, 0);

    unlink(CAP_SOCKET);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { fprintf(log, "socket: %s\n", strerror(errno)); return 1; }

    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", CAP_SOCKET);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(log, "bind %s: %s\n", CAP_SOCKET, strerror(errno));
        return 1;
    }
    chmod(CAP_SOCKET, 0666);
    fprintf(log, "bound %s, waiting for datagrams\n", CAP_SOCKET);

    unsigned char *buf = malloc(MAXDG);
    int n_dg = 0;

    for (;;) {
        ssize_t n = recv(fd, buf, MAXDG, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(log, "recv: %s\n", strerror(errno));
            break;
        }
        n_dg++;

        struct timeval tv;
        gettimeofday(&tv, NULL);
        fprintf(log, "\n=== datagram %d: %ld bytes (%ld.%06ld) ===\n",
                n_dg, (long)n, (long)tv.tv_sec, (long)tv.tv_usec);

        /* raw hex - the header, if any, will show up here */
        fprintf(log, "hex:");
        for (ssize_t i = 0; i < n && i < 160; i++) {
            if (i % 16 == 0) fprintf(log, "\n  %04lx: ", (long)i);
            fprintf(log, "%02x ", buf[i]);
        }
        fprintf(log, "\n");

        /* If it is a whole number of 16-byte input_events, decode them. If it
         * is not, the remainder tells us how big a header hidd prepends. */
        fprintf(log, "size %% 16 = %ld", (long)(n % 16));
        if (n % 16 == 0) {
            fprintf(log, "  -> %ld input_event(s), decoded:\n", (long)(n / 16));
            for (ssize_t i = 0; i + 16 <= n; i += 16) {
                unsigned int sec, usec, val;
                unsigned short type, code;
                memcpy(&sec,  buf + i,      4);
                memcpy(&usec, buf + i + 4,  4);
                memcpy(&type, buf + i + 8,  2);
                memcpy(&code, buf + i + 10, 2);
                memcpy(&val,  buf + i + 12, 4);
                fprintf(log, "    [%2ld] type=%-3u code=%-4u value=%-6d  t=%u.%06u\n",
                        (long)(i / 16), type, code, (int)val, sec, usec);
            }
        } else {
            fprintf(log, "  -> NOT a clean multiple of 16: hidd prepends a header\n");
        }
    }
    return 0;
}
