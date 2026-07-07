// query_ctrl - enumerate V4L2 controls on a device/subdev + current values.
// Usage: query_ctrl /dev/v4l-subdevN
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s /dev/...\n", argv[0]); return 2; }
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    printf("== controls on %s\n", argv[1]);
    struct v4l2_queryctrl q; memset(&q, 0, sizeof q);
    q.id = V4L2_CTRL_FLAG_NEXT_CTRL;
    int n = 0;
    while (ioctl(fd, VIDIOC_QUERYCTRL, &q) == 0) {
        long val = 0; int have = 0;
        struct v4l2_control c; memset(&c, 0, sizeof c); c.id = q.id;
        if (ioctl(fd, VIDIOC_G_CTRL, &c) == 0) { val = c.value; have = 1; }
        printf("  id=0x%08x type=%u  %-28s [%d..%d step %d def %d]%s",
               q.id, q.type, q.name, q.minimum, q.maximum, q.step, q.default_value,
               (q.flags & V4L2_CTRL_FLAG_DISABLED) ? " DISABLED" : "");
        if (have) printf("  = %ld", val);
        printf("\n");
        n++;
        q.id |= V4L2_CTRL_FLAG_NEXT_CTRL;
    }
    if (n == 0) printf("  (none: %s)\n", strerror(errno));
    close(fd);
    return 0;
}
