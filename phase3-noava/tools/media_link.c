// media_link - enable (or disable) a media-controller link.
// Usage: media_link /dev/media0 <src_ent> <src_pad> <sink_ent> <sink_pad> [0|1]
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/media.h>

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s /dev/media0 se sp de dp [0|1]\n", argv[0]); return 2; }
    int en = argc > 6 ? atoi(argv[6]) : 1;
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct media_link_desc l; memset(&l, 0, sizeof l);
    l.source.entity = atoi(argv[2]); l.source.index = atoi(argv[3]);
    l.sink.entity   = atoi(argv[4]); l.sink.index   = atoi(argv[5]);
    l.flags = en ? MEDIA_LNK_FL_ENABLED : 0;
    if (ioctl(fd, MEDIA_IOC_SETUP_LINK, &l) < 0) {
        fprintf(stderr, "SETUP_LINK e%s/p%s->e%s/p%s en=%d: %s\n",
                argv[2], argv[3], argv[4], argv[5], en, strerror(errno));
        return 1;
    }
    printf("link e%s/p%s -> e%s/p%s enabled=%d OK\n", argv[2], argv[3], argv[4], argv[5], en);
    close(fd);
    return 0;
}
