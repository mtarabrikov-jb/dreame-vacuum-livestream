// subdev_setfmt - set a v4l2 subdev pad format (VIDIOC_SUBDEV_S_FMT, ACTIVE).
// Usage: subdev_setfmt /dev/v4l-subdevN pad W H CODEHEX
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/v4l2-subdev.h>

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s /dev/v4l-subdevN pad W H CODEHEX\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    struct v4l2_subdev_format sf; memset(&sf, 0, sizeof sf);
    sf.pad = (unsigned)atoi(argv[2]);
    sf.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    sf.format.width  = (unsigned)atoi(argv[3]);
    sf.format.height = (unsigned)atoi(argv[4]);
    sf.format.code   = (unsigned)strtol(argv[5], NULL, 16);
    sf.format.field  = 1;   // V4L2_FIELD_NONE
    if (ioctl(fd, VIDIOC_SUBDEV_S_FMT, &sf) < 0) {
        fprintf(stderr, "S_FMT %s pad%s %sx%s 0x%s: %s\n",
                argv[1], argv[2], argv[3], argv[4], argv[5], strerror(errno));
        return 1;
    }
    printf("%s pad%s -> %ux%u code=0x%04x OK\n", argv[1], argv[2],
           sf.format.width, sf.format.height, sf.format.code);
    close(fd);
    return 0;
}
