// subdev_probe - enumerate a v4l2 subdev's mbus codes + frame sizes + current fmt.
// Usage: subdev_probe /dev/v4l-subdevN [pad]
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/v4l2-subdev.h>

static void p4cc(unsigned f, char *s) { s[0]=f; s[1]=f>>8; s[2]=f>>16; s[3]=f>>24; s[4]=0; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s /dev/v4l-subdevN [pad]\n", argv[0]); return 2; }
    unsigned pad = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    printf("== %s pad %u\n", argv[1], pad);
    printf("-- mbus codes:\n");
    for (unsigned i = 0;; i++) {
        struct v4l2_subdev_mbus_code_enum mc; memset(&mc, 0, sizeof mc);
        mc.pad = pad; mc.index = i; mc.which = V4L2_SUBDEV_FORMAT_ACTIVE;
        if (ioctl(fd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &mc) < 0) {
            if (i == 0) printf("   (none: %s)\n", strerror(errno));
            break;
        }
        printf("   code[%u] = 0x%04x\n", i, mc.code);
        for (unsigned j = 0;; j++) {
            struct v4l2_subdev_frame_size_enum fs; memset(&fs, 0, sizeof fs);
            fs.pad = pad; fs.index = j; fs.code = mc.code; fs.which = V4L2_SUBDEV_FORMAT_ACTIVE;
            if (ioctl(fd, VIDIOC_SUBDEV_ENUM_FRAME_SIZE, &fs) < 0) break;
            printf("        %ux%u .. %ux%u\n", fs.min_width, fs.min_height, fs.max_width, fs.max_height);
            if (j > 12) break;
        }
    }

    struct v4l2_subdev_format sf; memset(&sf, 0, sizeof sf);
    sf.pad = pad; sf.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    if (ioctl(fd, VIDIOC_SUBDEV_G_FMT, &sf) == 0) {
        char c[5]; p4cc(sf.format.code, c);
        printf("-- current ACTIVE fmt: %ux%u code=0x%04x field=%u\n",
               sf.format.width, sf.format.height, sf.format.code, sf.format.field);
    } else {
        printf("-- G_FMT: %s\n", strerror(errno));
    }
    close(fd);
    return 0;
}
