// media_topo - dump the /dev/media0 pipeline graph (entities + links).
// Reveals whether ov8856 (RGB) and ofilm0092/sunnytof (ToF) use separate
// CSI/ISP paths (=> can stream concurrently) or share them. Read-only.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/media.h>

int main(int argc, char **argv) {
    const char *dev = argc > 1 ? argv[1] : "/dev/media0";
    int fd = open(dev, O_RDWR);
    if (fd < 0) { perror("open"); return 1; }

    struct media_device_info di; memset(&di, 0, sizeof di);
    if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &di) == 0)
        printf("media: driver=%s model=%s\n\n", di.driver, di.model);

    __u32 id = 0;
    for (;;) {
        struct media_entity_desc ent; memset(&ent, 0, sizeof ent);
        ent.id = id | MEDIA_ENT_ID_FLAG_NEXT;
        if (ioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &ent) < 0) break;
        id = ent.id;
        printf("entity %2u: %-24s type=0x%08x pads=%u links=%u",
               ent.id, ent.name, ent.type, ent.pads, ent.links);
        if ((ent.type & 0xffff0000) == 0x00020000) printf("  [subdev]");
        if (ent.dev.major || ent.dev.minor) printf("  dev=%u:%u", ent.dev.major, ent.dev.minor);
        printf("\n");

        if (ent.links == 0 && ent.pads == 0) continue;
        struct media_pad_desc *pads = calloc(ent.pads ? ent.pads : 1, sizeof *pads);
        struct media_link_desc *links = calloc(ent.links ? ent.links : 1, sizeof *links);
        struct media_links_enum le; memset(&le, 0, sizeof le);
        le.entity = ent.id; le.pads = pads; le.links = links;
        if (ioctl(fd, MEDIA_IOC_ENUM_LINKS, &le) == 0) {
            for (unsigned i = 0; i < ent.links; i++) {
                struct media_link_desc *l = &links[i];
                printf("     link: e%u/pad%u -> e%u/pad%u  flags=0x%x%s%s\n",
                       l->source.entity, l->source.index,
                       l->sink.entity, l->sink.index, l->flags,
                       (l->flags & MEDIA_LNK_FL_ENABLED) ? " ENABLED" : "",
                       (l->flags & MEDIA_LNK_FL_IMMUTABLE) ? " immutable" : "");
            }
        }
        free(pads); free(links);
    }
    close(fd);
    return 0;
}
