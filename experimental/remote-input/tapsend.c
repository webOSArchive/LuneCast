/* tapsend X Y - send a tap request to the LuneCast hidd inject plugin */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char *argv[]) {
    if (argc < 3) { fprintf(stderr, "usage: tapsend X Y\n"); return 1; }
    int msg[3] = { argc>3?atoi(argv[1]):0, argc>3?atoi(argv[2]):atoi(argv[1]), argc>3?atoi(argv[3]):atoi(argv[2]) };

    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "/var/run/hidd/LuneCastInject");

    if (sendto(fd, msg, sizeof(msg), 0, (struct sockaddr *)&a, sizeof(a)) < 0) {
        perror("sendto"); close(fd); return 1;
    }
    printf("sent kind=%d p1=%d p2=%d\n", msg[0], msg[1], msg[2]);
    close(fd);
    return 0;
}
