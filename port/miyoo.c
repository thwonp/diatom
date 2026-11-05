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

static struct {
	int (*sys_init)(void);
	int (*set_attr)(int dev, mi_audio_attr *a);
	int (*enable)(int dev);
	int (*disable)(int dev);
	int (*enable_chn)(int dev, int chn);
	int (*disable_chn)(int dev, int chn);
	int (*send)(int dev, int chn, mi_audio_frame *f, int ms);
} mi;

static bool mi_load(void)
{
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
		if (have < AUDIO_CHUNK || g_muted) memset(chunk, 0, sizeof chunk);

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

static bool audio_open(void)
{
	mi_audio_attr a;

	static int loaded = -1;

	if (loaded < 0) loaded = mi_load() && mi.sys_init() == 0;
	if (!loaded) return false;
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
static diatom_rect g_last_dst[2];
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

static diatom_rect g_ov_drawn[2];

void diatom_port_present(const void *src, int w, int h, size_t pitch,
                         diatom_pixfmt fmt, diatom_rect dst, diatom_filter filter)
{
	int back = g_page == 0 ? 1 : 0;
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
	scale(src, w, h, pitch, fmt, dst, px, pitch_px);
	if (g_ov && diatom_port_now_us() < g_ov_until) {
		draw_overlay(px, pitch_px);
		g_ov_drawn[back].w = 1;
	}
	pan_to(back);
}

void diatom_port_present_stop(diatom_park park)
{
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
	g_page = (int)(g_var.yoffset / g_var.yres);
	if (g_page > 1) g_page = 0;

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
	audio_close();
	if (g_pad >= 0) { close(g_pad); g_pad = -1; }
	if (g_fbmem) { munmap(g_fbmem, g_fix.smem_len); g_fbmem = NULL; }
	if (g_fb >= 0) { close(g_fb); g_fb = -1; }
}

/* ---------- input ----------------------------------------------------------- */

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

void diatom_port_input_poll(void)
{
	struct input_event ev;

	while (g_pad >= 0 && read(g_pad, &ev, sizeof ev) == (ssize_t)sizeof ev) {
		if (ev.type != EV_KEY || ev.value == 2) continue;
		for (size_t i = 0; i < sizeof keymap / sizeof keymap[0]; i++)
			if (keymap[i].code == ev.code) {
				if (ev.value) g_buttons |= DIATOM_BIT(keymap[i].btn);
				else g_buttons &= ~DIATOM_BIT(keymap[i].btn);
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

/* Levels and the mute handover arrive with plorpos-f1p.8/.9. */
bool diatom_port_level_get(diatom_level_kind kind, int *index, int *count)
{
	(void)kind; (void)index; (void)count;
	return false;
}
bool diatom_port_level_set(diatom_level_kind kind, int index, int count)
{
	(void)kind; (void)index; (void)count;
	return false;
}
void diatom_port_level_invalidate(void) { }

void diatom_port_mute_set(bool on) { g_muted = on; }
bool diatom_port_mute_get(void) { return g_muted; }

bool diatom_port_take_switcher(void) { return false; }

void diatom_port_split(bool on, diatom_rect top, diatom_rect bot) { (void)on; (void)top; (void)bot; }
bool diatom_port_pointer(int16_t *x, int16_t *y) { (void)x; (void)y; return false; }
void diatom_port_stylus(bool down) { (void)down; }

bool   diatom_port_mic_open(unsigned rate) { (void)rate; return false; }
void   diatom_port_mic_close(void) {}
void   diatom_port_mic_run(bool on) { (void)on; }
size_t diatom_port_mic_read(int16_t *mono, size_t max) { (void)mono; (void)max; return 0; }
