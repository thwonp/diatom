/* GKD 350H Ultra (RK3576S) port, on vendor ROCKNIX.
 *
 * Started from port/desktop.c rather than port/brick.c: ROCKNIX runs a sway
 * session, so the display path is an SDL2 Wayland window like any desktop,
 * not raw fbdev. SDL2 is the device's own 2.32.6 (a GLES-only build) via
 * sysroot/gkd - tools/fetch-gkd-sysroot.sh. Audio is SDL's pulseaudio driver
 * talking to PipeWire.
 *
 * Input is read from evdev directly, NOT through SDL's joystick layer:
 *
 *   - the map below is then the codes measured on the device, with no SDL
 *     index derivation in between (the Brick's table had to be verified
 *     press by press for exactly that reason);
 *   - the volume keys live on a separate device (gpio-keys), which under
 *     Wayland SDL would only see as keyboard events while focused.
 *
 * Needs the session's environment to run standalone:
 *
 *   XDG_RUNTIME_DIR=/var/run/0-runtime-dir WAYLAND_DISPLAY=wayland-1 \
 *     ./diatom --core X.so --rom game
 *
 * This file does not include libretro.h and must never need to.
 */
#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <spawn.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include "diatom_port.h"
#include "port_clock.h"

#define AUDIO_RATE 48000
/* Capacity in FRAMES (one frame = two int16 samples). 4096 at 48kHz is ~85ms,
 * with rate control aiming to hold it near half that. */
#define AUDIO_BUFFER_FRAMES 4096
#define AUDIO_FRAME_BYTES   (2 * (int)sizeof(int16_t))

static SDL_Window   *g_window;
static SDL_Renderer *g_renderer;
static SDL_Texture  *g_texture;
static int           g_tex_w, g_tex_h;
static Uint32        g_tex_fmt;
static diatom_filter g_tex_filter;
static SDL_AudioDeviceID g_audio;
static bool          g_quit;
static uint32_t      g_buttons;
static int           g_pad_fd = -1, g_keys_fd = -1;
/* Home held, from the pad: it turns the volume keys into brightness keys. */
static bool          g_home;

static int evdev_open(const char *want);
static bool g_hidden;
static void draw_osd(void);
static void levels_init(void);

/* ---------- the audio sink (ADR-0029) ------------------------------------- */

/* Empty means the default device. */
static char g_audio_dev[128];

/* Closing an SDL audio device JOINS its audio thread, and that thread can be
 * blocked writing to a sink that has stopped draining. So this call can fail to
 * return, and on 2026-09-05 a stalled A2DP sink did exactly that.
 *
 * The obvious fix - close on a detached thread - was tried and REVERTED the
 * same day. SDL is not safe against opening a device while a close of another
 * is in flight: test/port_test.c fails three ways with it, the first being that
 * the fallback open after a refused device stops working. A frontend that
 * cannot reopen its audio is worse than one that can hang on a dead headset.
 *
 * The hang is also not really here. The frame loop calls SDL_QueueAudio and
 * SDL_GetQueuedAudioSize every frame, and those take the same device lock the
 * audio thread holds while it writes - so a stalled sink stops the loop whether
 * or not anything closes. Fixing that means not letting the write stall, which
 * is the sink question, not the teardown question.
 */
static bool audio_open(const char *name)
{
	SDL_AudioSpec want, have;

	if (g_audio) { SDL_CloseAudioDevice(g_audio); g_audio = 0; }

	SDL_memset(&want, 0, sizeof want);
	want.freq     = AUDIO_RATE;
	want.format   = AUDIO_S16SYS;
	want.channels = 2;
	want.samples  = 1024;

	if (SDL_WasInit(SDL_INIT_AUDIO) == 0 && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
		return false;
	/* allowed_changes 0: SDL converts behind this spec, so caps.audio_rate
	 * stays true across a reopen. */
	g_audio = SDL_OpenAudioDevice(name && *name ? name : NULL, 0,
	                              &want, &have, 0);
	if (!g_audio) return false;
	SDL_PauseAudioDevice(g_audio, 0);
	/* What SDL actually negotiated, not what was asked for. A device can open
	 * cleanly and then carry no sound - that is exactly what a bluealsa sink
	 * did on 2026-09-05 - and without this there is no way to tell from
	 * outside whether SDL agreed to something the device cannot service. */
	{
		char msg[192];

		snprintf(msg, sizeof msg,
		         "audio: opened %s at %d Hz, %d ch, %d frames/period, %u byte buffer",
		         name && *name ? name : "the default device",
		         have.freq, have.channels, have.samples, have.size);
		diatom_port_log(DIATOM_LOG_INFO, msg);
	}
	return true;
}

bool diatom_port_audio_set(const char *name, char *actual, size_t cap)
{
	const char *want = name ? name : "";
	bool ok;

	/* Already there. A write on the state plane is idempotent, so re-stating
	 * the current device must not close and reopen a working sink: that drops
	 * whatever is queued and costs a fresh A2DP stream setup. Repeated writes
	 * are not hypothetical - a launcher that recomputes its routing on a timer
	 * sends one whenever it thinks the answer might have moved. */
	if (g_audio && !strcmp(want, g_audio_dev)) {
		diatom_port_audio_get(actual, cap);
		return true;
	}

	ok = audio_open(name);

	if (!ok) {
		char msg[192];

		snprintf(msg, sizeof msg, "audio: %s unavailable (%s); using the default",
		         name && *name ? name : "the default device", SDL_GetError());
		diatom_port_log(DIATOM_LOG_WARN, msg);
		if (audio_open(NULL)) g_audio_dev[0] = '\0';
	} else {
		snprintf(g_audio_dev, sizeof g_audio_dev, "%s", name ? name : "");
	}
	diatom_port_audio_get(actual, cap);
	return ok;
}

void diatom_port_audio_get(char *out, size_t cap)
{
	if (out && cap) snprintf(out, cap, "%s", g_audio_dev);
}

bool diatom_port_init(diatom_port_caps *out)
{
	int sw = 0, sh = 0;

	/* Audio is initialised SEPARATELY and is allowed to fail. A subsystem that
	 * will not come up must not take the rest of the port with it - see the
	 * open below for why silence beats refusing to start. Rolled into one
	 * SDL_Init, a missing or busy audio device is indistinguishable from a
	 * missing display and kills both. */
	/* Without this the Mali driver hands sway AFBC-compressed buffers and the
	 * window composites as solid black while the game runs and plays sound
	 * (seen 2026-09-29). ROCKNIX sets it for its own frontends; set here so
	 * the port does not depend on its launcher's environment. Not overwritten
	 * if the caller chose otherwise. */
	setenv("MALI_WAYLAND_AFBC", "0", 0);

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return false;
	}

	/* FULLSCREEN_DESKTOP at the output's own size. Asking for 0x0 instead
	 * leaves SDL's Wayland backend at a 1x1 window that fullscreen never
	 * grows (seen 2026-09-29). The size is still read back below rather than
	 * assumed: sway rotates the portrait panel (transform 270) to 1600x1440. */
	{
		SDL_DisplayMode dm = { 0 };

		if (SDL_GetDesktopDisplayMode(0, &dm) != 0) { dm.w = 0; dm.h = 0; }
		g_window = SDL_CreateWindow("diatom",
			SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			dm.w, dm.h, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN);
	}
	if (!g_window) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return false; }

	g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED);
	if (!g_renderer) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); return false; }
	SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
	SDL_ShowCursor(SDL_DISABLE);

	/* Wayland sizes a fullscreen window in the compositor's first configure,
	 * which arrives after SDL_CreateWindow returns: read straight away, the
	 * output can still read small. So pump until sway has answered, bounded
	 * so a compositor that never does fails visibly instead of hanging. */
	for (int i = 0; i < 100; i++) {
		SDL_PumpEvents();
		SDL_GetRendererOutputSize(g_renderer, &sw, &sh);
		if (sw > 1 && sh > 1) break;
		SDL_Delay(10);
	}
	{
		char msg[64];
		snprintf(msg, sizeof msg, "display: surface %dx%d", sw, sh);
		diatom_port_log(DIATOM_LOG_INFO, msg);
	}

	g_pad_fd = evdev_open("gkd_atom_joypad");
	g_keys_fd = evdev_open("gpio-keys");
	if (g_pad_fd < 0)
		diatom_port_log(DIATOM_LOG_WARN, "input: gkd_atom_joypad not found; no buttons");
	levels_init();

	/* Vsync OFF, deliberately.
	 *
	 * No console runs at the panel's rate - measured 50.0070, 59.7275, 59.8200,
	 * 60.0000 across six cores. Blocking on a 60Hz vblank while trying to hold
	 * 59.7275 leaves 0.07ms of slack per frame, so any jitter misses a vblank
	 * and costs a whole 16.67ms. That measured as a consistent 1.4% deficit.
	 *
	 * The host paces against a monotonic clock instead, and audio drift is
	 * absorbed by rate control rather than by hoping the panel agrees. */
	if (SDL_RenderSetVSync(g_renderer, 0) != 0)
		fprintf(stderr, "note: could not disable vsync: %s\n", SDL_GetError());

	/* NOT fatal. A port that cannot open a sound device can still put pixels
	 * where the device wants them, and refusing to start costs far more than
	 * silence does: Diatom is resident, so a failed open here leaves the
	 * launcher with no emulator at all and every launch then pays the cold
	 * cost (~1100 ms) instead of the warm one (~15 ms). That is the worst
	 * outcome available and it happened by accident - see ADR-0029, which
	 * needs this to be true before a sink can be chosen at runtime, since
	 * anything settable while a game runs can fail while a game runs.
	 *
	 * capacity 0 is how the silence is reported rather than hidden: it turns
	 * off dynamic rate control, which would otherwise read a permanently empty
	 * queue as a permanent deficit and hold the resampler at its deviation
	 * limit forever, correcting for a buffer that does not exist. */
	if (!audio_open(NULL))
		fprintf(stderr, "audio unavailable (%s); continuing without sound\n",
		        SDL_GetError());

	out->surface_w          = sw;
	out->surface_h          = sh;
	/* `have` is untouched when the open failed, so the rate comes from what
	 * was asked for: the resampler still needs a target to convert into,
	 * and a zero here would divide. */
	out->audio_rate         = AUDIO_RATE;
	out->audio_buffer_frames = g_audio ? AUDIO_BUFFER_FRAMES : 0;
	out->present_blocks     = false;
	return true;
}

void diatom_port_shutdown(void)
{
	if (g_audio)    SDL_CloseAudioDevice(g_audio);
	if (g_texture)  SDL_DestroyTexture(g_texture);
	if (g_renderer) SDL_DestroyRenderer(g_renderer);
	if (g_window)   SDL_DestroyWindow(g_window);
	if (g_pad_fd >= 0)  close(g_pad_fd);
	if (g_keys_fd >= 0) close(g_keys_fd);
	SDL_Quit();
}

static bool ensure_texture(int w, int h, diatom_pixfmt fmt, diatom_filter filter)
{
	/* RGB888 is SDL's name for XRGB8888: no alpha. ARGB8888 would read the
	 * core's don't-care top byte (0 from mGBA) as alpha, and on a Wayland
	 * surface a zero alpha is a transparent window - black on this device. */
	Uint32 sdlfmt = (fmt == DIATOM_PIX_RGB565)
	              ? SDL_PIXELFORMAT_RGB565
	              : SDL_PIXELFORMAT_RGB888;
	/* SDL has no sharp-bilinear, so this is an APPROXIMATION: linear blends
	 * across the whole source pixel where sharp-bilinear blends across one
	 * destination pixel, which reads as blurrier than the device will look.
	 * Good enough to check the geometry is right on desktop; judgment about
	 * how a filter actually looks belongs on the panel. */
	SDL_ScaleMode mode = (filter == DIATOM_FILTER_SHARP)
	                   ? SDL_ScaleModeLinear
	                   : SDL_ScaleModeNearest;

	if (g_texture && g_tex_w == w && g_tex_h == h && g_tex_fmt == sdlfmt) {
		if (filter != g_tex_filter) {
			SDL_SetTextureScaleMode(g_texture, mode);
			g_tex_filter = filter;
		}
		return true;
	}

	if (g_texture) SDL_DestroyTexture(g_texture);
	g_texture = SDL_CreateTexture(g_renderer, sdlfmt,
	                              SDL_TEXTUREACCESS_STREAMING, w, h);
	if (!g_texture) {
		fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
		return false;
	}
	SDL_SetTextureScaleMode(g_texture, mode);
	g_tex_w = w; g_tex_h = h; g_tex_fmt = sdlfmt; g_tex_filter = filter;
	{
		char msg[96];
		snprintf(msg, sizeof msg, "display: texture %dx%d %s", w, h,
		         SDL_GetPixelFormatName(sdlfmt));
		diatom_port_log(DIATOM_LOG_INFO, msg);
	}
	return true;
}

/* The overlay from diatom_port_overlay: a borrowed pointer, not a copy. The
 * texture is rebuilt only when the image changes, which for a notice on a
 * timer is once. */
static const uint8_t *g_ov;
static int            g_ov_w, g_ov_h;
static uint64_t       g_ov_until;
static SDL_Texture   *g_ov_tex;

void diatom_port_overlay(const uint8_t *bgra, int w, int h, unsigned ms)
{
	int sw = 0, sh = 0;

	if (g_ov_tex) { SDL_DestroyTexture(g_ov_tex); g_ov_tex = NULL; }
	g_ov = NULL;
	g_ov_until = 0;
	if (!bgra || ms == 0 || w <= 0 || h <= 0) return;

	SDL_GetRendererOutputSize(g_renderer, &sw, &sh);
	/* Refused, not clipped - see diatom_port.h. */
	if ((sw && w > sw) || (sh && h > sh)) {
		diatom_port_log(DIATOM_LOG_WARN, "overlay larger than the window; ignored");
		return;
	}

	g_ov_tex = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_ARGB8888,
	                             SDL_TEXTUREACCESS_STATIC, w, h);
	if (!g_ov_tex) return;
	SDL_SetTextureBlendMode(g_ov_tex, SDL_BLENDMODE_BLEND);
	SDL_UpdateTexture(g_ov_tex, NULL, bgra, w * 4);
	g_ov = bgra;
	g_ov_w = w;
	g_ov_h = h;
	g_ov_until = diatom_port_now_us() + (uint64_t)ms * 1000ull;
}

static void draw_overlay(void)
{
	SDL_Rect r;
	int sw = 0, sh = 0;

	if (!g_ov_tex || diatom_port_now_us() >= g_ov_until) return;
	SDL_GetRendererOutputSize(g_renderer, &sw, &sh);
	r.w = g_ov_w;
	r.h = g_ov_h;
	r.x = (sw - g_ov_w) / 2;
	r.y = sh - g_ov_h - sh / 24;
	SDL_RenderCopy(g_renderer, g_ov_tex, NULL, &r);
}

void diatom_port_present(const void *src, int w, int h, size_t pitch,
                         diatom_pixfmt fmt, diatom_rect dst,
                         diatom_filter filter)
{
	SDL_Rect r;

	/* Back from a handover (present_stop): map again before drawing. */
	if (g_hidden) {
		SDL_ShowWindow(g_window);
		g_hidden = false;
	}
	if (w > 0 && h > 0 && !ensure_texture(w, h, fmt, filter)) return;
	if (src && g_texture) SDL_UpdateTexture(g_texture, NULL, src, (int)pitch);

	SDL_RenderClear(g_renderer);
	if (g_texture) {
		r.x = dst.x; r.y = dst.y; r.w = dst.w; r.h = dst.h;
		SDL_RenderCopy(g_renderer, g_texture, NULL, &r);
	}
	draw_overlay();
	draw_osd();
	SDL_RenderPresent(g_renderer);
}

size_t diatom_port_audio_write(const int16_t *frames, size_t n)
{
	size_t queued, room;

	/* Never blocks; drops on overflow. A blocking write would pace the whole
	 * program off the audio clock, which rules out dynamic rate control.
	 *
	 * Take only what fits. The previous test was `queued >= capacity`, which
	 * refuses only an ALREADY FULL queue and then admits a whole batch: at 4095
	 * queued a 832-frame batch landed entire, and the queue reached a measured
	 * **4927 against a stated 4096**. That mattered beyond the extra latency,
	 * because rate control clamps its error term at +1.0 - reached at exactly
	 * capacity - so everything above capacity was invisible to the controller
	 * that was supposed to be preventing it.
	 *
	 * Clamping rather than refusing the batch outright loses less: the frames
	 * that do not fit are dropped either way, and there is no reason to discard
	 * the ones that would have. */
	/* A sink can die under the port - a headset switched off, or carried out
	 * of range. SDL reports the device STOPPED; falling back happens here
	 * rather than in a poll of its own because this already runs every frame,
	 * and the host finds out through diatom_port_audio_get, which is the only
	 * way it can (ADR-0007: the port cannot announce anything).
	 *
	 * Only when a name was asked for. If the DEFAULT device dies there is
	 * nowhere better to go, and retrying it every frame would be a reopen
	 * storm in place of silence. */
	if (g_audio && g_audio_dev[0] &&
	    SDL_GetAudioDeviceStatus(g_audio) == SDL_AUDIO_STOPPED) {
		diatom_port_log(DIATOM_LOG_WARN,
		                "audio: the sink went away; falling back to the default");
		g_audio_dev[0] = '\0';
		audio_open(NULL);
	}
	if (!g_audio || !frames || !n) return 0;
	queued = diatom_port_audio_queued();
	room   = queued >= (size_t)AUDIO_BUFFER_FRAMES
	       ? 0 : (size_t)AUDIO_BUFFER_FRAMES - queued;
	if (n > room) n = room;
	if (!n) return 0;

	SDL_QueueAudio(g_audio, frames, (Uint32)(n * AUDIO_FRAME_BYTES));
	return n;
}

size_t diatom_port_audio_queued(void)
{
	if (!g_audio) return 0;
	return SDL_GetQueuedAudioSize(g_audio) / AUDIO_FRAME_BYTES;
}

/* Measured 2026-09-29: every cap pressed once in a known order under evtest,
 * vendor ROCKNIX, kernel 6.1.118. X and Y are positional here (top = X =
 * NORTH), unlike the Brick, where they are crossed. The dpad is four real
 * buttons, not a hat. Menu is BTN_MODE, the Brick's MENU code too.
 *
 * Unmapped on purpose: Home (BTN_TRIGGER_HAPPY1, 704) is the hotkey and
 * brightness modifier, not a game input; the stick click (BTN_THUMBL) has
 * no shipped core that wants L3. */
static const struct { int code; int btn; } padmap[] = {
	{ BTN_EAST,       DIATOM_BTN_A      },
	{ BTN_SOUTH,      DIATOM_BTN_B      },
	{ BTN_NORTH,      DIATOM_BTN_X      },
	{ BTN_WEST,       DIATOM_BTN_Y      },
	{ BTN_DPAD_UP,    DIATOM_BTN_UP     },
	{ BTN_DPAD_DOWN,  DIATOM_BTN_DOWN   },
	{ BTN_DPAD_LEFT,  DIATOM_BTN_LEFT   },
	{ BTN_DPAD_RIGHT, DIATOM_BTN_RIGHT  },
	{ BTN_TL,         DIATOM_BTN_L1     },
	{ BTN_TR,         DIATOM_BTN_R1     },
	{ BTN_TL2,        DIATOM_BTN_L2     },
	{ BTN_TR2,        DIATOM_BTN_R2     },
	{ BTN_SELECT,     DIATOM_BTN_SELECT },
	{ BTN_START,      DIATOM_BTN_START  },
	{ BTN_MODE,       DIATOM_BTN_MENU   },
};

/* The stick reads as the dpad (as on the Brick Pro). Range -900..899, right
 * = +X, up = -Y; half travel is the threshold, well clear of the 32 flat. */
#define STICK_THRESHOLD 450

static uint32_t g_pad_buttons;
static int g_stick_x, g_stick_y;

/* Found by name, not by event number: the numbering is probe order, and a
 * Bluetooth pad connecting first would shift it. */
static int evdev_open(const char *want)
{
	char path[32], name[64];
	int i, fd;

	for (i = 0; i < 32; i++) {
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0) continue;
		if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 && !strcmp(name, want))
			return fd;
		close(fd);
	}
	return -1;
}

static void pad_read(void)
{
	struct input_event ev[32];
	ssize_t n;
	size_t i, k;

	while ((n = read(g_pad_fd, ev, sizeof ev)) > 0) {
		for (i = 0; i < (size_t)n / sizeof ev[0]; i++) {
			if (ev[i].type == EV_ABS) {
				if (ev[i].code == ABS_X) g_stick_x = ev[i].value;
				if (ev[i].code == ABS_Y) g_stick_y = ev[i].value;
				continue;
			}
			if (ev[i].type != EV_KEY || ev[i].value == 2) continue;
			if (ev[i].code == BTN_TRIGGER_HAPPY1) g_home = ev[i].value;
			for (k = 0; k < sizeof padmap / sizeof padmap[0]; k++) {
				if (padmap[k].code != ev[i].code) continue;
				if (ev[i].value) g_pad_buttons |=  DIATOM_BIT(padmap[k].btn);
				else             g_pad_buttons &= ~DIATOM_BIT(padmap[k].btn);
			}
		}
	}
}

static void gain_nudge(int dir);
static void bright_nudge(int dir);

/* The volume keys are the port's and stop here - never reported upward, never
 * a core's - because nothing else is watching them while a game runs. Presses
 * only; a held key does not repeat, as on the Brick. */
static void keys_read(void)
{
	struct input_event ev[16];
	ssize_t n;
	size_t i;

	while ((n = read(g_keys_fd, ev, sizeof ev)) > 0) {
		for (i = 0; i < (size_t)n / sizeof ev[0]; i++) {
			int dir;

			if (ev[i].type != EV_KEY || ev[i].value != 1) continue;
			if      (ev[i].code == KEY_VOLUMEUP)   dir = +1;
			else if (ev[i].code == KEY_VOLUMEDOWN) dir = -1;
			else continue;
			if (g_home) bright_nudge(dir);
			else        gain_nudge(dir);
		}
	}
}

void diatom_port_input_poll(void)
{
	SDL_Event ev;
	uint32_t s;

	while (SDL_PollEvent(&ev))
		if (ev.type == SDL_QUIT) g_quit = true;

	if (g_pad_fd >= 0)  pad_read();
	if (g_keys_fd >= 0) keys_read();

	s = g_pad_buttons;
	if (g_stick_x >=  STICK_THRESHOLD) s |= DIATOM_BIT(DIATOM_BTN_RIGHT);
	if (g_stick_x <= -STICK_THRESHOLD) s |= DIATOM_BIT(DIATOM_BTN_LEFT);
	if (g_stick_y >=  STICK_THRESHOLD) s |= DIATOM_BIT(DIATOM_BTN_DOWN);
	if (g_stick_y <= -STICK_THRESHOLD) s |= DIATOM_BIT(DIATOM_BTN_UP);
	g_buttons = s;
}

uint32_t diatom_port_input_state(void) { return g_buttons; }
bool     diatom_port_should_quit(void) { return g_quit; }

bool diatom_port_capture(const char *path)
{
	SDL_Surface *s;
	int w, h;
	bool ok;

	if (!g_renderer || !path) return false;
	SDL_GetRendererOutputSize(g_renderer, &w, &h);

	s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!s) return false;

	/* Read back what was actually presented, so the capture shows the real
	 * scaling and letterboxing rather than the core's raw framebuffer. */
	if (SDL_RenderReadPixels(g_renderer, NULL, SDL_PIXELFORMAT_ARGB8888,
	                         s->pixels, s->pitch) != 0) {
		SDL_FreeSurface(s);
		return false;
	}
	/* Save as plain 24-bit RGB. A 32-bit BMP carries a V4/V5 header with alpha
	 * masks that several readers - macOS ImageIO among them - refuse, and the
	 * alpha channel is meaningless here anyway. */
	{
		SDL_Surface *rgb = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_RGB24, 0);
		SDL_FreeSurface(s);
		if (!rgb) return false;
		ok = SDL_SaveBMP(rgb, path) == 0;
		SDL_FreeSurface(rgb);
	}
	return ok;
}

uint64_t diatom_port_now_us(void)
{
	return diatom_ticks_to_us(SDL_GetPerformanceCounter(),
	                          SDL_GetPerformanceFrequency());
}

void diatom_port_log(diatom_log_level lvl, const char *msg)
{
	static const char *tag[] = { "debug", "info", "warn", "error" };
	fprintf(stderr, "[%s] %s\n", tag[lvl], msg);
}

/* ---------- levels: volume and brightness (ADR-0020) ---------------------- */

/* Volume is the default PipeWire sink's, in the same 5% steps ROCKNIX's own
 * /usr/bin/volume uses and the Brick's GAIN_LEVELS: 21 positions. The ALSA
 * DAC stays at 100% underneath (design: PipeWire owns the level). */
#define GAIN_LEVELS 20

/* NOT the Brick's ratio ladder. This backlight reports scale=non-linear: the
 * driver already curves raw values toward perceptual, so the Brick's ladder
 * put five of its twelve rungs where they could not be told apart. Measured
 * 2026-09-29 with a test ladder stepped by hand: raw 24, 32 and 40 look
 * identical, 48 is the first visibly brighter, and every 8-unit step above it
 * is visible. So: evenly spaced from the floor (40) to 255, keeping the
 * Brick's twelve positions. The launcher's GKD ladder must match (gkd.3). */
static const unsigned char bright_ladder[] = {
	40, 60, 79, 99, 118, 138, 157, 177, 196, 216, 235, 255
};
#define BRIGHT_LEVELS ((int)(sizeof bright_ladder / sizeof bright_ladder[0]) - 1)

static int  g_level  = -1;      /* 0..GAIN_LEVELS, -1 unread */
static int  g_bright = -1;      /* index into bright_ladder, -1 unread */
static char g_bl_path[300];     /* .../brightness, empty if none */

/* wpctl is a process, ~tens of ms: far too slow for the frame loop, so sets go
 * to a worker. Latest wins - a burst of presses is one write of where they
 * ended, not a queue of stale ones. */
static pthread_t       g_vol_thread;
static pthread_mutex_t g_vol_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_vol_cv = PTHREAD_COND_INITIALIZER;
static int             g_vol_want = -1;

static void run_wait(char *const argv[])
{
	extern char **environ;
	pid_t pid;
	int st;

	if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) == 0)
		waitpid(pid, &st, 0);
}

static void *vol_worker(void *arg)
{
	(void)arg;
	for (;;) {
		char pct[16];
		char *argv[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", pct, NULL };
		int want;

		pthread_mutex_lock(&g_vol_mu);
		while (g_vol_want < 0) pthread_cond_wait(&g_vol_cv, &g_vol_mu);
		want = g_vol_want;
		g_vol_want = -1;
		pthread_mutex_unlock(&g_vol_mu);

		snprintf(pct, sizeof pct, "%d%%", want * 100 / GAIN_LEVELS);
		run_wait(argv);
	}
	return NULL;
}

/* Read synchronously, but only at start and after a handover (invalidate),
 * never per frame: the one wpctl run lands where a menu is already up. */
static void gain_ensure(void)
{
	FILE *f;
	float v;

	if (g_level >= 0) return;
	f = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
	if (!f) return;
	if (fscanf(f, "Volume: %f", &v) == 1) {
		int lv = (int)(v * GAIN_LEVELS + 0.5f);
		g_level = lv < 0 ? 0 : lv > GAIN_LEVELS ? GAIN_LEVELS : lv;
	}
	pclose(f);
}

static void gain_apply(void)
{
	pthread_mutex_lock(&g_vol_mu);
	g_vol_want = g_level;
	pthread_cond_signal(&g_vol_cv);
	pthread_mutex_unlock(&g_vol_mu);
}

static void bright_ensure(void)
{
	FILE *f;
	int raw, i;

	if (g_bright >= 0 || !g_bl_path[0]) return;
	f = fopen(g_bl_path, "r");
	if (!f) return;
	if (fscanf(f, "%d", &raw) == 1) {
		/* Nearest rung, so the first press steps from where the launcher
		 * left the panel rather than jumping. */
		g_bright = 0;
		for (i = 1; i <= BRIGHT_LEVELS; i++)
			if (abs(bright_ladder[i] - raw) < abs(bright_ladder[g_bright] - raw))
				g_bright = i;
	}
	fclose(f);
}

static void bright_apply(void)
{
	FILE *f = fopen(g_bl_path, "w");

	if (!f) return;
	fprintf(f, "%d\n", bright_ladder[g_bright]);
	fclose(f);
}

/* The bar: shown 1.5s from the last press, so holding the keys keeps it up. */
static uint64_t g_osd_until;
static int      g_osd_kind;     /* 0 volume, 1 brightness */
static int      g_osd_level, g_osd_max;

static void osd_show(int kind, int level, int max)
{
	g_osd_kind  = kind;
	g_osd_level = level;
	g_osd_max   = max;
	g_osd_until = diatom_port_now_us() + 1500000ull;
}

static void gain_nudge(int dir)
{
	gain_ensure();
	if (g_level < 0) return;
	g_level += dir;
	if (g_level < 0)           g_level = 0;
	if (g_level > GAIN_LEVELS) g_level = GAIN_LEVELS;
	gain_apply();
	osd_show(0, g_level, GAIN_LEVELS);
}

static void bright_nudge(int dir)
{
	bright_ensure();
	if (g_bright < 0) return;
	g_bright += dir;
	if (g_bright < 0)             g_bright = 0;
	if (g_bright > BRIGHT_LEVELS) g_bright = BRIGHT_LEVELS;
	bright_apply();
	osd_show(1, g_bright, BRIGHT_LEVELS);
}

/* The Brick's indicator (port/brick.c draw_gain_bar), scaled from its 768
 * rows to this panel's: a scrim across the top with a bar in it, in the
 * launcher's two colors, so the tint says which key was pressed. */
static void draw_osd(void)
{
	int sw = 0, sh = 0, pad, bar, fill;
	SDL_Rect r;

	if (diatom_port_now_us() >= g_osd_until) return;
	SDL_GetRendererOutputSize(g_renderer, &sw, &sh);
	pad = 3 * sh / 768;
	bar = 6 * sh / 768;
	fill = g_osd_max > 0 ? sw * g_osd_level / g_osd_max : 0;

	SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 128);
	r.x = 0; r.y = 0; r.w = sw; r.h = pad * 2 + bar;
	SDL_RenderFillRect(g_renderer, &r);
	SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_NONE);

	SDL_SetRenderDrawColor(g_renderer, 60, 62, 72, 255);
	r.y = pad; r.h = bar;
	SDL_RenderFillRect(g_renderer, &r);
	if (g_osd_kind) SDL_SetRenderDrawColor(g_renderer, 255, 206, 128, 255);
	else            SDL_SetRenderDrawColor(g_renderer,  61, 214, 255, 255);
	r.w = fill;
	SDL_RenderFillRect(g_renderer, &r);

	SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);   /* RenderClear's */
}

static void levels_init(void)
{
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *e;

	while (d && (e = readdir(d)))
		if (e->d_name[0] != '.') {
			snprintf(g_bl_path, sizeof g_bl_path,
			         "/sys/class/backlight/%s/brightness", e->d_name);
			break;
		}
	if (d) closedir(d);
	if (pthread_create(&g_vol_thread, NULL, vol_worker, NULL) == 0)
		pthread_detach(g_vol_thread);
}

/* ADR-0020's rescale: round-to-nearest, endpoints exact. */
static int rescale(int index, int from, int to)
{
	if (from <= 1 || to <= 1) return 0;
	if (index < 0)        index = 0;
	if (index > from - 1) index = from - 1;
	return (index * (to - 1) + (from - 1) / 2) / (from - 1);
}

bool diatom_port_level_get(diatom_level_kind kind, int *index, int *count)
{
	switch (kind) {
	case DIATOM_LEVEL_VOLUME:
		gain_ensure();
		if (g_level < 0) return false;
		*index = g_level;
		*count = GAIN_LEVELS + 1;
		return true;
	case DIATOM_LEVEL_BRIGHTNESS:
		bright_ensure();
		if (g_bright < 0) return false;
		*index = g_bright;
		*count = BRIGHT_LEVELS + 1;
		return true;
	default:
		return false;
	}
}

bool diatom_port_level_set(diatom_level_kind kind, int index, int count)
{
	switch (kind) {
	case DIATOM_LEVEL_VOLUME:
		g_level = rescale(index, count, GAIN_LEVELS + 1);
		gain_apply();
		return true;
	case DIATOM_LEVEL_BRIGHTNESS:
		if (!g_bl_path[0]) return false;
		g_bright = rescale(index, count, BRIGHT_LEVELS + 1);
		bright_apply();
		return true;
	default:
		return false;
	}
}

/* The launcher owns levels while Diatom is not presenting; forget ours. */
void diatom_port_level_invalidate(void)
{
	g_level  = -1;
	g_bright = -1;
}

/* The handover. The launcher is another Wayland client under the same sway,
 * so giving it the display means getting out of its way: unmap this window
 * and let the compositor show what is behind it. The next present() maps it
 * again. Design: plorpos design.md "Display"; latency is gkd.5's to measure.
 *
 * `park` does not change what happens here. Both answers are about what
 * stays on glass for the launcher, and under a compositor nothing of this
 * window stays: at a pause the launcher draws its menu over the PREVIEW
 * image the host wrote first, not over this window.
 *
 * Nothing is in flight to drain - present() is synchronous here - but the
 * unmap has to REACH sway before the host sends PAUSED or EXIT, or both
 * windows are up at once. Pumping flushes the Wayland connection. */

void diatom_port_present_stop(diatom_park park)
{
	(void)park;
	if (!g_window || g_hidden) return;
	SDL_HideWindow(g_window);
	SDL_PumpEvents();
	g_hidden = true;
}

/* No mute switch and no analog stage to hold off on the GKD (ADR-0031): the
 * port answers what it was told, as the desktop port does. */
static bool g_muted;
void diatom_port_mute_set(bool on) { g_muted = on; }
bool diatom_port_mute_get(void) { return g_muted; }
