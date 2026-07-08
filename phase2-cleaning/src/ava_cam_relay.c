// ---------------------------------------------------------------------------
// ava_cam_relay — Phase 2 Source B: infrared "watch during cleaning" feed.
//
// Reads the raw ToF frame that libcamtap (CAMTAP_IR=1) copies from /dev/video1
// into /tmp/camtap.shm, turns it into a viewable grayscale INFRARED image
// (ir_process.h), software-JPEG-encodes it (jpeg_gray.h — stateless, no vendor
// codec), and serves an MJPEG stream on 127.0.0.1:<port>. The robot's existing
// go2rtc consumes that as a source and serves RTSP / MSE to viewers.
//
//   ava + libcamtap ──raw ToF──▶ /tmp/camtap.shm ──▶ ava_cam_relay ──MJPEG──▶ 127.0.0.1:8090 ──▶ go2rtc
//
// Grayscale IR, not RGB (the RGB camera can't stream while cleaning). This is
// the live feed of what the robot sees the whole time it cleans.
//
// Env: IR_PORT (8090), IR_BAND (-1=max-project, 0..8=one sub-frame),
//      IR_SCALE (2), IR_QUALITY (80), CAM_SHM (/tmp/camtap.shm — the shm to
//      read; set a second path to run a second relay for the other camera).
//      Also: --stats to just report the tap.
// ---------------------------------------------------------------------------
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "camtap_shm.h"
#include "ir_process.h"
#include "jpeg_gray.h"

static volatile int g_stop = 0;
static void on_sig(int s){ (void)s; g_stop = 1; }
// install a handler WITHOUT SA_RESTART so blocking accept()/read() return EINTR
// and the main loop can notice g_stop and exit cleanly on SIGTERM.
static void catch_sig(int sig){ struct sigaction sa; memset(&sa,0,sizeof sa); sa.sa_handler=on_sig; sigaction(sig,&sa,NULL); }

static struct camtap_shm *open_shm(const char *path) {
	int fd = open(path, O_RDWR);
	if (fd < 0) return NULL;
	void *p = mmap(NULL, sizeof(struct camtap_shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	return (p == MAP_FAILED) ? NULL : (struct camtap_shm *)p;
}

// consistent read of one frame (any format) via the seqlock; returns 1 if new
static int read_frame(struct camtap_shm *shm, uint8_t *out, int cap,
                      int *w, int *h, int *fmt, int *size, uint64_t *last) {
	uint32_t s0 = shm->seq;
	if (s0 & 1u) return 0;
	__sync_synchronize();
	uint64_t fr = shm->frames;
	if (fr == *last) return 0;
	int ww = shm->width, hh = shm->height, ff = shm->format, sz = shm->size;
	if (sz <= 0 || sz > cap) return 0;
	memcpy(out, shm->data, (size_t)sz);
	__sync_synchronize();
	if (shm->seq != s0) return 0;
	*w = ww; *h = hh; *fmt = ff; *size = sz; *last = fr;
	return 1;
}

static int tcp_listen(int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a; memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(port);
	if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return -1; }
	if (listen(fd, 4) < 0) { perror("listen"); return -1; }
	return fd;
}
static int send_all(int fd, const void *b, int n) {
	const char *p = b; while (n > 0) { int w = write(fd, p, n); if (w <= 0) return -1; p += w; n -= w; } return 0;
}

int main(int argc, char **argv) {
	int stats = (argc > 1 && strcmp(argv[1], "--stats") == 0);
	int band  = getenv("IR_BAND")    ? atoi(getenv("IR_BAND"))    : -1;
	int scale = getenv("IR_SCALE")   ? atoi(getenv("IR_SCALE"))   : 2;
	int port  = getenv("IR_PORT")    ? atoi(getenv("IR_PORT"))    : 8090;
	int qual  = getenv("IR_QUALITY") ? atoi(getenv("IR_QUALITY")) : 80;
	const char *shmpath = getenv("CAM_SHM"); if (!shmpath) shmpath = CAMTAP_SHM_PATH;
	if (scale < 1) scale = 1; if (scale > 4) scale = 4;
	catch_sig(SIGINT); catch_sig(SIGTERM); signal(SIGPIPE, SIG_IGN);

	struct camtap_shm *shm = NULL;
	for (int i = 0; i < 50 && !shm && !g_stop; i++) { shm = open_shm(shmpath); if (!shm) usleep(200000); }
	if (!shm || shm->magic != CAMTAP_MAGIC) {
		fprintf(stderr, "no %s (is libcamtap CAMTAP_IR=1 active and is it cleaning?)\n", shmpath);
		return 1;
	}
	fprintf(stderr, "attached to %s\n", shmpath);

	static unsigned char frame[CAMTAP_MAX_FRAME];
	static unsigned char gray[IR_SUB * TOF_W];
	static unsigned char big[IR_SUB * TOF_W * 16];       // IR upscale, up to scale 4
	static unsigned char nv21[IR_SUB * TOF_W * 16 * 3 / 2]; // IR as NV21 (Y + neutral UV)
	static unsigned char jpg[2 * 1024 * 1024];
	int w = 0, h = 0, fmt = 0, size = 0;
	uint64_t last = 0;

	if (stats) {
		uint64_t cnt = 0;
		while (!g_stop) {
			if (read_frame(shm, frame, sizeof frame, &w, &h, &fmt, &size, &last)) { if (++cnt % 15 == 0)
				fprintf(stderr, "tap: %dx%d fmt=%d, %llu frames\n", w, h, fmt, (unsigned long long)shm->frames); }
			usleep(4000);
		}
		return 0;
	}

	int srv = tcp_listen(port);
	if (srv < 0) return 1;
	fprintf(stderr, "MJPEG on 127.0.0.1:%d/  (band=%d scale=%d q=%d) -> point go2rtc here\n", port, band, scale, qual);
	char hdr[128];

	while (!g_stop) {
		int cl = accept(srv, NULL, NULL);
		if (cl < 0) continue;
		char req[512]; int rn = read(cl, req, sizeof req - 1); (void)rn;   // go2rtc opens the URL
		const char *h0 = "HTTP/1.0 200 OK\r\n"
			"Content-Type: multipart/x-mixed-replace; boundary=ffcam\r\n"
			"Cache-Control: no-cache\r\nConnection: close\r\n\r\n";
		if (send_all(cl, h0, strlen(h0)) < 0) { close(cl); continue; }
		fprintf(stderr, "consumer connected\n");
		uint64_t l2 = 0;
		while (!g_stop) {
			if (!read_frame(shm, frame, sizeof frame, &w, &h, &fmt, &size, &l2)) { usleep(6000); continue; }
			int n;
			if (fmt == 100) {                       // ToF -> infrared (cleaning)
				tof_to_gray((const uint16_t *)frame, band, gray);
				ir_upscale(gray, TOF_W, IR_SUB, scale, big);
				int iw = TOF_W * scale, ih = IR_SUB * scale;
				// encode as YCbCr with neutral chroma (visually gray) — go2rtc's MJPEG
				// transcoder panics on a 1-component grayscale JPEG (expects YCbCr).
				memcpy(nv21, big, (size_t)iw * ih);
				memset(nv21 + iw * ih, 128, (size_t)iw * ih / 2);
				n = jpeg_encode_nv21(nv21, iw, ih, qual, jpg);
			} else {                                // NV21 -> full color (docked)
				n = jpeg_encode_nv21(frame, w, h, qual, jpg);
			}
			if (n <= 0) continue;
			int hl = snprintf(hdr, sizeof hdr,
				"--ffcam\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", n);
			if (send_all(cl, hdr, hl) < 0 || send_all(cl, jpg, n) < 0 || send_all(cl, "\r\n", 2) < 0) break;
		}
		close(cl);
	}
	close(srv);
	return 0;
}
