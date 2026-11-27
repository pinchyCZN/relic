/* RELIC - the SDL2 host.
 *
 * THE ONLY LOOP IN THE PROJECT LIVES HERE, and on the web it does
 * not even live here - emscripten owns the loop and calls one
 * frame at a time. Both paths call exactly the same tick():
 *
 *     native   while (running) tick();
 *     web      emscripten_set_main_loop(tick, 0, 1);
 *
 * Nothing under src/ outside main_*.c may loop, block or sleep.
 *
 *     relic_sdl [relic-binary] [-w WIDTH] [-h HEIGHT] [-p PART]
 *                 [-rdiv N] [-mod FILE]
 *
 * -w AND -h ARE THE VISIBLE SIZE, -rdiv THE COST: the frame is drawn
 * at the window divided by N and stretched, so a big window need not
 * starve the audio. 'r' cycles the same divisor while it runs.
 *
 * THE TUNE COMES OUT OF RELIC.DAT and so loads after Demo_Init;
 * -mod overrides it.
 *
 * Keys: [ ] step mark, space pause, . single frame, f fast forward
 * while held, r render divisor, i subpixel, m mute, + and - volume
 * in tens, q quit. Left-drag swings the camera in parts 2.3b, 2.4,
 * 2.7, 2.10, 2.11, 2.13 and 2.14, and springs back on release; c or
 * the middle button toggles the look hold, which keeps it instead, so
 * drag after drag turns a full circle. On c.1 the drag orbits what is
 * shown and stays put, the wheel dollies it, and the hold does not
 * apply. The right button puts either back to rest. Tab or Enter
 * shows the next thing on c.1, shift the previous, and z and x are
 * held.
 *
 * F OVERRIDES PAUSE, so it doubles as a scrub from a stopped frame.
 * Its KEYDOWN repeats while held, which is why setting it is
 * guarded - otherwise the title is rewritten on every event. AND
 * ALT-TAB EATS THE KEYUP: without the focus-lost arm the demo keeps
 * sprinting with the window in the background.
 *
 * EMSCRIPTEN ANSWERS A SIZE QUERY BY NOT WRITING IT, so the present
 * seeds its locals with -1 and takes the first size that answers -
 * the renderer's, then the window's, then the frame's. A rect built
 * from an unwritten query draws nothing and reports no error, which
 * is a black window with the demo running behind it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "SDL.h"

#include "relic.h"
#include "host.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static s32 g_tex_w, g_tex_h;
static int g_running = 1;
static int g_paused;

/* OURS - ticks a pass while F is held. Twenty is about seven seconds
 * of demo a second at 60 Hz, and the skip costs one render. */
#define FF_STEPS 20
static int g_ff;

/* OURS - the audio side. A QUEUE, NOT A CALLBACK, so the mixer runs
 * on this thread and fast forward cannot outrun the device: the
 * top-up stops at AUDIO_TARGET however many ticks a pass ran.
 * Zero is not one of the volume steps - m owns silence, so there
 * are not two ways to be quiet and unmuting knows where to return. */
#define AUDIO_RATE   44100
#define AUDIO_CHUNK  1024                   /* pairs per queue call */
/* THE QUEUE IS LATENCY: everything is heard this far after it is
 * mixed, so the typewriter trailed the typing by 125 ms at the old
 * eighth of a second. Three PAL fields is still two frames of slack
 * at 60 Hz, which the pre-parsed tunes make safe - nothing stalls
 * the loop long enough to drain it any more. */
#define AUDIO_TARGET ((AUDIO_RATE / 50) * 4 * 3)   /* bytes: 60 ms */
#define VOL_MIN      10
#define VOL_MAX      100
#define VOL_STEP     10
static SDL_AudioDeviceID g_aud;
static s16 *g_mix;
static int  g_muted;
static s32  g_vol = VOL_MAX;

static void retexture(s32 w, s32 h)
{
	if (g_tex && w == g_tex_w && h == g_tex_h) return;
	if (g_tex) SDL_DestroyTexture(g_tex);
	g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_RGB24,
	                          SDL_TEXTUREACCESS_STREAMING, (int)w, (int)h);
	g_tex_w = w;
	g_tex_h = h;
}

/* OURS. On by default: the demo's own motion is quantised far below a
 * pixel per tick, so at any real window size it steps visibly without
 * this. 'i' turns it off to compare. */
static s32 g_modes_want = RELIC_MODE_SUBPIXEL;

static void title(void)
{
	char buf[256];
	char vol[32];

	if (!Music_Playing()) strcpy(vol, "  [no module]");
	else if (g_muted) strcpy(vol, "  [muted]");
	else sprintf(vol, "  [vol %ld%%]", (long)g_vol);
	sprintf(buf, "RELIC  -  %s  [%s]%s%s%s%s", Demo_PartLabel(),
	        Demo_MarkName(),
	        (Demo_Modes() & RELIC_MODE_SUBPIXEL) ? "  [subpixel, OURS]" : "",
	        Demo_LookHold() ? "  [look hold]" : "",
	        g_ff ? "  [fast forward]" : "", vol);
	if (g_win) SDL_SetWindowTitle(g_win, buf);
}

/* OURS - THE PAGE'S HANDLES ON THE AUDIO, and the only things in
 * this host a browser may call. The keys already move these two, so
 * a page SETS with these and POLLS the getters rather than being
 * told: nothing here has to know a page exists, and the demo's own
 * m and +/- stay the source of truth.
 *
 * MUTE AND LEVEL ARE KEPT APART, unlike the keyboard's + which also
 * clears mute: a page has two controls and they should not move
 * each other. docs/reconstruction.txt 3.5. */
EMSCRIPTEN_KEEPALIVE void Host_SetMute(int on)
{
	g_muted = on ? 1 : 0;
	title();
}

EMSCRIPTEN_KEEPALIVE int Host_Muted(void)
{
	return g_muted;
}

EMSCRIPTEN_KEEPALIVE void Host_SetVolume(int v)
{
	if (v < VOL_MIN) v = VOL_MIN;
	if (v > VOL_MAX) v = VOL_MAX;
	g_vol = (s32)v;
	title();
}

EMSCRIPTEN_KEEPALIVE int Host_Volume(void)
{
	return (int)g_vol;
}


/* OURS. THE RENDER RESOLUTION IS NOT THE WINDOW'S. Every part is a
 * per-pixel loop, so cost is the frame's AREA: a 1920 x 1080 window
 * is 2.07 million pixels against the demo's 160 x 100, which is
 * 16000. Rendering at the window size and blitting one to one is what
 * makes a large window slow and shrinking it the only cure. This
 * divides the render frame and lets SDL stretch it, so window size
 * and fill cost come apart. 'r' cycles the divisor. */
static s32 g_rdiv = 1;
static int g_win_w = 1, g_win_h = 1;

/* OURS - THE POINTER, REPORTED AS A GESTURE AND NOT AS A CAMERA.
 * What the deflection is measured against is the window, which is
 * this host's business; what it MEANS is the demo's - src/host.h. So
 * this reports the button and how far it has moved since it went
 * down, and nothing else: no limit on the wheel, no easing, and no
 * knowledge of which parts care. A BUTTON-UP DELIVERED ELSEWHERE
 * NEVER ARRIVES and the gesture would stay held for good, so a lost
 * focus ends it and so does the first motion reporting the button
 * already up. */
static s32 g_wheel;
/* OURS - THE KEYBOARD'S TWO NEUTRAL CHANNELS. Tab and Enter both
 * mean "next" and only bump a count; Z is reported as held and this
 * host does not know what either is for - src/host.h. A KEY-UP
 * DELIVERED ELSEWHERE NEVER ARRIVES, so a lost focus clears the
 * held set exactly as it ends a drag. */
static s32 g_cycle;
static u32 g_hold;
static int g_down;
static int g_press_x, g_press_y;    /* where the button went down */

/* OURS - FROM THE PRESS POINT, not accumulated per motion event, so a
 * dropped event cannot leave the gesture leaning. */
static void drag_move(int x, int y)
{
	s32 hw = g_win_w / 2, hh = g_win_h / 2;

	if (hw < 1) hw = 1;
	if (hh < 1) hh = 1;
	Demo_SetDrag(1, ((s32)(x - g_press_x) << 16) / hw,
	                ((s32)(y - g_press_y) << 16) / hh);
}

/* OURS */
static void drag_end(void)
{
	g_down = 0;
	Demo_SetDrag(0, 0L, 0L);
}

/* OURS - the render size, from the window. A FAILURE HERE LEAVES NO
 * FRAME AT ALL: Demo_SetResolution frees the old one before it builds
 * the new, so the only sign is a black window. Say so. */
static void set_render_size(void)
{
	s32 w = g_win_w / g_rdiv, h = g_win_h / g_rdiv;
	if (w < 16) w = 16;
	if (h < 16) h = 16;
	if (Demo_SetResolution(w, h))
		fprintf(stderr, "relic: no frame at %ldx%ld\n",
		        (long)w, (long)h);
}

/* OURS - the device. A FAILURE HERE IS NOT FATAL: no device leaves
 * Demo_Audio handing back silence and the demo runs as it did. */
static void open_audio(void)
{
	SDL_AudioSpec want, got;

	memset(&want, 0, sizeof want);
	want.freq = AUDIO_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	want.samples = 1024;
	g_aud = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
	g_mix = (s16 *)malloc((size_t)AUDIO_CHUNK * 2 * sizeof(s16));
	if (g_aud && g_mix) SDL_PauseAudioDevice(g_aud, 0);
}

/* OURS - -mod, for a module the demo does not carry. Read whole,
 * because that is how ibxm wants it. */
static int load_module_file(const char *path)
{
	FILE *fh = fopen(path, "rb");
	u8 *bytes = 0;
	long len = 0;
	int rc;

	if (!fh) {
		fprintf(stderr, "relic: cannot read %s\n", path);
		return 1;
	}
	fseek(fh, 0, SEEK_END);
	len = ftell(fh);
	fseek(fh, 0, SEEK_SET);
	bytes = (len > 0) ? (u8 *)malloc((size_t)len) : 0;
	if (bytes && fread(bytes, 1, (size_t)len, fh) != (size_t)len) {
		free(bytes);
		bytes = 0;
	}
	fclose(fh);
	if (!bytes) return 1;
	rc = Music_Load(bytes, (s32)len, AUDIO_RATE);
	free(bytes);
	if (!rc) Music_Pin();
	return rc;
}

/* OURS - volume is applied HERE and not in the mixer, so muting
 * never costs the replayer its place: it keeps running and only the
 * samples handed to the device are scaled. */
static void queue_audio(s32 pairs)
{
	s32 v = g_muted ? 0 : g_vol;
	s32 i;

	if (!g_aud || !g_mix) return;
	if (v != VOL_MAX)
		for (i = 0; i < pairs * 2; i++)
			g_mix[i] = (s16)((s32)g_mix[i] * v / VOL_MAX);
	SDL_QueueAudio(g_aud, g_mix, (Uint32)(pairs * 2 * (s32)sizeof(s16)));
}

/* OURS - ONE FIELD OF MUSIC, PULLED AND HEARD. A single step moves
 * the demo's tick, so the song has to move with it or the picture
 * walks away from the score and every cue gate after it answers
 * against a stale position. One PAL field is AUDIO_RATE / 50 pairs.
 * THE DEVICE IS NEVER STOPPED, which is what makes this audible:
 * pause holds the pump instead, so the queued tail drains and then
 * goes quiet of its own accord. */
static void audio_step_one(void)
{
	if (!g_aud || !g_mix) return;
	Demo_Audio(g_mix, AUDIO_RATE / 50);
	queue_audio(AUDIO_RATE / 50);
}

/* OURS - top the device up to AUDIO_TARGET bytes, however many that
 * takes. The guard is what stops a stalled device spinning here. */
static void pump_audio(void)
{
	int guard = 64;

	if (!g_aud || !g_mix) return;
	while (SDL_GetQueuedAudioSize(g_aud) < (Uint32)AUDIO_TARGET
	       && guard-- > 0) {
		Demo_Audio(g_mix, AUDIO_CHUNK);
		queue_audio(AUDIO_CHUNK);
	}
}

static void events(void)
{
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		if (e.type == SDL_QUIT) g_running = 0;
		else if (e.type == SDL_KEYDOWN) {
			switch (e.key.keysym.sym) {
			case SDLK_ESCAPE:
			case SDLK_q:            g_running = 0; break;
			case SDLK_RIGHTBRACKET:
			case SDLK_LEFTBRACKET:
				Demo_StepMark(e.key.keysym.sym
				              == SDLK_RIGHTBRACKET ? 1 : -1);
				title();
				puts(Demo_StateLine());
				puts(Demo_MusicLine());
				break;
			case SDLK_TAB:
			case SDLK_RETURN:
			case SDLK_KP_ENTER:
				g_cycle += (e.key.keysym.mod & KMOD_SHIFT)
				         ? -1 : 1;
				Demo_SetCycle(g_cycle);
				break;
			case SDLK_z:
				g_hold |= DEMO_HOLD_A;
				Demo_SetHold(g_hold);
				break;
			case SDLK_x:
				g_hold |= DEMO_HOLD_B;
				Demo_SetHold(g_hold);
				break;
			case SDLK_SPACE:
				g_paused = !g_paused;
				if (g_paused) puts(Demo_StateLine());
				break;
			case SDLK_PERIOD:
				g_paused = 1;
				Demo_Frame();
				audio_step_one();
				puts(Demo_StateLine());
				break;
			case SDLK_r:
				g_rdiv = (g_rdiv >= 4) ? 1 : g_rdiv + 1;
				set_render_size();
				title();
				break;
			case SDLK_m:
				g_muted = !g_muted;
				title();
				break;
			case SDLK_KP_PLUS:
			case SDLK_EQUALS:
			case SDLK_PLUS:
				if (g_vol < VOL_MAX) g_vol += VOL_STEP;
				g_muted = 0;
				title();
				break;
			case SDLK_KP_MINUS:
			case SDLK_MINUS:
				if (g_vol > VOL_MIN) g_vol -= VOL_STEP;
				title();
				break;
			case SDLK_f:
				if (!g_ff) { g_ff = 1; title(); }
				break;
			case SDLK_c:
				Demo_SetLookHold(!Demo_LookHold());
				title();
				break;
			case SDLK_i:
				g_modes_want ^= RELIC_MODE_SUBPIXEL;
				Demo_SetModes(g_modes_want);
				title();
				break;
			default: break;
			}
		} else if (e.type == SDL_KEYUP) {
			if (e.key.keysym.sym == SDLK_z) {
				g_hold &= ~DEMO_HOLD_A;
				Demo_SetHold(g_hold);
			}
			if (e.key.keysym.sym == SDLK_x) {
				g_hold &= ~DEMO_HOLD_B;
				Demo_SetHold(g_hold);
			}
			if (e.key.keysym.sym == SDLK_f && g_ff) {
				g_ff = 0;
				title();
			}
		} else if (e.type == SDL_MOUSEWHEEL) {
			g_wheel += (s32)e.wheel.y;
			Demo_SetWheel(g_wheel);
		} else if (e.type == SDL_MOUSEBUTTONDOWN) {
			if (e.button.button == SDL_BUTTON_LEFT) {
				g_down = 1;
				g_press_x = e.button.x;
				g_press_y = e.button.y;
				drag_move(e.button.x, e.button.y);
			} else if (e.button.button == SDL_BUTTON_MIDDLE) {
				Demo_SetLookHold(!Demo_LookHold());
				title();
			} else if (e.button.button == SDL_BUTTON_RIGHT) {
				Demo_SetHome();
			}
		} else if (e.type == SDL_MOUSEBUTTONUP) {
			if (e.button.button == SDL_BUTTON_LEFT) drag_end();
		} else if (e.type == SDL_MOUSEMOTION && g_down) {
			if (e.motion.state & SDL_BUTTON_LMASK)
				drag_move(e.motion.x, e.motion.y);
			else
				drag_end();
		} else if (e.type == SDL_WINDOWEVENT) {
			if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
				g_win_w = e.window.data1;
				g_win_h = e.window.data2;
				set_render_size();
			} else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
				drag_end();
				g_hold = 0;
				Demo_SetHold(0UL);
				if (g_ff) {
					g_ff = 0;
					title();
				}
			}
		}
	}
}

static u32 g_last_ms;
static int g_blackmsg;              /* OURS: said it once already */

static void tick(void)
{
	const u8 *rgb;
	s32 w, h;
	SDL_Rect dst;
	int ww, wh;

	events();
	{
		u32 now = (u32)SDL_GetTicks();
		s32 dt = (s32)(now - g_last_ms);
		g_last_ms = now;
		if (g_ff) Demo_Skip(FF_STEPS);
		else if (!g_paused) Demo_Advance(dt);
		if (!g_paused || g_ff) pump_audio();
	}

	rgb = Demo_Pixels(&w, &h);
	if (!rgb || w < 1 || h < 1) {
		if (!g_blackmsg) {
			g_blackmsg = 1;
			fprintf(stderr, "relic: no frame to present, %ldx%ld\n",
			        (long)w, (long)h);
		}
		return;
	}
	retexture(w, h);
	if (!g_tex) {
		if (!g_blackmsg) {
			g_blackmsg = 1;
			fprintf(stderr, "relic: no %ldx%ld texture: %s\n",
			        (long)w, (long)h, SDL_GetError());
		}
		return;
	}
	if (SDL_UpdateTexture(g_tex, NULL, rgb, (int)w * 3) && !g_blackmsg) {
		g_blackmsg = 1;
		fprintf(stderr, "relic: UpdateTexture: %s\n", SDL_GetError());
	}

	/* STRETCHED, NOT CENTRED, so a divided render still fills the
	 * window. The aspect is the window's; the frame is built to it. */
	ww = wh = -1;
	SDL_GetRendererOutputSize(g_ren, &ww, &wh);
	if (ww < 1 || wh < 1) SDL_GetWindowSize(g_win, &ww, &wh);
	if (ww < 1 || wh < 1) { ww = (int)w; wh = (int)h; }
	dst.x = 0;
	dst.y = 0;
	dst.w = ww;
	dst.h = wh;
	SDL_RenderSetViewport(g_ren, &dst);
	SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
	SDL_RenderClear(g_ren);
	if (SDL_RenderCopy(g_ren, g_tex, NULL, &dst) && !g_blackmsg) {
		g_blackmsg = 1;
		fprintf(stderr, "relic: RenderCopy: %s\n", SDL_GetError());
	}
	SDL_RenderPresent(g_ren);
}

int main(int argc, char **argv)
{
	const char *bin = "reverse/ghidra/relic";
	const char *dat = "reverse/ghidra/relic.dat";
	const char *mod = 0;
	int i, w = 1280, h = 720, part = 0;
#ifdef _DEBUG
	w=640; h=360;
	g_muted=1;
#endif

	for (i = 1; i < argc; i++) {
		if      (!strcmp(argv[i], "-w") && i + 1 < argc) w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-h") && i + 1 < argc) h = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-p") && i + 1 < argc) part = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-rdiv") && i + 1 < argc) {
			g_rdiv = (s32)atol(argv[++i]);
			if (g_rdiv < 1L) g_rdiv = 1L;
			if (g_rdiv > 4L) g_rdiv = 4L;
		}
		else if (!strcmp(argv[i], "-mod") && i + 1 < argc) mod = argv[++i];
		else if (argv[i][0] != '-') bin = argv[i];
	}

	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	open_audio();
	g_win = SDL_CreateWindow("RELIC", SDL_WINDOWPOS_CENTERED,
	                         SDL_WINDOWPOS_CENTERED, w, h,
	                         SDL_WINDOW_RESIZABLE);
	if (!g_win) {
		fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		SDL_Quit();
		return 1;
	}
	g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED
	                                       | SDL_RENDERER_PRESENTVSYNC);
	if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
	if (!g_ren) {
		fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
		SDL_DestroyWindow(g_win);
		SDL_Quit();
		return 1;
	}

	if (Demo_Init(bin, dat)) {
		fprintf(stderr, "relic: could not load %s\n", bin);
		SDL_Quit();
		return 1;
	}
	Music_SetRate(AUDIO_RATE);
	if (Music_Preload(AUDIO_RATE) != 3)
		fputs("relic: not every tune loaded\n", stderr);
	if (mod && load_module_file(mod))
		fputs("relic: no music\n", stderr);
	if (part) Demo_GotoPart(part);
	Demo_SetModes(g_modes_want);
	g_win_w = (int)w;
	g_win_h = (int)h;
	if (Demo_SetResolution(w / g_rdiv, h / g_rdiv)) {
		fprintf(stderr, "relic: could not size the frame\n");
		Demo_Shutdown();
		SDL_Quit();
		return 1;
	}
	g_last_ms = (u32)SDL_GetTicks();
	title();

#ifdef __EMSCRIPTEN__
	emscripten_set_main_loop(tick, 0, 1);
#else
	while (g_running) {
		tick();
		SDL_Delay(1);
	}
	Demo_Shutdown();
	if (g_aud) SDL_CloseAudioDevice(g_aud);
	free(g_mix);
	Music_Unload();
	if (g_tex) SDL_DestroyTexture(g_tex);
	SDL_DestroyRenderer(g_ren);
	SDL_DestroyWindow(g_win);
	SDL_Quit();
#endif
	return 0;
}
