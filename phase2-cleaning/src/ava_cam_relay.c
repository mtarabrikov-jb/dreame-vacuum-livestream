// ---------------------------------------------------------------------------
// ava_cam_relay — Source B for streaming DURING cleaning (Phase 2, SCAFFOLD)
//
// Idea: while the robot cleans, `ava` already opens the camera and publishes
// NV21 frames to its AI-navigation node over an internal nanomsg bus
// (ipc:///tmp/avamsg.socket). Instead of opening the camera a SECOND time (which
// reboots the W10 — see docs/REVERSE_ENGINEERING.md), we passively SUBSCRIBE to
// that bus, pull the frames ava is already producing, encode them to H264 and
// serve them on 127.0.0.1:6969 for go2rtc — exactly the socket Source A uses, so
// the supervisor can hot-swap A<->B without go2rtc noticing.
//
//   ava (camera_streamer)  --NV21-->  [avamsg bus]  --> ava_cam_relay
//                                                          |  encode H264 (CedarX)
//                                                          v
//                                                     TCP :6969  --> go2rtc
//
// WHAT WORKS in this file:
//   * connect to the nanomsg bus, receive loop, CRC16 framing helper
//   * a complete TCP server on :6969 that go2rtc connects to
//   * --dump mode: prints len/type/first-bytes of the first bus messages so you
//     can VERIFY the subscription on-device and identify the camera message.
//
// WHAT IS TODO (the reverse-engineering that still has to be finished):
//   [TODO-1] MSGTYPE_AVA_AI_CAMERA — the numeric raknetmessage type id to filter.
//   [TODO-2] struct ava_cam_hdr    — exact width/height/stride/format/ts offsets.
//   [TODO-3] nv21_to_h264()        — hardware H264 via libvencoder (CedarX).
//
// nanomsg is linked directly against the ROBOT's libnanomsg.so.5 (pulled with
// `make pull-libs`) rather than dlopen'd, so no dlopen@GLIBC_2.34 dependency is
// introduced on newer build hosts. See phase2-cleaning/Makefile.
//
// Build:  see ../Makefile   (cross-compiles to aarch64 with clang)
// Run  :  ./ava_cam_relay [--dump]   (on the robot, while it is CLEANING)
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>

// ---- nanomsg (declared here; linked against the robot's libnanomsg.so.5) ---
#define NN_AF_SP     1
#define NN_PROTO_BUS 7
#define NN_BUS       (NN_PROTO_BUS * 16 + 0)   // 112
#define NN_MSG       ((size_t)-1)
// If the bus turns out to be PUB/SUB rather than BUS, switch to NN_SUB (2*16+1)
// and call nn_setsockopt(s, NN_SUB, NN_SUB_SUBSCRIBE, "", 0) after connect.
extern int  nn_socket(int domain, int protocol);
extern int  nn_connect(int s, const char *addr);
extern int  nn_recv(int s, void *buf, size_t len, int flags);
extern int  nn_freemsg(void *msg);
extern int  nn_close(int s);
extern const char *nn_strerror(int errnum);

// ---- CRC16-Modbus (ava wraps messages with this; used to validate frames) --
static uint16_t crc16_modbus(const uint8_t *d, size_t n) {
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < n; i++) {
		crc ^= d[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
	}
	return crc;
}

// ---- [TODO-1] message type of the AI camera frame --------------------------
// ava wraps every bus message as a "raknetmessage" whose first bytes encode a
// type (getMsgType). camera_streamer publishes the AI frame under a specific
// type id. Find it: run with --dump during cleaning and correlate the big,
// ~15fps messages that appear only while cleaning.
#define MSGTYPE_AVA_AI_CAMERA   0xFFFF   /* TODO: replace with real id */

// ---- [TODO-2] payload header of an ava_ai_camera_msg -----------------------
// Fill these offsets from the capture. NV21 = YUV420SP: a W*H Y plane followed
// by a W*H/2 interleaved VU plane, so payload = W*H*3/2 (+ this header).
struct ava_cam_hdr {
	uint16_t type;      // getMsgType — compare to MSGTYPE_AVA_AI_CAMERA
	// uint32_t width;  // TODO offset
	// uint32_t height; // TODO offset
	// uint32_t stride; // TODO offset
	// uint64_t ts_us;  // TODO offset
	// uint8_t  nv21[];  // TODO: Y plane (w*h) + VU plane (w*h/2)
};

// ---- [TODO-3] NV21 -> H264 via CedarX libvencoder --------------------------
// The robot has libvencoder.so (Allwinner CedarX HW encoder). Bind these and
// feed NV21 in, get an H264 Annex-B NAL out. This is the last missing piece.
//   VideoEncoder *VideoEncCreate(VENC_CODEC_TYPE);   // VENC_CODEC_H264
//   int VideoEncInit(VideoEncoder*, VencBaseConfig*);// nInputYuvFmt=VENC_PIXEL_YVU420SP
//   AllocInputBuffer / FillInputBuffer / VideoEncodeOneFrame / GetOneBitstreamFrame
// Known-good params (vacuumstreamer recorder.cfg): 864x480 @15fps YVU420SP.
static int nv21_to_h264(const uint8_t *nv21, int w, int h,
                        uint8_t *out, int out_cap) {
	(void)nv21; (void)w; (void)h; (void)out; (void)out_cap;
	return -1; // TODO: not implemented yet
}

// ---- TCP server on :6969 (go2rtc connects here) — this part is complete ----
static volatile int g_stop = 0;
static void on_sig(int s){ (void)s; g_stop = 1; }

static int tcp_listen(int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1 only
	a.sin_port = htons(port);
	if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind :6969"); return -1; }
	if (listen(fd, 1) < 0) { perror("listen"); return -1; }
	return fd;
}

int main(int argc, char **argv) {
	int dump = (argc > 1 && strcmp(argv[1], "--dump") == 0);
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);
	signal(SIGPIPE, SIG_IGN);

	int bus = nn_socket(NN_AF_SP, NN_BUS);
	if (bus < 0) { fprintf(stderr, "nn_socket failed\n"); return 1; }
	const char *ep = "ipc:///tmp/avamsg.socket";
	if (nn_connect(bus, ep) < 0) {
		fprintf(stderr, "nn_connect %s failed: %s\n", ep, nn_strerror(errno));
		return 1;
	}
	fprintf(stderr, "connected to %s\n", ep);

	int srv = tcp_listen(6969);
	if (srv < 0) return 1;
	fprintf(stderr, "serving H264 on 127.0.0.1:6969 (waiting for go2rtc)\n");

	int client = -1;
	unsigned long long frames = 0, cam_frames = 0;
	static uint8_t h264[512 * 1024];

	while (!g_stop) {
		// lazily accept go2rtc without blocking the recv loop
		if (client < 0) {
			struct timeval tv = {0, 0};
			fd_set r; FD_ZERO(&r); FD_SET(srv, &r);
			if (select(srv + 1, &r, NULL, NULL, &tv) > 0)
				client = accept(srv, NULL, NULL);
		}

		void *buf = NULL;
		int n = nn_recv(bus, &buf, NN_MSG, 0);
		if (n < 0) { if (errno == EINTR) continue; usleep(2000); continue; }
		frames++;

		const uint8_t *m = (const uint8_t*)buf;
		uint16_t type = (n >= 2) ? (uint16_t)(m[0] | (m[1] << 8)) : 0xFFFF;

		if (dump && frames <= 40) {
			fprintf(stderr, "msg #%llu len=%d type=0x%04x  %02x %02x %02x %02x %02x %02x\n",
			        frames, n, type,
			        n>0?m[0]:0, n>1?m[1]:0, n>2?m[2]:0, n>3?m[3]:0, n>4?m[4]:0, n>5?m[5]:0);
		}

		// [TODO-1] once MSGTYPE_AVA_AI_CAMERA is known, gate on it:
		if (type == MSGTYPE_AVA_AI_CAMERA) {
			cam_frames++;
			// [TODO-2] parse struct ava_cam_hdr -> w,h and NV21 pointer
			// [TODO-3] int len = nv21_to_h264(nv21, w, h, h264, sizeof h264);
			int len = nv21_to_h264(NULL, 0, 0, h264, sizeof h264);
			if (len > 0 && client >= 0) {
				if (write(client, h264, len) < 0) { close(client); client = -1; }
			}
			if (cam_frames % 30 == 0)
				fprintf(stderr, "camera frames seen: %llu\n", cam_frames);
		}

		nn_freemsg(buf);
	}

	fprintf(stderr, "stopping: %llu msgs, %llu camera frames\n", frames, cam_frames);
	if (client >= 0) close(client);
	close(srv);
	nn_close(bus);
	return 0;
}

// Suppress unused-function warning until the CRC check is wired into [TODO-2].
static void (*const _keep_crc)(void) __attribute__((unused)) = (void(*)(void))crc16_modbus;
