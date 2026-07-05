// ---------------------------------------------------------------------------
// ava_cam_relay — Source B encoder/relay  (Phase 2, out-of-ava process)
//
// Reads NV21 frames that libcamtap.so (LD_PRELOAD'd into ava) drops into the
// tmpfs seqlock buffer /tmp/camtap.shm, encodes them to H264 with the SoC's
// hardware encoder (CedarX libvencoder.so), and serves the H264 on
// 127.0.0.1:6969 — the SAME socket Source A (video_monitor) uses, so go2rtc and
// the supervisor treat A and B identically.
//
//   ava + libcamtap.so ──NV21──▶ /tmp/camtap.shm ──▶ ava_cam_relay ──H264──▶ :6969 ──▶ go2rtc
//
// Running as a SEPARATE process means a bug here can never crash ava (the
// robot's navigation). It also uses the video engine only while cleaning, when
// nothing else encodes, so there is no HW contention.
//
// Modes:
//   ava_cam_relay            encode + serve on :6969
//   ava_cam_relay --stats    just report the tap (fps/size) — validates
//                            libcamtap without needing the encoder. Use this
//                            first to confirm frames are flowing.
//
// The frame tap + NV21 layout are fully reverse-engineered/verified. The CedarX
// H264 parameter tuning is the one part to confirm on-device (see vencoder.h).
// ---------------------------------------------------------------------------
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "camtap_shm.h"
#include "vencoder.h"

static volatile int g_stop = 0;
static void on_sig(int s){ (void)s; g_stop = 1; }

// ---- open the tmpfs frame buffer written by libcamtap ----------------------
static struct camtap_shm *open_shm(void) {
	int fd = open(CAMTAP_SHM_PATH, O_RDWR);
	if (fd < 0) return NULL;
	void *p = mmap(NULL, sizeof(struct camtap_shm), PROT_READ | PROT_WRITE,
	               MAP_SHARED, fd, 0);
	close(fd);
	return (p == MAP_FAILED) ? NULL : (struct camtap_shm *)p;
}

// ---- consistent read of one frame via the seqlock --------------------------
// Returns 1 and fills out/w/h if a NEW frame (seq/frames advanced) was read.
static int read_frame(struct camtap_shm *shm, uint8_t *out, int cap,
                      int *w, int *h, int *size, uint64_t *last_frames) {
	uint32_t s0 = shm->seq;
	if (s0 & 1u) return 0;                 // writer mid-update
	__sync_synchronize();
	uint64_t fr = shm->frames;
	if (fr == *last_frames) return 0;       // nothing new
	int ww = shm->width, hh = shm->height, sz = shm->size;
	if (sz <= 0 || sz > cap) return 0;
	memcpy(out, shm->data, (size_t)sz);
	__sync_synchronize();
	if (shm->seq != s0) return 0;           // torn read, try again next tick
	*w = ww; *h = hh; *size = sz; *last_frames = fr;
	return 1;
}

// ---- TCP server on :6969 (go2rtc connects here) ----------------------------
static int tcp_listen(int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in a; memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons(port);
	if (bind(fd, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind :6969"); return -1; }
	if (listen(fd, 1) < 0) { perror("listen"); return -1; }
	return fd;
}

// ---- CedarX encoder (dlopen libvencoder.so) --------------------------------
struct enc {
	void *lib, *venc;
	fn_VideoEncCreate            Create;
	fn_VideoEncInit              Init;
	fn_VideoEncSetParameter      SetParam;
	fn_AllocInputBuffer          Alloc;
	fn_GetOneAllocInputBuffer    GetIn;
	fn_FlushCacheAllocInputBuffer Flush;
	fn_AddOneInputBuffer         AddIn;
	fn_VideoEncodeOneFrame       Encode;
	fn_AlreadyUsedInputBuffer    Used;
	fn_ReturnOneAllocInputBuffer RetIn;
	fn_GetOneBitstreamFrame      GetBits;
	fn_FreeOneBitStreamFrame     FreeBits;
	fn_VideoEncUnInit            UnInit;
	fn_VideoEncDestroy           Destroy;
};

static int enc_load(struct enc *e) {
	memset(e, 0, sizeof(*e));
	e->lib = dlopen("libvencoder.so", RTLD_NOW | RTLD_GLOBAL);
	if (!e->lib) { fprintf(stderr, "dlopen libvencoder: %s\n", dlerror()); return -1; }
	// resolve by the library's real export names
	*(void**)&e->Create   = dlsym(e->lib, "VideoEncCreate");
	*(void**)&e->Init     = dlsym(e->lib, "VideoEncInit");
	*(void**)&e->SetParam = dlsym(e->lib, "VideoEncSetParameter");
	*(void**)&e->Alloc    = dlsym(e->lib, "AllocInputBuffer");
	*(void**)&e->GetIn    = dlsym(e->lib, "GetOneAllocInputBuffer");
	*(void**)&e->Flush    = dlsym(e->lib, "FlushCacheAllocInputBuffer");
	*(void**)&e->AddIn    = dlsym(e->lib, "AddOneInputBuffer");
	*(void**)&e->Encode   = dlsym(e->lib, "VideoEncodeOneFrame");
	*(void**)&e->Used     = dlsym(e->lib, "AlreadyUsedInputBuffer");
	*(void**)&e->RetIn    = dlsym(e->lib, "ReturnOneAllocInputBuffer");
	*(void**)&e->GetBits  = dlsym(e->lib, "GetOneBitstreamFrame");
	*(void**)&e->FreeBits = dlsym(e->lib, "FreeOneBitStreamFrame");
	*(void**)&e->UnInit   = dlsym(e->lib, "VideoEncUnInit");
	*(void**)&e->Destroy  = dlsym(e->lib, "VideoEncDestroy");
	if (!e->Create || !e->Init || !e->GetIn || !e->Encode || !e->GetBits) {
		fprintf(stderr, "libvencoder missing required symbols\n"); return -1;
	}
	return 0;
}

static int enc_init(struct enc *e, int w, int h, int fps, int kbps) {
	e->venc = e->Create(VENC_CODEC_H264);
	if (!e->venc) { fprintf(stderr, "VideoEncCreate failed\n"); return -1; }
	int fr = fps, br = kbps * 1000, gop = fps * 2;
	if (e->SetParam) {
		e->SetParam(e->venc, VENC_IndexParamFramerate, &fr);
		e->SetParam(e->venc, VENC_IndexParamBitrate, &br);
		e->SetParam(e->venc, VENC_IndexParamMaxKeyInterval, &gop);
	}
	VencBaseConfig base; memset(&base, 0, sizeof(base));
	base.nInputWidth = base.nDstWidth = (uint32_t)w;
	base.nInputHeight = base.nDstHeight = (uint32_t)h;
	base.nStride = (uint32_t)w;
	base.eInputFormat = VENC_PIXEL_YVU420SP;         // NV21
	if (e->Init(e->venc, &base) != 0) { fprintf(stderr, "VideoEncInit failed\n"); return -1; }
	VencAllocateBufferParam ap; memset(&ap, 0, sizeof(ap));
	ap.nBufferNum = 4; ap.nSizeY = (uint32_t)(w*h); ap.nSizeC = (uint32_t)(w*h/2);
	if (e->Alloc) e->Alloc(e->venc, &ap);
	fprintf(stderr, "encoder ready %dx%d @%dfps %dkbps\n", w, h, fps, kbps);
	return 0;
}

// Encode one NV21 frame; write H264 to client. Returns 0 ok, -1 fatal.
static int enc_frame(struct enc *e, const uint8_t *nv21, int w, int h, int client) {
	VencInputBuffer in; memset(&in, 0, sizeof(in));
	if (e->GetIn(e->venc, &in) != 0) return 0;       // no free buffer this tick
	if (in.pAddrVirY) memcpy(in.pAddrVirY, nv21, (size_t)(w*h));
	if (in.pAddrVirC) memcpy(in.pAddrVirC, nv21 + w*h, (size_t)(w*h/2));
	if (e->Flush) e->Flush(e->venc, &in);
	if (e->AddIn) e->AddIn(e->venc, &in);
	e->Encode(e->venc);
	if (e->Used)  e->Used(e->venc, &in);
	if (e->RetIn) e->RetIn(e->venc, &in);

	VencOutputBuffer out; memset(&out, 0, sizeof(out));
	if (e->GetBits(e->venc, &out) == 0) {
		if (client >= 0) {
			if (out.pData0 && out.nSize0 && write(client, out.pData0, out.nSize0) < 0) return -1;
			if (out.pData1 && out.nSize1 && write(client, out.pData1, out.nSize1) < 0) return -1;
		}
		if (e->FreeBits) e->FreeBits(e->venc, &out);
	}
	return 0;
}

int main(int argc, char **argv) {
	int stats = (argc > 1 && strcmp(argv[1], "--stats") == 0);
	int fps = 15, kbps = 2000;
	signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);

	struct camtap_shm *shm = NULL;
	for (int i = 0; i < 50 && !shm && !g_stop; i++) {   // wait for the tap to appear
		shm = open_shm();
		if (!shm) usleep(200000);
	}
	if (!shm || shm->magic != CAMTAP_MAGIC) {
		fprintf(stderr, "no %s yet (is libcamtap active in ava, and is it cleaning?)\n", CAMTAP_SHM_PATH);
		return 1;
	}
	fprintf(stderr, "attached to %s\n", CAMTAP_SHM_PATH);

	static uint8_t frame[CAMTAP_MAX_FRAME];
	uint64_t last = 0; int w = 0, h = 0, size = 0;

	if (stats) {   // diagnostic: prove the tap works, no encoder
		uint64_t t0 = 0, cnt = 0;
		while (!g_stop) {
			if (read_frame(shm, frame, sizeof frame, &w, &h, &size, &last)) {
				if (!t0) t0 = shm->ts_ns;
				cnt++;
				if (cnt % 15 == 0)
					fprintf(stderr, "tap: %dx%d %d bytes, %llu frames total\n",
					        w, h, size, (unsigned long long)shm->frames);
			}
			usleep(3000);
		}
		return 0;
	}

	struct enc e;
	if (enc_load(&e) != 0) return 1;
	int srv = tcp_listen(6969);
	if (srv < 0) return 1;
	fprintf(stderr, "serving H264 on 127.0.0.1:6969\n");

	int client = -1, inited = 0;
	while (!g_stop) {
		if (client < 0) {
			struct timeval tv = {0, 0}; fd_set r; FD_ZERO(&r); FD_SET(srv, &r);
			if (select(srv+1, &r, NULL, NULL, &tv) > 0) client = accept(srv, NULL, NULL);
		}
		if (read_frame(shm, frame, sizeof frame, &w, &h, &size, &last)) {
			if (!inited) { if (enc_init(&e, w, h, fps, kbps) != 0) break; inited = 1; }
			if (client >= 0 && enc_frame(&e, frame, w, h, client) < 0) { close(client); client = -1; }
		}
		usleep(3000);
	}

	if (client >= 0) close(client);
	close(srv);
	if (inited && e.UnInit) e.UnInit(e.venc);
	if (inited && e.Destroy) e.Destroy(e.venc);
	return 0;
}
