/* SPDX-License-Identifier: MIT */
/* Miyoo Mini Flip port (TortOS plorpos-f1p): SigmaStar SSD202D, no GPU.
 *
 * Video: the core's frame is scaled by the CPU (nearest) into the back page
 * of fb0 (752x560, 32-bit xRGB, three pages) and panned onto glass. No
 * shaders - there is nothing to run them on (ADR-0041's None only).
 *
 * Audio: the vendor's MI_AO, reached with dlopen so the build needs none of
 * the vendor's libraries or headers. ALSA hw:0,0 and OSS /dev/dsp both play
 * at HALF rate on this device - measured and heard (TortOS plorpos-f1p.3) -
 * so SDL audio is not an option; MI_AO with the attributes steward-fu's SDL
 * uses (I2S master, 16-bit stereo, 48 kHz, 512-sample frames) plays true.
 * A writer thread drains a ring the frame loop fills without blocking.
 *
 * Input: "soc:gpio_keys" (event0), a keyboard, in Miyoo's map.
 *
 * G2 scope (TortOS plorpos-f1p.6): enough to measure memory and speed.
 * Levels, mute handover, the lid and MI_GFX scaling are plorpos-f1p.8/.9. */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "diatom_port.h"

#define AUDIO_RATE          48000
#define AUDIO_CHUNK         512     /* frames per MI_AO frame */
#define AUDIO_BUFFER_FRAMES 4096    /* the ring; rate control holds ~half */

/* ---------- MI_AO, ABI-compatible with the vendor's mi_aio_datatype.h ---- */

typedef struct {
	int eFmt, eMclk;
	unsigned char bSyncClock;
} mi_i2s_cfg;
typedef struct {
	int eSamplerate, eBitwidth, eWorkmode, eSoundmode;
	unsigned u32FrmNum, u32PtNumPerFrm, u32CodecChnCnt, u32ChnCnt;
	union { mi_i2s_cfg i2s; } work;
} mi_audio_attr;
typedef struct {
	int eBitwidth, eSoundmode;
	void *apVirAddr[16];
	unsigned long long u64TimeStamp;
	unsigned u32Seq, u32Len, au32PoolId[2];
	void *apSrcPcmVirAddr[16];
	unsigned u32SrcPcmLen;
} mi_audio_frame;

/* MI_GFX, ABI-compatible with the vendor's mi_gfx_datatype.h. */
typedef struct { int x, y; unsigned w, h; } mi_gfx_rect;
typedef struct {
	unsigned long long phy;
	int fmt;                        /* 6 RGB565, 11 ARGB8888 */
	unsigned w, h, stride;
} mi_gfx_surface;
typedef struct {
	unsigned char enable; int op, fmt; unsigned start, end;
} mi_gfx_ckey;
typedef struct {
	mi_gfx_rect clip;
	mi_gfx_ckey src_key, dst_key;
	int src_bld, dst_bld;           /* 1 ONE, 0 ZERO */
	int mirror, rotate;             /* rotate 2 = 180 degrees */
	int blend_flags;
	unsigned src_const, dst_const;
} mi_gfx_opt;
#define MI_GFX_RGB565   6
#define MI_GFX_ARGB8888 11
#define MI_GFX_ROT180   2

static struct {
	int (*sys_init)(void);
	int (*mma_alloc)(unsigned char *heap, unsigned size, unsigned long long *phy);
	int (*mma_free)(unsigned long long phy);
	int (*mmap)(unsigned long long phy, unsigned size, void **virt, unsigned char cache);
	int (*munmap)(void *virt, unsigned size);
	int (*flush)(void *virt, unsigned len);
	int (*gfx_open)(void);
	int (*gfx_blit)(mi_gfx_surface *s, mi_gfx_rect *sr, mi_gfx_surface *d, mi_gfx_rect *dr,
	                mi_gfx_opt *o, unsigned short *fence);
	int (*gfx_wait)(unsigned char all, unsigned short fence);
	int (*set_attr)(int dev, mi_audio_attr *a);
	int (*enable)(int dev);
	int (*disable)(int dev);
	int (*enable_chn)(int dev, int chn);
	int (*disable_chn)(int dev, int chn);
	int (*send)(int dev, int chn, mi_audio_frame *f, int ms);
	int (*set_volume)(int dev, int db);
} mi;

static bool mi_load(void)
{
	/* The vendor's libraries declare none of what they need (no DT_NEEDED):
	 * the CamOs* helpers come first, globally, or libmi_ao.so fails to load
	 * on CamOsGetTimeOfDay (seen on the Flip, 2026-11-04). */
	dlopen("libcam_os_wrapper.so", RTLD_NOW | RTLD_GLOBAL);
	dlopen("libmi_common.so", RTLD_NOW | RTLD_GLOBAL);
	void *sys = dlopen("libmi_sys.so", RTLD_NOW | RTLD_GLOBAL);
	void *ao = sys ? dlopen("libmi_ao.so", RTLD_NOW | RTLD_GLOBAL) : NULL;

	if (!ao) {
		fprintf(stderr, "[warn] audio: no MI_AO (%s)\n", dlerror());
		return false;
	}
	mi.sys_init    = (int (*)(void))dlsym(sys, "MI_SYS_Init");
	mi.set_attr    = (int (*)(int, mi_audio_attr *))dlsym(ao, "MI_AO_SetPubAttr");
	mi.enable      = (int (*)(int))dlsym(ao, "MI_AO_Enable");
	mi.disable     = (int (*)(int))dlsym(ao, "MI_AO_Disable");
	mi.enable_chn  = (int (*)(int, int))dlsym(ao, "MI_AO_EnableChn");
	mi.disable_chn = (int (*)(int, int))dlsym(ao, "MI_AO_DisableChn");
	mi.send        = (int (*)(int, int, mi_audio_frame *, int))dlsym(ao, "MI_AO_SendFrame");
	mi.set_volume  = (int (*)(int, int))dlsym(ao, "MI_AO_SetVolume");
	mi.mma_alloc = (int (*)(unsigned char *, unsigned, unsigned long long *))dlsym(sys, "MI_SYS_MMA_Alloc");
	mi.mma_free  = (int (*)(unsigned long long))dlsym(sys, "MI_SYS_MMA_Free");
	mi.mmap      = (int (*)(unsigned long long, unsigned, void **, unsigned char))dlsym(sys, "MI_SYS_Mmap");
	mi.munmap    = (int (*)(void *, unsigned))dlsym(sys, "MI_SYS_Munmap");
	mi.flush     = (int (*)(void *, unsigned))dlsym(sys, "MI_SYS_FlushInvCache");
	{
		void *gfx = dlopen("libmi_gfx.so", RTLD_NOW | RTLD_GLOBAL);
		if (!gfx) fprintf(stderr, "[warn] video: libmi_gfx.so: %s\n", dlerror());
		if (gfx) {
			mi.gfx_open = (int (*)(void))dlsym(gfx, "MI_GFX_Open");
			mi.gfx_blit = (int (*)(mi_gfx_surface *, mi_gfx_rect *, mi_gfx_surface *,
			                       mi_gfx_rect *, mi_gfx_opt *, unsigned short *))dlsym(gfx, "MI_GFX_BitBlit");
			mi.gfx_wait = (int (*)(unsigned char, unsigned short))dlsym(gfx, "MI_GFX_WaitAllDone");
		}
	}
	return mi.sys_init && mi.set_attr && mi.enable && mi.disable &&
	       mi.enable_chn && mi.disable_chn && mi.send;
}

/* ---------- audio: a ring the frame loop fills, a thread drains ---------- */

static int16_t         g_ring[AUDIO_BUFFER_FRAMES * 2];
static size_t          g_rd, g_wr;          /* in frames, free-running */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_writer;
static volatile bool   g_audio_on, g_stop;
static volatile bool   g_muted;
static volatile bool   g_lid_closed;   /* the hall sensor, polled in input_poll */

static void *writer(void *arg)
{
	static int16_t chunk[AUDIO_CHUNK * 2];
	(void)arg;

	while (!g_stop) {
		size_t have;

		pthread_mutex_lock(&g_lock);
		have = g_wr - g_rd;
		if (have >= AUDIO_CHUNK) {
			for (size_t i = 0; i < AUDIO_CHUNK; i++) {
				size_t at = (g_rd + i) % AUDIO_BUFFER_FRAMES;
				chunk[2 * i]     = g_ring[2 * at];
				chunk[2 * i + 1] = g_ring[2 * at + 1];
			}
			g_rd += AUDIO_CHUNK;
		}
		pthread_mutex_unlock(&g_lock);
		/* Nothing to play: play silence. A starved MI_AO lets the amplifier
		 * pick up the Wi-Fi radio - three pops every couple of seconds, heard
		 * on the Flip (2026-11-04) - which is why Allium runs an audioserver
		 * that does exactly this. MI_AO's own queue paces the loop. */
		if (have < AUDIO_CHUNK || g_muted || g_lid_closed) memset(chunk, 0, sizeof chunk);

		mi_audio_frame f;
		memset(&f, 0, sizeof f);
		f.eBitwidth = 0;            /* 16-bit */
		f.eSoundmode = 1;           /* stereo */
		f.u32Len = sizeof chunk;
		f.apVirAddr[0] = chunk;
		/* Nonzero is "no room yet" (MI_AO_ERR_NOBUF) or worse; either way,
		 * wait a little and try the same frame again. */
		while (!g_stop && mi.send(0, 0, &f, 1) != 0) usleep(2000);
	}
	return NULL;
}

static bool mi_ready(void)
{
	static int loaded = -1;

	if (loaded < 0) loaded = mi_load() && mi.sys_init() == 0;
	return loaded;
}

/* Volume: MI_AO's own gain, in dB, set here because this process owns
 * MI_AO. 0 dB is the hardware's full level - far too loud on the Flip's
 * speaker (heard 2026-11-04) - so the ladder runs -40 dB..0 dB with the
 * bottom rung a mute, and starts quiet until the launcher says otherwise. */
static int g_vol_idx = 8, g_vol_cnt = 21;

static void volume_apply(void)
{
	int db;

	if (!g_audio_on || !mi.set_volume) return;
	db = g_vol_idx <= 0 ? -60 : -40 + 40 * g_vol_idx / (g_vol_cnt - 1);
	mi.set_volume(0, db);
}

static bool audio_open(void)
{
	mi_audio_attr a;

	if (!mi_ready()) return false;
	memset(&a, 0, sizeof a);
	a.eSamplerate = AUDIO_RATE;
	a.eBitwidth = 0;                /* E_MI_AUDIO_BIT_WIDTH_16 */
	a.eWorkmode = 1;                /* E_MI_AUDIO_MODE_I2S_MASTER */
	a.eSoundmode = 1;               /* E_MI_AUDIO_SOUND_MODE_STEREO */
	a.u32FrmNum = 6;
	a.u32PtNumPerFrm = AUDIO_CHUNK;
	a.u32ChnCnt = 2;
	if (mi.set_attr(0, &a) != 0 || mi.enable(0) != 0 || mi.enable_chn(0, 0) != 0) {
		fprintf(stderr, "[warn] audio: MI_AO would not open (busy?)\n");
		return false;
	}
	g_stop = false;
	if (pthread_create(&g_writer, NULL, writer, NULL) != 0) return false;
	g_audio_on = true;
	volume_apply();
	diatom_port_log(DIATOM_LOG_INFO, "audio: MI_AO 48000 Hz stereo, 512-frame chunks");
	return true;
}

static void audio_close(void)
{
	if (!g_audio_on) return;
	g_stop = true;
	pthread_join(g_writer, NULL);
	mi.disable_chn(0, 0);
	mi.disable(0);
	g_audio_on = false;
}

bool diatom_port_audio_set(const char *name, char *actual, size_t cap)
{
	(void)name;
	if (actual && cap) actual[0] = '\0';
	return name == NULL || !*name;      /* one sink: the speaker */
}

void diatom_port_audio_get(char *out, size_t cap) { if (out && cap) out[0] = '\0'; }

/* MI_AO stays open for diatom's whole life, fed silence between games (see
 * writer). Handing it to Muse is TortOS plorpos-f1p.9's design. */
size_t diatom_port_audio_write(const int16_t *frames, size_t n)
{
	size_t room;

	if (!g_audio_on || !frames || !n) return 0;
	pthread_mutex_lock(&g_lock);
	room = AUDIO_BUFFER_FRAMES - (g_wr - g_rd);
	if (n > room) n = room;
	for (size_t i = 0; i < n; i++) {
		size_t at = (g_wr + i) % AUDIO_BUFFER_FRAMES;
		g_ring[2 * at]     = frames[2 * i];
		g_ring[2 * at + 1] = frames[2 * i + 1];
	}
	g_wr += n;
	pthread_mutex_unlock(&g_lock);
	return n;
}

size_t diatom_port_audio_queued(void)
{
	size_t q;

	pthread_mutex_lock(&g_lock);
	q = g_wr - g_rd;
	pthread_mutex_unlock(&g_lock);
	return q;
}

/* ---------- video: fb0, CPU scaling, page flip ---------------------------- */

static int      g_fb = -1;
static uint8_t *g_fbmem;
static struct fb_var_screeninfo g_var;
static struct fb_fix_screeninfo g_fix;
static int      g_page;             /* the page on glass */
static diatom_rect g_last_dst[3];
static uint32_t g_xmap[2048];

static uint32_t *page_px(int page)
{
	return (uint32_t *)(g_fbmem + (size_t)page * g_var.yres * g_fix.line_length);
}

static void clear_page(int page)
{
	memset(page_px(page), 0, (size_t)g_var.yres * g_fix.line_length);
}

static void pan_to(int page)
{
	g_var.yoffset = (unsigned)page * g_var.yres;
	if (ioctl(g_fb, FBIOPAN_DISPLAY, &g_var) != 0)
		ioctl(g_fb, FBIOPUT_VSCREENINFO, &g_var);
	g_page = page;
}

static inline uint32_t rgb565(uint16_t p)
{
	uint32_t r = p >> 11 & 31, g = p >> 5 & 63, b = p & 31;

	/* Top bits repeated into the bottom, so white is 0xFFFFFF, not 0xF8FCF8. */
	return 0xFF000000u | (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
}

static void scale(const void *src, int w, int h, size_t pitch, diatom_pixfmt fmt,
                  diatom_rect d, uint32_t *dst, size_t dpitch_px)
{
	int last_sy = -1;

	if (d.w > (int)(sizeof g_xmap / sizeof g_xmap[0])) d.w = sizeof g_xmap / sizeof g_xmap[0];
	for (int x = 0; x < d.w; x++) g_xmap[x] = (uint32_t)((long)x * w / d.w);
	for (int y = 0; y < d.h; y++) {
		int sy = (int)((long)y * h / d.h);
		/* The panel is mounted upside down: logical (x, y) is glass
		 * (W-1-x, H-1-y). `out` is the logical row's first pixel, and the row
		 * runs DOWN in memory from there. */
		uint32_t *out = dst + (size_t)(g_var.yres - 1 - (unsigned)(d.y + y)) * dpitch_px +
		                (g_var.xres - 1 - (unsigned)d.x);
		uint32_t *lo = out - (d.w - 1);

		if (sy == last_sy) {            /* the row above, again (below in memory) */
			memcpy(lo, lo + dpitch_px, (size_t)d.w * 4);
			continue;
		}
		last_sy = sy;
		if (fmt == DIATOM_PIX_RGB565) {
			const uint16_t *in = (const uint16_t *)((const uint8_t *)src + (size_t)sy * pitch);
			for (int x = 0; x < d.w; x++) out[-x] = rgb565(in[g_xmap[x]]);
		} else {
			const uint32_t *in = (const uint32_t *)((const uint8_t *)src + (size_t)sy * pitch);
			for (int x = 0; x < d.w; x++) out[-x] = in[g_xmap[x]] | 0xFF000000u;
		}
	}
}

/* The overlay: a borrowed BGRA image, alpha-blended over the frame. */
static const uint8_t *g_ov;
static int g_ov_w, g_ov_h;
static uint64_t g_ov_until;

void diatom_port_overlay(const uint8_t *bgra, int w, int h, unsigned ms)
{
	g_ov = NULL;
	if (!bgra || ms == 0 || w <= 0 || h <= 0) return;
	if (w > (int)g_var.xres || h > (int)g_var.yres) {
		diatom_port_log(DIATOM_LOG_WARN, "overlay larger than the panel; ignored");
		return;
	}
	g_ov = bgra; g_ov_w = w; g_ov_h = h;
	g_ov_until = diatom_port_now_us() + (uint64_t)ms * 1000ull;
}

static void draw_overlay(uint32_t *px, size_t pitch_px)
{
	int x0, y0;

	if (!g_ov || diatom_port_now_us() >= g_ov_until) return;
	x0 = ((int)g_var.xres - g_ov_w) / 2;
	y0 = (int)g_var.yres - g_ov_h - (int)g_var.yres / 24;
	for (int y = 0; y < g_ov_h; y++) {
		const uint8_t *s = g_ov + (size_t)y * (size_t)g_ov_w * 4;
		/* Turned 180 degrees, as scale() draws (the panel is upside down). */
		uint32_t *d = px + (size_t)(g_var.yres - 1 - (unsigned)(y0 + y)) * pitch_px +
		              (g_var.xres - 1 - (unsigned)x0);
		for (int x = 0; x < g_ov_w; x++, s += 4) {
			unsigned a = s[3], na = 255 - a, dp = d[-x];
			unsigned r = (s[2] * a + ((dp >> 16) & 255) * na) / 255;
			unsigned g = (s[1] * a + ((dp >> 8) & 255) * na) / 255;
			unsigned b = (s[0] * a + (dp & 255) * na) / 255;
			d[-x] = 0xFF000000u | r << 16 | g << 8 | b;
		}
	}
}

/* The level bar after a Vol or MENU+Vol step (plorpos-f1p.50): 12 logical rows
 * across the top, written straight into the page after the blit - no read-back
 * of fb memory, and nothing at all while hidden but one compare in present.
 * Same tints as the launcher's UI_OSD_VOLUME / UI_OSD_BRIGHT. */
static uint64_t g_bar_until;
static int g_bar_level, g_bar_max = 1;
static bool g_bar_bright;

static void bar_show(bool bright, int level, int max)
{
	g_bar_bright = bright; g_bar_level = level; g_bar_max = max > 0 ? max : 1;
	g_bar_until = diatom_port_now_us() + 1500000ull;
}

static void draw_bar(uint32_t *px, size_t pitch_px)
{
	const uint32_t on = 0xFF000000u | (g_bar_bright ? (255u << 16 | 206u << 8 | 128u) : (61u << 16 | 214u << 8 | 255u));
	const uint32_t off = 0xFF000000u | (60u << 16 | 62u << 8 | 72u), scrim = 0xFF000000u | (24u << 16 | 24u << 8 | 30u);
	const int W = (int)g_var.xres, fill = W * g_bar_level / g_bar_max;

	for (int y = 0; y < 12; y++) {
		/* Turned 180 degrees, as scale() draws (the panel is upside down). */
		uint32_t *row = px + (size_t)(g_var.yres - 1 - (unsigned)y) * pitch_px;
		for (int x = 0; x < W; x++)
			row[W - 1 - x] = y < 3 || y >= 9 ? scrim : x < fill ? on : off;
	}
}

static diatom_rect g_ov_drawn[3];

/* The flip thread: FBIOPAN_DISPLAY waits for vblank here (12.7 ms a frame
 * measured, TortOS plorpos-f1p.14), and waiting in present halved every
 * game to 30 fps. So present hands its page over and returns; this thread
 * pans to it. Three pages: one on glass, one waiting, one being drawn - a
 * newer frame replaces a waiting one that has not flipped yet. */
static pthread_t       g_flip_thr;
static pthread_mutex_t g_flip_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_flip_cond = PTHREAD_COND_INITIALIZER;
static int             g_pending = -1;  /* page waiting for the pan, or -1 */
static int             g_inflight = -1; /* page being panned to, or -1 */
static bool            g_flipping, g_flip_stop;
static int             g_npages;

static void *flipper(void *arg)
{
	uint64_t t0 = diatom_port_now_us();
	unsigned pans = 0;
	(void)arg;

	pthread_mutex_lock(&g_flip_lock);
	while (!g_flip_stop) {
		int page;

		if (g_pending < 0) { pthread_cond_wait(&g_flip_cond, &g_flip_lock); continue; }
		page = g_pending;
		g_pending = -1;
		g_inflight = page;
		g_flipping = true;
		pthread_mutex_unlock(&g_flip_lock);
		pan_to(page);
		pthread_mutex_lock(&g_flip_lock);
		g_flipping = false;
		g_inflight = -1;
		pthread_cond_broadcast(&g_flip_cond);
		if (++pans == 600) {
			uint64_t now = diatom_port_now_us();
			fprintf(stderr, "flip: %.2f pans/s\n", 600 * 1e6 / (double)(now - t0));
			t0 = now; pans = 0;
		}
	}
	pthread_mutex_unlock(&g_flip_lock);
	return NULL;
}

/* A page that is neither on glass nor waiting to go there. */
static int free_page(void)
{
	int p;

	pthread_mutex_lock(&g_flip_lock);
	for (p = 0; p < g_npages; p++)
		if (p != g_page && p != g_pending && p != g_inflight) break;
	pthread_mutex_unlock(&g_flip_lock);
	return p < g_npages ? p : (g_page == 0 ? 1 : 0);
}

static void flip_to(int page)
{
	pthread_mutex_lock(&g_flip_lock);
	g_pending = page;
	pthread_cond_signal(&g_flip_cond);
	pthread_mutex_unlock(&g_flip_lock);
}

/* Until nothing is waiting or panning: the display is about to change hands. */
static void flip_drain(void)
{
	pthread_mutex_lock(&g_flip_lock);
	while (g_pending >= 0 || g_flipping) pthread_cond_wait(&g_flip_cond, &g_flip_lock);
	pthread_mutex_unlock(&g_flip_lock);
}

/* The hardware path: the core's frame copied into MMA memory (physically
 * contiguous, which the 2D engine needs), then one MI_GFX blit scales it,
 * converts it to xRGB and turns it 180 degrees into fb0's back page. G2
 * measured the CPU path at 29.6 ms a frame (TortOS plorpos-f1p.6). */
static bool g_gfx;
static unsigned long long g_src_phy;
static void *g_src_virt;
static unsigned g_src_size;

/* A diatom that is killed (SIGKILL, the OOM killer, a crash, a deploy) cannot free
 * its MI_SYS video buffer, and the kernel does not: the chunk stays "app-mmaAlloc"
 * in the 21 MB heap until reboot (plorpos-f1p.31). So at start, when no other
 * diatom is running and nothing of ours can be live, free what is left. The heap
 * is listed in /proc as offset/length/used/task per chunk under a header row that
 * carries the CPU bus base; MI_SYS_MMA_Free takes the memory controller's
 * address, which is that base less 0x20000000, plus the chunk's offset. */
static void mma_reap_stale(void)
{
	const char *heap = "/proc/mi_modules/mi_sys_mma/mma_heap_name0";
	FILE *f;
	char line[256];
	unsigned long long base = 0, off, len;
	int used, freed = 0;
	char task[64];
	DIR *d = opendir("/proc");
	struct dirent *e;

	if (!mi.mma_free || !d) { if (d) closedir(d); return; }
	while ((e = readdir(d))) {
		char path[64], comm[32] = "";
		FILE *c;
		if (e->d_name[0] < '0' || e->d_name[0] > '9' || atoi(e->d_name) == (int)getpid()) continue;
		snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
		if ((c = fopen(path, "r"))) {
			if (fgets(comm, sizeof comm, c) && !strncmp(comm, "diatom", 6)) { fclose(c); closedir(d); return; }
			fclose(c);
		}
	}
	closedir(d);
	if (!(f = fopen(heap, "r"))) return;
	while (fgets(line, sizeof line, f)) {
		char name[64];
		if (sscanf(line, " %63s %llx", name, &off) == 2 && !strncmp(name, "mma_heap_name", 13)) { base = off; continue; }
		if (base && sscanf(line, " %llx %llx %d %63s", &off, &len, &used, task) == 4 &&
		    used == 1 && !strcmp(task, "app-mmaAlloc")) {
			if (mi.mma_free(base - 0x20000000ull + off) == 0) freed++;
		}
	}
	fclose(f);
	if (freed) fprintf(stderr, "[info] video: freed %d stale MMA chunk(s) of a killed diatom\n", freed);
}

static bool gfx_init(void)
{
	int rc = -1;

	if (!mi_ready() || !mi.gfx_open || !mi.gfx_blit || !mi.gfx_wait || !mi.mma_alloc ||
	    !mi.mmap || !mi.flush || (rc = mi.gfx_open()) != 0) {
		fprintf(stderr, "[warn] video: no MI_GFX (ready %d, open %p, rc %#x); scaling on the CPU\n",
		        mi_ready(), (void *)mi.gfx_open, (unsigned)rc);
		return false;
	}
	mma_reap_stale();
	diatom_port_log(DIATOM_LOG_INFO, "video: MI_GFX blit (scale + 180-degree turn)");
	return true;
}

static bool gfx_src(unsigned size)
{
	if (size <= g_src_size) return true;
	if (g_src_virt) { mi.munmap(g_src_virt, g_src_size); mi.mma_free(g_src_phy); g_src_virt = NULL; g_src_size = 0; }
	size = (size + 65535) & ~65535u;
	if (mi.mma_alloc(NULL, size, &g_src_phy) != 0) return false;
	if (mi.mmap(g_src_phy, size, &g_src_virt, 1) != 0) { mi.mma_free(g_src_phy); return false; }
	g_src_size = size;
	return true;
}

static bool gfx_present(const void *src, int w, int h, size_t pitch, diatom_pixfmt fmt,
                        diatom_rect d, int page)
{
	unsigned bpp = fmt == DIATOM_PIX_RGB565 ? 2 : 4, row = (unsigned)w * bpp;
	mi_gfx_surface ss, ds;
	mi_gfx_rect sr, dr;
	mi_gfx_opt o;
	unsigned short fence = 0;

	if (!gfx_src(row * (unsigned)h)) return false;
	for (int y = 0; y < h; y++)
		memcpy((uint8_t *)g_src_virt + (size_t)y * row, (const uint8_t *)src + (size_t)y * pitch, row);
	mi.flush(g_src_virt, row * (unsigned)h);
	ss = (mi_gfx_surface){ g_src_phy, bpp == 2 ? MI_GFX_RGB565 : MI_GFX_ARGB8888,
	                       (unsigned)w, (unsigned)h, row };
	ds = (mi_gfx_surface){ g_fix.smem_start + (unsigned long long)page * g_var.yres * g_fix.line_length,
	                       MI_GFX_ARGB8888, g_var.xres, g_var.yres, g_fix.line_length };
	sr = (mi_gfx_rect){ 0, 0, (unsigned)w, (unsigned)h };
	/* Where the logical rect lands once turned. */
	dr = (mi_gfx_rect){ (int)g_var.xres - d.x - d.w, (int)g_var.yres - d.y - d.h,
	                    (unsigned)d.w, (unsigned)d.h };
	memset(&o, 0, sizeof o);
	o.clip = (mi_gfx_rect){ 0, 0, g_var.xres, g_var.yres };
	o.src_bld = 1;                  /* ONE */
	o.dst_bld = 0;                  /* ZERO: a copy, no blending */
	o.rotate = MI_GFX_ROT180;
	if (mi.gfx_blit(&ss, &sr, &ds, &dr, &o, &fence) != 0) return false;
	mi.gfx_wait(0, fence);
	return true;
}

void diatom_port_present(const void *src, int w, int h, size_t pitch,
                         diatom_pixfmt fmt, diatom_rect dst, diatom_filter filter)
{
	int back = free_page();
	uint32_t *px;
	size_t pitch_px = g_fix.line_length / 4;
	(void)filter;

	if (!g_fbmem || !src || w <= 0 || h <= 0) return;
	px = page_px(back);
	/* The rect moved, or an overlay was on this page: clear the borders. */
	if (memcmp(&g_last_dst[back], &dst, sizeof dst) != 0 || g_ov_drawn[back].w) {
		clear_page(back);
		g_last_dst[back] = dst;
		g_ov_drawn[back].w = 0;
	}
	uint64_t t0 = diatom_port_now_us(), t1, t2;

	if (!g_gfx || !gfx_present(src, w, h, pitch, fmt, dst, back))
		scale(src, w, h, pitch, fmt, dst, px, pitch_px);
	t1 = diatom_port_now_us();
	if (g_ov && diatom_port_now_us() < g_ov_until) {
		draw_overlay(px, pitch_px);
		g_ov_drawn[back].w = 1;
	}
	if (g_bar_until && t1 < g_bar_until) {
		draw_bar(px, pitch_px);
		g_ov_drawn[back].w = 1;
	}
	flip_to(back);
	t2 = diatom_port_now_us();
	/* Where present's time goes (TortOS plorpos-f1p.14): the blit, and the
	 * pan, which may wait for vblank. */
	{
		static uint64_t blit, pan;
		static unsigned n;

		blit += t1 - t0; pan += t2 - t1;
		if (++n == 300) {
			fprintf(stderr, "present: blit avg %.2f ms, hand-over avg %.2f ms (%s)\n",
			        blit / 300 / 1000.0, pan / 300 / 1000.0, g_gfx ? "MI_GFX" : "CPU");
			blit = pan = 0; n = 0;
		}
	}
}

void diatom_port_present_stop(diatom_park park)
{
	flip_drain();
	pthread_mutex_lock(&g_lock);
	g_rd = g_wr = 0;
	pthread_mutex_unlock(&g_lock);
	if (!g_fbmem) return;
	if (park == DIATOM_PARK_BLANK) clear_page(g_page);
	memset(g_last_dst, 0, sizeof g_last_dst);
}

bool diatom_port_shader_set(const diatom_shader_pass *p, int n, bool final_linear,
                            char *err, size_t cap)
{
	(void)p; (void)final_linear;
	if (n == 0) return true;
	snprintf(err, cap, "this port has no shaders");
	return false;
}

bool diatom_port_grab(uint8_t **rgb, int *w_out, int *h_out)
{
	int w = (int)g_var.xres, h = (int)g_var.yres;
	uint8_t *out;
	const uint32_t *px;

	if (!g_fbmem) return false;
	out = malloc((size_t)w * (size_t)h * 3);
	if (!out) return false;
	px = page_px(g_page);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint32_t p = px[(size_t)(h - 1 - y) * (g_fix.line_length / 4) + (size_t)(w - 1 - x)];
			uint8_t *o = out + ((size_t)y * w + x) * 3;
			o[0] = p >> 16; o[1] = p >> 8; o[2] = p;
		}
	*rgb = out; *w_out = w; *h_out = h;
	return true;
}

/* ---------- init / shutdown ------------------------------------------------ */

static int g_pad = -1;

bool diatom_port_init(diatom_port_caps *out)
{
	g_fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (g_fb < 0 || ioctl(g_fb, FBIOGET_VSCREENINFO, &g_var) != 0 ||
	    ioctl(g_fb, FBIOGET_FSCREENINFO, &g_fix) != 0) {
		fprintf(stderr, "fb0: %s\n", strerror(errno));
		return false;
	}
	if (g_var.bits_per_pixel != 32 || g_var.yres_virtual < 2 * g_var.yres) {
		fprintf(stderr, "fb0: need 32-bit and two pages\n");
		return false;
	}
	g_fbmem = mmap(NULL, g_fix.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, g_fb, 0);
	if (g_fbmem == MAP_FAILED) { g_fbmem = NULL; return false; }
	g_npages = (int)(g_var.yres_virtual / g_var.yres);
	if (g_npages > 3) g_npages = 3;
	g_page = (int)(g_var.yoffset / g_var.yres);
	if (g_page >= g_npages) g_page = 0;
	if (pthread_create(&g_flip_thr, NULL, flipper, NULL) != 0) return false;

	g_gfx = gfx_init();
	g_pad = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (g_pad < 0) fprintf(stderr, "[warn] input: event0: %s\n", strerror(errno));

	/* Not fatal, as on every port: silence beats no emulator. */
	if (!audio_open()) fprintf(stderr, "[warn] audio unavailable; continuing without sound\n");

	out->surface_w = (int)g_var.xres;
	out->surface_h = (int)g_var.yres;
	out->audio_rate = AUDIO_RATE;
	out->audio_buffer_frames = g_audio_on ? AUDIO_BUFFER_FRAMES : 0;
	out->present_blocks = false;
	return true;
}

void diatom_port_shutdown(void)
{
	flip_drain();
	pthread_mutex_lock(&g_flip_lock);
	g_flip_stop = true;
	pthread_cond_signal(&g_flip_cond);
	pthread_mutex_unlock(&g_flip_lock);
	pthread_join(g_flip_thr, NULL);
	/* The MI_SYS contiguous-memory heap is 21 MB and owned by the kernel, not the
	 * process: a buffer never freed here stays "app-mmaAlloc" until reboot. About
	 * a hundred diatom launches filled it, and then every blit took a vblank
	 * longer (5 ms -> 22 ms) until a power cycle (plorpos-f1p.31). */
	if (g_src_virt) { mi.munmap(g_src_virt, g_src_size); mi.mma_free(g_src_phy); g_src_virt = NULL; g_src_size = 0; }
	audio_close();
	if (g_pad >= 0) { close(g_pad); g_pad = -1; }
	if (g_fbmem) { munmap(g_fbmem, g_fix.smem_len); g_fbmem = NULL; }
	if (g_fb >= 0) { close(g_fb); g_fb = -1; }
}

/* ---------- backlight: pwm0, the launcher's ladder ---------------------------- */

/* The same twelve rungs and period as the launcher's apply_brightness (TortOS
 * platform_miyoo.c), so a level handed either way lands on the same duty. */
#define PWM_DUTY   "/sys/class/pwm/pwmchip0/pwm0/duty_cycle"
#define PWM_PERIOD 800
#define BRIGHT_CNT 12
static int g_bright_idx = -1;

static void bright_apply(void)
{
	static const unsigned char ladder[BRIGHT_CNT] = { 2, 4, 8, 16, 32, 48, 72, 96, 128, 160, 192, 255 };
	FILE *f;

	if (g_bright_idx < 0) return;
	if (!(f = fopen(PWM_DUTY, "w"))) return;
	fprintf(f, "%d", ladder[g_bright_idx] * PWM_PERIOD / 255);
	fclose(f);
}

static void bright_step(int step)
{
	int to = (g_bright_idx < 0 ? BRIGHT_CNT / 2 : g_bright_idx) + step;

	if (to < 0) to = 0;
	if (to > BRIGHT_CNT - 1) to = BRIGHT_CNT - 1;
	if (to != g_bright_idx) {
		g_bright_idx = to;
		bright_apply();
		fprintf(stderr, "backlight: %d/%d\n", g_bright_idx, BRIGHT_CNT - 1);
	}
	bar_show(true, g_bright_idx, BRIGHT_CNT - 1);
}

/* ---------- input ----------------------------------------------------------- */

/* The lid, in a game (plorpos-f1p.7; the launcher does the same on the shelf):
 * the hall sensor's level, closed = 0 (1 was read with the lid open). Closed
 * blanks pwm0 and mutes the writer; open writes the duty back. No suspend. */
#define HALL_VALUE "/sys/devices/soc0/soc/soc:hall-mh248/hallvalue"
static void lid_poll(void)
{
	static uint64_t next;
	static char saved[16];
	uint64_t now = diatom_port_now_us();
	char buf[8] = "";
	int fd;
	bool c;
	FILE *f;

	if (now < next) return;
	next = now + 250000;
	if ((fd = open(HALL_VALUE, O_RDONLY | O_CLOEXEC)) < 0) return;
	if (read(fd, buf, sizeof buf - 1) <= 0) { close(fd); return; }
	close(fd);
	c = buf[0] == '0';
	if (c == g_lid_closed) return;
	fprintf(stderr, "lid: %s (hall %c)\n", c ? "closed" : "open", buf[0]);
	if (c) {
		saved[0] = 0;
		if ((f = fopen(PWM_DUTY, "r"))) { if (!fgets(saved, sizeof saved, f)) saved[0] = 0; fclose(f); }
		if ((f = fopen(PWM_DUTY, "w"))) { fputs("0", f); fclose(f); }
	} else if (saved[0] && (f = fopen(PWM_DUTY, "w"))) {
		fputs(saved, f);
		fclose(f);
	}
	g_lid_closed = c;
}

static const struct { int code; int btn; } keymap[] = {
	{ KEY_UP, DIATOM_BTN_UP }, { KEY_DOWN, DIATOM_BTN_DOWN },
	{ KEY_LEFT, DIATOM_BTN_LEFT }, { KEY_RIGHT, DIATOM_BTN_RIGHT },
	{ KEY_SPACE, DIATOM_BTN_A }, { KEY_LEFTCTRL, DIATOM_BTN_B },
	{ KEY_LEFTSHIFT, DIATOM_BTN_X }, { KEY_LEFTALT, DIATOM_BTN_Y },
	{ KEY_E, DIATOM_BTN_L1 }, { KEY_T, DIATOM_BTN_R1 },
	{ KEY_TAB, DIATOM_BTN_L2 }, { KEY_BACKSPACE, DIATOM_BTN_R2 },
	{ KEY_RIGHTCTRL, DIATOM_BTN_SELECT }, { KEY_ENTER, DIATOM_BTN_START },
	{ KEY_ESC, DIATOM_BTN_MENU },
};
static uint32_t g_buttons;

/* The keys as the kernel holds them now, not as events said: while the
 * launcher's in-game menu is up it grabs the pad, so a release made then
 * never reaches this process - and a Menu still "held" paused the game again
 * the moment it resumed, so Quit could never leave it (2026-11-04). The
 * events are drained only so the queue does not fill. */
void diatom_port_input_poll(void)
{
	struct input_event ev;
	unsigned char bits[KEY_MAX / 8 + 1];
	uint32_t s = 0;

	lid_poll();
	while (g_pad >= 0 && read(g_pad, &ev, sizeof ev) == (ssize_t)sizeof ev) { }
	memset(bits, 0, sizeof bits);
	if (g_pad < 0 || ioctl(g_pad, EVIOCGKEY(sizeof bits), bits) < 0) { g_buttons = 0; return; }
	for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
		if (bits[keymap[i].code / 8] >> (keymap[i].code % 8) & 1) s |= DIATOM_BIT(keymap[i].btn);
	g_buttons = s;
	/* The volume keys, here in a game: this process owns MI_AO, and the
	 * launcher does not see them while a game runs (2026-11-04). Under MENU
	 * they are brightness (as on the shelf; plorpos-f1p.50), and LEVEL is
	 * held while one is down so that MENU's release is no menu tap. */
	{
		static bool up_was, dn_was, chord;
		bool up = bits[KEY_VOLUMEUP / 8] >> (KEY_VOLUMEUP % 8) & 1;
		bool dn = bits[KEY_VOLUMEDOWN / 8] >> (KEY_VOLUMEDOWN % 8) & 1;
		bool menu = s & DIATOM_BIT(DIATOM_BTN_MENU);
		int step = (up && !up_was) - (dn && !dn_was);

		up_was = up; dn_was = dn;
		if (step && menu) chord = true;
		if (!up && !dn) chord = false;
		if (chord) g_buttons |= DIATOM_BIT(DIATOM_BTN_LEVEL);
		if (step && menu) {
			bright_step(step);
		} else if (step) {
			int to = g_vol_idx + step;
			if (to < 0) to = 0;
			if (to > g_vol_cnt - 1) to = g_vol_cnt - 1;
			if (to != g_vol_idx) {
				g_vol_idx = to;
				volume_apply();
				fprintf(stderr, "audio: volume %d/%d\n", g_vol_idx, g_vol_cnt - 1);
			}
			bar_show(false, g_vol_idx, g_vol_cnt - 1);
		}
	}
}

uint32_t diatom_port_input_state(void) { return g_buttons; }
bool     diatom_port_should_quit(void) { return false; }

uint64_t diatom_port_now_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000ull;
}

void diatom_port_log(diatom_log_level lvl, const char *msg)
{
	static const char *tag[] = { "debug", "info", "warn", "error" };
	fprintf(stderr, "[%s] %s\n", tag[lvl], msg);
}

bool diatom_port_level_get(diatom_level_kind kind, int *index, int *count)
{
	if (kind == DIATOM_LEVEL_BRIGHTNESS) {
		if (g_bright_idx < 0) return false;   /* nothing handed over yet */
		*index = g_bright_idx;
		*count = BRIGHT_CNT;
		return true;
	}
	if (kind != DIATOM_LEVEL_VOLUME) return false;
	*index = g_vol_idx;
	*count = g_vol_cnt;
	return true;
}
bool diatom_port_level_set(diatom_level_kind kind, int index, int count)
{
	if (kind == DIATOM_LEVEL_BRIGHTNESS) {
		if (count < 2 || index < 0 || index >= count) return false;
		g_bright_idx = (index * (BRIGHT_CNT - 1) + (count - 1) / 2) / (count - 1);
		bright_apply();
		return true;
	}
	if (kind != DIATOM_LEVEL_VOLUME || count < 2 || index < 0 || index >= count) return false;
	g_vol_idx = index;
	g_vol_cnt = count;
	volume_apply();
	return true;
}
void diatom_port_level_invalidate(void) { }

void diatom_port_mute_set(bool on) { g_muted = on; }
bool diatom_port_mute_get(void) { return g_muted; }

bool diatom_port_take_switcher(void) { return false; }
/* New with main's port interface. No touch panel on the Flip (touch_core is a
 * no-op), and no volume-key hold for Muse track skip yet: returns 0 (unverified
 * port decision, plorpos-f1p.19). */
void diatom_port_touch_core(bool on) { (void)on; }
int  diatom_port_take_skip(void) { return 0; }

void diatom_port_split(bool on, diatom_rect top, diatom_rect bot) { (void)on; (void)top; (void)bot; }
bool diatom_port_pointer(int16_t *x, int16_t *y) { (void)x; (void)y; return false; }
void diatom_port_stylus(bool down) { (void)down; }

bool   diatom_port_mic_open(unsigned rate) { (void)rate; return false; }
void   diatom_port_mic_close(void) {}
void   diatom_port_mic_run(bool on) { (void)on; }
size_t diatom_port_mic_read(int16_t *mono, size_t max) { (void)mono; (void)max; return 0; }
