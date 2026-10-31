#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/input.h>

#include <pulse/simple.h>
#include <pulse/error.h>
#include <sndfile.h>

#include "minijson.h"

#define OUT_RATE      48000
#define OUT_CH        2
/* One mix period. 960 frames = 20 ms at 48 kHz.
 *
 * This is a *maximum* chunk, not a latency floor: a keystroke starts a fresh
 * period with the click at sample 0, so it is not delayed by the period size.
 * Larger periods just mean fewer wake-ups and writes per keystroke, which is
 * why this is the cheapest setting measured (5.3 ms CPU/key against 10 ms at
 * 120 frames) without changing latency. */
#ifndef PERIOD_FRAMES
#define PERIOD_FRAMES 960
#endif
#define MAX_KEYS      1024

/* Wheel clicks have no evdev key code of their own; give them synthetic ones
 * well above the real BTN_* range so a pack can map them numerically. */
#define CODE_WHEEL_UP    1001
#define CODE_WHEEL_DOWN  1002
#define CODE_WHEEL_LEFT  1003
#define CODE_WHEEL_RIGHT 1004
#define MAX_VOICES    64
#define MAX_DEVICES   64

typedef struct {
	int16_t *data;
	size_t frames;
	int ch;
	int owned;
} sample_t;

typedef struct {
	const sample_t *smp;
	size_t pos;
	float gain;
	int used;
} voice_t;

static pthread_mutex_t mix_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mix_cond = PTHREAD_COND_INITIALIZER;
static int g_trace = 0;
static int g_mouse = 1;

static double mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static voice_t voices[MAX_VOICES];
static volatile sig_atomic_t g_stop = 0;
static volatile sig_atomic_t g_input_failed = 0;
static float g_gain = 0.5f;
static int g_verbose = 0;

static void on_signal(int sig)
{
	(void)sig;
	g_stop = 1;
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n);
	if (!p) {
		fprintf(stderr, "mechsim: out of memory (%zu bytes)\n", n);
		exit(1);
	}
	return p;
}

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *d = xmalloc(n);
	memcpy(d, s, n);
	return d;
}

static char *path_join(const char *a, const char *b)
{
	if (b[0] == '/')
		return xstrdup(b);
	size_t n = strlen(a) + strlen(b) + 2;
	char *p = xmalloc(n);
	snprintf(p, n, "%s/%s", a, b);
	return p;
}

static unsigned g_rng = 0x2545F491;
static unsigned rnd(void)
{
	g_rng ^= g_rng << 13;
	g_rng ^= g_rng >> 17;
	g_rng ^= g_rng << 5;
	return g_rng;
}

static int16_t *resample_linear(const int16_t *in, size_t in_frames, int ch,
				 int in_rate, int out_rate, size_t *out_frames)
{
	if (in_rate == out_rate || in_frames == 0) {
		size_t n = in_frames * (size_t)ch;
		int16_t *copy = xmalloc(n * sizeof(int16_t));
		memcpy(copy, in, n * sizeof(int16_t));
		*out_frames = in_frames;
		return copy;
	}

	double step = (double)in_rate / (double)out_rate;
	size_t out_n = (size_t)((double)in_frames / step) + 1;
	int16_t *out = xmalloc(out_n * (size_t)ch * sizeof(int16_t));

	for (size_t i = 0; i < out_n; i++) {
		double src = (double)i * step;
		size_t i0 = (size_t)src;
		size_t i1 = i0 + 1 < in_frames ? i0 + 1 : in_frames - 1;
		float frac = (float)(src - (double)i0);
		for (int c = 0; c < ch; c++) {
			float a = in[i0 * (size_t)ch + (size_t)c];
			float b = in[i1 * (size_t)ch + (size_t)c];
			out[i * (size_t)ch + (size_t)c] = (int16_t)lrintf(a + (b - a) * frac);
		}
	}
	*out_frames = out_n;
	return out;
}

static sample_t *load_file(const char *path)
{
	SF_INFO info;
	memset(&info, 0, sizeof(info));
	SNDFILE *sf = sf_open(path, SFM_READ, &info);
	if (!sf) {
		if (g_verbose)
			fprintf(stderr, "mechsim: cannot open %s: %s\n", path,
				sf_strerror(NULL));
		return NULL;
	}
	if (info.frames <= 0 || info.channels <= 0) {
		sf_close(sf);
		return NULL;
	}

	size_t frames = (size_t)info.frames;
	int ch = info.channels > 2 ? 2 : info.channels;
	int16_t *raw = xmalloc(frames * (size_t)info.channels * sizeof(int16_t));
	sf_count_t got = sf_readf_short(sf, raw, (sf_count_t)frames);
	sf_close(sf);
	if (got <= 0) {
		free(raw);
		return NULL;
	}
	frames = (size_t)got;

	if (info.channels > 2) {
		for (size_t i = 0; i < frames; i++) {
			raw[i * 2 + 0] = raw[i * (size_t)info.channels + 0];
			raw[i * 2 + 1] = raw[i * (size_t)info.channels + 1];
		}
		ch = 2;
	}

	size_t out_frames = 0;
	int16_t *res = resample_linear(raw, frames, ch, info.samplerate, OUT_RATE, &out_frames);
	free(raw);

	sample_t *s = xmalloc(sizeof(*s));
	s->data = res;
	s->frames = out_frames;
	s->ch = ch;
	s->owned = 1;

	if (g_verbose)
		fprintf(stderr, "  loaded %s: %zu frames %dch %dHz\n",
			path, out_frames, ch, info.samplerate);
	return s;
}

typedef struct {
	char *path;
	sample_t *smp;
} cache_entry;

static cache_entry g_cache[512];
static size_t g_cache_n = 0;

static size_t g_missing = 0;

static sample_t *load_cached(const char *path)
{
	for (size_t i = 0; i < g_cache_n; i++)
		if (strcmp(g_cache[i].path, path) == 0)
			return g_cache[i].smp;
	sample_t *s = load_file(path);
	if (!s) {
		/* Sound packs in the wild reference files they do not ship;
		 * that is a pack defect, not a fatal error. */
		g_missing++;
		return NULL;
	}
	if (g_cache_n < sizeof(g_cache) / sizeof(g_cache[0])) {
		g_cache[g_cache_n].path = xstrdup(path);
		g_cache[g_cache_n].smp = s;
		g_cache_n++;
	}
	return s;
}

static sample_t *g_press[MAX_KEYS];
static sample_t *g_release[MAX_KEYS];
static sample_t **g_generic = NULL;
static size_t g_generic_n = 0;
static sample_t *g_generic_release = NULL;
static sample_t *g_mouse_press = NULL;
static sample_t *g_mouse_release = NULL;

/* MechVibes V2 names mouse entries; MechSim packs use numeric evdev codes.
 * Accept both, so a pack written for either tool loads. */
static int key_name_to_code(const char *k, int *is_release)
{
	*is_release = 0;

	if (!k || !*k)
		return -1;

	if (k[0] >= '0' && k[0] <= '9')
		return atoi(k);

	char base[64];
	snprintf(base, sizeof(base), "%s", k);
	char *dash = strstr(base, "-up");
	if (dash) {
		*dash = 0;
		*is_release = 1;
	} else {
		size_t len = strlen(base);
		if (len > 3 && strcmp(base + len - 4, "_up") == 0) {
			base[len - 4] = 0;
			*is_release = 1;
		}
	}

	static const struct { const char *name; int code; } names[] = {
		{ "MouseLeft",       BTN_LEFT },
		{ "MouseRight",      BTN_RIGHT },
		{ "MouseMiddle",     BTN_MIDDLE },
		{ "MouseSide",       BTN_SIDE },
		{ "MouseExtra",      BTN_EXTRA },
		{ "MouseWheelUp",    CODE_WHEEL_UP },
		{ "MouseWheelDown",  CODE_WHEEL_DOWN },
		{ "MouseWheelLeft",  CODE_WHEEL_LEFT },
		{ "MouseWheelRight", CODE_WHEEL_RIGHT },
	};

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (strcmp(base, names[i].name) == 0)
			return names[i].code;

	return -1;
}

static void set_map(sample_t **map, int code, sample_t *s)
{
	if (code >= 0 && code < MAX_KEYS)
		map[code] = s;
}

static void add_generic(sample_t *s)
{
	if (!s)
		return;
	g_generic = realloc(g_generic, (g_generic_n + 1) * sizeof(*g_generic));
	if (!g_generic) {
		fprintf(stderr, "mechsim: out of memory\n");
		exit(1);
	}
	g_generic[g_generic_n++] = s;
}

static void expand_pattern(const char *dir, const char *pattern)
{
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", pattern);

	char *brace = strstr(buf, "{");
	char *close = brace ? strstr(brace, "}") : NULL;
	char *pct = strstr(buf, "%d");

	if (!close && !pct) {
		char *p = path_join(dir, pattern);
		add_generic(load_cached(p));
		free(p);
		return;
	}

	for (int i = 0; i <= 4; i++) {
		char head[1024];
		const char *tail;
		size_t hl;
		if (close) {
			hl = (size_t)(brace - buf);
			tail = close + 1;
		} else {
			hl = (size_t)(pct - buf);
			tail = pct + 2;
		}
		memcpy(head, buf, hl);
		head[hl] = 0;

		/* Sized for the pattern, never truncated: a wrong file name here
		 * would silently drop a pack's sounds. */
		size_t need = hl + strlen(tail) + 16;
		char *name = xmalloc(need);
		snprintf(name, need, "%s%d%s", head, i, tail);
		char *full = path_join(dir, name);
		free(name);
		if (access(full, R_OK) != 0) {
			free(full);
			break;
		}
		sample_t *s = load_cached(full);
		free(full);
		if (!s)
			break;
		add_generic(s);
	}
}

static char *read_whole_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
	long sz = ftell(f);
	if (sz < 0) { fclose(f); return NULL; }
	rewind(f);
	char *buf = xmalloc((size_t)sz + 1);
	size_t got = fread(buf, 1, (size_t)sz, f);
	buf[got] = 0;
	fclose(f);
	return buf;
}

static int load_pack(const char *pack_dir)
{
	char *cfg_path = path_join(pack_dir, "config.json");
	char *text = read_whole_file(cfg_path);
	if (!text) {
		fprintf(stderr, "mechsim: cannot read %s\n", cfg_path);
		free(cfg_path);
		return -1;
	}
	free(cfg_path);

	mj_val *root = mj_parse(text);
	free(text);
	if (!root) {
		fprintf(stderr, "mechsim: invalid JSON in config.json\n");
		return -1;
	}

	const char *type = mj_str(mj_get(root, "key_define_type"));
	int is_multi = type && strcmp(type, "multi") == 0;
	const char *sound = mj_str(mj_get(root, "sound"));
	const char *soundup = mj_str(mj_get(root, "soundup"));

	if (g_verbose)
		fprintf(stderr, "mechsim: pack type %s\n", is_multi ? "multi" : "single");

	if (is_multi) {
		if (sound)
			expand_pattern(pack_dir, sound);
		if (soundup) {
			char *p = path_join(pack_dir, soundup);
			g_generic_release = load_cached(p);
			free(p);
		}

		mj_val *defs = mj_get(root, "defines");
		for (size_t i = 0; defs && i < mj_len(defs); i++) {
			const char *k = mj_key_at(defs, i);
			const char *file = mj_str(mj_at(defs, i));
			if (!k || !file)
				continue;
			int is_rel = 0;
			int code = key_name_to_code(k, &is_rel);
			if (code < 0)
				continue;
			char *full = path_join(pack_dir, file);
			sample_t *s = load_cached(full);
			free(full);
			if (s)
				set_map(is_rel ? g_release : g_press, code, s);
		}
	} else {
		if (!sound) {
			fprintf(stderr, "mechsim: single-mode pack has no sound field\n");
			mj_free(root);
			return -1;
		}
		char *full = path_join(pack_dir, sound);
		sample_t *all = load_cached(full);
		free(full);
		if (!all) {
			mj_free(root);
			return -1;
		}

		mj_val *defs = mj_get(root, "defines");
		for (size_t i = 0; defs && i < mj_len(defs); i++) {
			const char *k = mj_key_at(defs, i);
			mj_val *arr = mj_at(defs, i);
			if (!k || mj_len(arr) < 2)
				continue;
			int is_rel = 0;
			int code = key_name_to_code(k, &is_rel);
			if (code < 0)
				continue;
			double start_ms = mj_num(mj_at(arr, 0), 0);
			double dur_ms = mj_num(mj_at(arr, 1), 0);
			if (dur_ms <= 0)
				continue;
			size_t start = (size_t)(start_ms * OUT_RATE / 1000.0);
			size_t len = (size_t)(dur_ms * OUT_RATE / 1000.0);
			if (start >= all->frames)
				continue;
			if (start + len > all->frames)
				len = all->frames - start;

			sample_t *slice = xmalloc(sizeof(*slice));
			slice->data = all->data + start * (size_t)all->ch;
			slice->frames = len;
			slice->ch = all->ch;
			slice->owned = 0;
			set_map(g_press, code, slice);
		}
	}

	mj_free(root);

	/* Mouse sounds: use the pack's own mouse entries if it has any, otherwise
	 * borrow a representative click so buttons are not silent. Packs here are
	 * keyboard-only, so the borrowed sample is the common case. */
	for (int c = BTN_MOUSE; c < BTN_MOUSE + 16 && c < MAX_KEYS; c++) {
		if (g_press[c] && !g_mouse_press)
			g_mouse_press = g_press[c];
		if (g_release[c] && !g_mouse_release)
			g_mouse_release = g_release[c];
	}
	if (!g_mouse_press) {
		if (g_generic_n > 0)
			g_mouse_press = g_generic[0];
		else
			g_mouse_press = g_press[KEY_SPACE] ? g_press[KEY_SPACE]
							  : g_press[KEY_A];
	}
	if (!g_mouse_release)
		g_mouse_release = g_generic_release;

	return 0;
}

static void play_sample(const sample_t *s, float gain)
{
	if (!s || s->frames == 0)
		return;

	pthread_mutex_lock(&mix_lock);
	int slot = -1;
	for (int i = 0; i < MAX_VOICES; i++) {
		if (!voices[i].used) { slot = i; break; }
	}
	if (slot < 0) {
		size_t best = (size_t)-1;
		for (int i = 0; i < MAX_VOICES; i++) {
			size_t left = voices[i].smp ? voices[i].smp->frames - voices[i].pos : 0;
			if (left < best) { best = left; slot = i; }
		}
	}
	voices[slot].smp = s;
	voices[slot].pos = 0;
	voices[slot].gain = gain;
	voices[slot].used = 1;
	/* Wake the writer so the first period carrying this sound goes out at
	 * once, instead of waiting for the next tick of a free-running loop. */
	pthread_cond_signal(&mix_cond);
	pthread_mutex_unlock(&mix_lock);
}

/* Returns the number of voices still active after this period. */
static int mix_period(float *out, int frames)
{
	memset(out, 0, (size_t)frames * OUT_CH * sizeof(float));
	int active = 0;

	pthread_mutex_lock(&mix_lock);
	for (int v = 0; v < MAX_VOICES; v++) {
		voice_t *vc = &voices[v];
		if (!vc->used)
			continue;
		const sample_t *s = vc->smp;
		size_t avail = s->frames - vc->pos;
		size_t n = avail < (size_t)frames ? avail : (size_t)frames;
		float g = vc->gain;

		if (g_trace && vc->pos == 0) {
			printf("MIXST %.3f\n", mono_ms());
			fflush(stdout);
		}

		if (s->ch == 1) {
			const int16_t *src = s->data + vc->pos;
			for (size_t f = 0; f < n; f++) {
				float smp = (float)src[f] * g;
				out[f * 2 + 0] += smp;
				out[f * 2 + 1] += smp;
			}
		} else {
			const int16_t *src = s->data + vc->pos * 2;
			for (size_t f = 0; f < n; f++) {
				out[f * 2 + 0] += (float)src[f * 2 + 0] * g;
				out[f * 2 + 1] += (float)src[f * 2 + 1] * g;
			}
		}

		vc->pos += n;
		if (vc->pos >= s->frames)
			vc->used = 0;
		else
			active++;
	}
	pthread_mutex_unlock(&mix_lock);
	return active;
}

static void *audio_thread(void *arg)
{
	(void)arg;

	pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = OUT_RATE, .channels = OUT_CH };
	pa_buffer_attr attr = {
		.maxlength = (uint32_t)-1,
		.tlength = PERIOD_FRAMES * OUT_CH * 2 * 2,
		.prebuf = (uint32_t)-1,
		.minreq = (uint32_t)-1,
		.fragsize = (uint32_t)-1,
	};

	int err = 0;
	pa_simple *pa = pa_simple_new(NULL, "mechsim", PA_STREAM_PLAYBACK, NULL,
				      "keyboard", &ss, NULL, &attr, &err);
	if (!pa) {
		fprintf(stderr, "mechsim: cannot open audio output: %s\n", pa_strerror(err));
		g_stop = 1;
		return NULL;
	}

	float *mix = xmalloc((size_t)PERIOD_FRAMES * OUT_CH * sizeof(float));
	int16_t *out = xmalloc((size_t)PERIOD_FRAMES * OUT_CH * sizeof(int16_t));

	/* Deliberately do NOT keep the sink topped up with silence.
	 *
	 * Latency here is dominated by how much audio is already queued ahead
	 * of the click: anything sitting in the buffer must play out before the
	 * click is heard. A free-running writer parks a full buffer of silence
	 * between the keystroke and the sound, which measured ~25 ms no matter
	 * how the period was tuned. Writing only while a voice is active keeps
	 * the queue near-empty, so a press reaches the speaker in ~10 ms.
	 *
	 * The trade is that the stream goes idle between keystrokes. That is
	 * fine: the sink suspends, and the write that carries the first click
	 * of a burst wakes it up. */
	while (!g_stop) {
		pthread_mutex_lock(&mix_lock);
		int pending = 0;
		for (int i = 0; i < MAX_VOICES; i++)
			pending |= voices[i].used;
		if (!pending && !g_stop) {
			/* Nothing to play: sleep until a key registers a voice. */
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_nsec += 100 * 1000 * 1000;   /* 100 ms guard */
			if (until.tv_nsec >= 1000000000L) {
				until.tv_sec += 1;
				until.tv_nsec -= 1000000000L;
			}
			pthread_cond_timedwait(&mix_cond, &mix_lock, &until);
			pthread_mutex_unlock(&mix_lock);
			continue;
		}
		pthread_mutex_unlock(&mix_lock);

		int active = mix_period(mix, PERIOD_FRAMES);
		for (int i = 0; i < PERIOD_FRAMES * OUT_CH; i++) {
			float v = mix[i];
			if (v > 32767.0f) v = 32767.0f;
			else if (v < -32768.0f) v = -32768.0f;
			out[i] = (int16_t)lrintf(v);
		}
		if (pa_simple_write(pa, out, (size_t)PERIOD_FRAMES * OUT_CH * sizeof(int16_t), &err) < 0) {
			fprintf(stderr, "mechsim: audio write failed: %s\n", pa_strerror(err));
			g_stop = 1;
			break;
		}
		(void)active;
	}

	pa_simple_drain(pa, &err);
	pa_simple_free(pa);
	free(mix);
	free(out);
	return NULL;
}

typedef struct {
	int fd;
	int num;
	int mouse;
	char name[128];
} device_t;

/* A device worth reading: anything that reports letter keys, a mouse button,
 * or a wheel. Mice report BTN_LEFT and no letter keys at all, so a keyboard-only
 * test skips them entirely. */
static int device_caps(int fd, int *has_keys, int *has_buttons, int *has_wheel)
{
	uint8_t bits[(KEY_CNT + 7) / 8];
	uint8_t rel[(REL_CNT + 7) / 8];

	*has_keys = *has_buttons = *has_wheel = 0;

	/* EVIOCGBIT returns a positive byte count on success, not 0, so compare
	 * against < 0. Testing == 0 here makes every device look unsupported. */
	memset(bits, 0, sizeof(bits));
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) >= 0) {
#define HAS(code) (bits[(code) / 8] & (1u << ((code) % 8)))
		*has_keys = HAS(KEY_A) || HAS(KEY_Z) || HAS(KEY_SPACE);
		*has_buttons = HAS(BTN_LEFT) || HAS(BTN_RIGHT) || HAS(BTN_MIDDLE) ||
			       HAS(BTN_SIDE) || HAS(BTN_EXTRA);
#undef HAS
	}

	memset(rel, 0, sizeof(rel));
	if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) >= 0)
		*has_wheel = (rel[REL_WHEEL / 8] & (1u << (REL_WHEEL % 8))) ||
			     (rel[REL_HWHEEL / 8] & (1u << (REL_HWHEEL % 8)));

	return *has_keys || *has_buttons || *has_wheel;
}

static int device_known(device_t *devs, int n, int num)
{
	for (int i = 0; i < n; i++)
		if (devs[i].fd >= 0 && devs[i].num == num)
			return 1;
	return 0;
}

static int rescan_devices(device_t *devs, int n)
{
	DIR *d = opendir("/dev/input");
	if (!d)
		return n;

	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		unsigned num;
		if (sscanf(ent->d_name, "event%u", &num) != 1)
			continue;
		if (device_known(devs, n, (int)num))
			continue;
		if (n >= MAX_DEVICES)
			break;

		char path[300];
		snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		int has_keys = 0, has_buttons = 0, has_wheel = 0;
		if (!device_caps(fd, &has_keys, &has_buttons, &has_wheel)) {
			close(fd);
			continue;
		}
		/* A pointer-only device is skipped unless the user asked for mouse
		 * sounds, so a keyboard-only setup keeps paying nothing for them. */
		if (!has_keys && !g_mouse) {
			close(fd);
			continue;
		}
		devs[n].mouse = !has_keys;

		devs[n].fd = fd;
		devs[n].num = (int)num;
		devs[n].name[0] = 0;
		ioctl(fd, EVIOCGNAME(sizeof(devs[n].name) - 1), devs[n].name);
		if (g_verbose)
			fprintf(stderr, "mechsim: capturing %s (%s)\n", path, devs[n].name);
		n++;
	}
	closedir(d);
	return n;
}

static void handle_key(int code, int value)
{
	if (code < 0 || code >= MAX_KEYS)
		return;

	if (!g_mouse && code >= BTN_MOUSE)
		return;

	if (g_trace && value == 1) {
		printf("KEYIN %.3f\n", mono_ms());
		fflush(stdout);
	}

	int is_mouse = (code >= BTN_MOUSE);

	if (value == 1) {
		const sample_t *s = g_press[code];
		if (!s && is_mouse)
			s = g_mouse_press;          /* pack's mouse entry, or the fallback */
		if (!s && g_generic_n > 0)
			s = g_generic[rnd() % g_generic_n];
		if (s)
			play_sample(s, g_gain);
	} else if (value == 0) {
		const sample_t *s = g_release[code];
		if (!s && is_mouse)
			s = g_mouse_release;
		if (!s)
			s = g_generic_release;
		if (s)
			play_sample(s, g_gain);
	}
}

static void *input_thread(void *arg)
{
	(void)arg;
	device_t devs[MAX_DEVICES];
	for (int i = 0; i < MAX_DEVICES; i++)
		devs[i].fd = -1;
	int n = 0;

	n = rescan_devices(devs, n);
	int have_keyboard = 0;
	for (int i = 0; i < n; i++)
		if (!devs[i].mouse)
			have_keyboard = 1;
	if (!have_keyboard) {
		/* Give a Bluetooth or late-enumerating keyboard a moment, then
		 * fail loudly: a player that runs but captures nothing looks
		 * identical to a broken one. */
		for (int tries = 0; tries < 10 && !have_keyboard && !g_stop; tries++) {
			usleep(200 * 1000);
			n = rescan_devices(devs, n);
			for (int i = 0; i < n; i++)
				if (!devs[i].mouse)
					have_keyboard = 1;
		}
		if (!have_keyboard) {
			fprintf(stderr, "mechsim: no readable keyboard devices - make sure "
				"you are in the 'input' group and have logged back in\n");
			g_stop = 1;
			g_input_failed = 1;
			return NULL;
		}
	}
	/* Readiness marker: mechsim-toggle waits for this line so it can report
	 * success the moment capture is live instead of polling blindly. */
	int nkeys = 0, npads = 0;
	for (int i = 0; i < n; i++)
		if (devs[i].mouse)
			npads++;
		else
			nkeys++;
	if (npads)
		printf("ready: capturing %d device(s) (%d keyboard, %d mouse)\n",
		       n, nkeys, npads);
	else
		printf("ready: capturing %d device(s)\n", n);
	fflush(stdout);

	struct pollfd pfds[MAX_DEVICES];
	struct timespec scan_ts;
	clock_gettime(CLOCK_MONOTONIC, &scan_ts);
	double last_scan = (double)scan_ts.tv_sec +
			   (double)scan_ts.tv_nsec / 1e9;

	while (!g_stop) {
		for (int i = 0; i < n; i++) {
			pfds[i].fd = devs[i].fd;
			pfds[i].events = POLLIN;
			pfds[i].revents = 0;
		}

		int r = poll(pfds, (nfds_t)n, 200);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		for (int i = 0; i < n; i++) {
			if (pfds[i].revents & POLLIN) {
				struct input_event ev;
				while (read(devs[i].fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
					if (ev.type == EV_KEY) {
						handle_key(ev.code, ev.value);
					} else if (ev.type == EV_REL && g_mouse) {
						/* Wheel clicks are impulses with no release
						 * event, so treat each tick as a press. */
						if (ev.code == REL_WHEEL) {
							for (int k = 0; k < ev.value; k++)
								handle_key(CODE_WHEEL_UP, 1);
							for (int k = 0; k < -ev.value; k++)
								handle_key(CODE_WHEEL_DOWN, 1);
						} else if (ev.code == REL_HWHEEL) {
							for (int k = 0; k < ev.value; k++)
								handle_key(CODE_WHEEL_RIGHT, 1);
							for (int k = 0; k < -ev.value; k++)
								handle_key(CODE_WHEEL_LEFT, 1);
						}
					}
				}
			} else if (pfds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				if (g_verbose)
					fprintf(stderr, "mechsim: event%d went away\n", devs[i].num);
				close(devs[i].fd);
				devs[i] = devs[n - 1];
				n--;
				i--;
			}
		}

		/* Pick up keyboards that appear while the app runs (hotplug,
		 * Bluetooth, or the app having started before the device). */
		struct timespec now_ts;
		clock_gettime(CLOCK_MONOTONIC, &now_ts);
		double now_s = (double)now_ts.tv_sec +
			       (double)now_ts.tv_nsec / 1e9;
		if (now_s - last_scan >= 1.0) {
			n = rescan_devices(devs, n);
			last_scan = now_s;
		}
	}

	for (int i = 0; i < n; i++)
		if (devs[i].fd >= 0)
			close(devs[i].fd);
	return NULL;
}

static const char *g_data_dir = NULL;

static int list_packs(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d) {
		fprintf(stderr, "mechsim: cannot open %s\n", dir);
		return 1;
	}
	printf("Available sound packs in %s:\n", dir);
	struct dirent *ent;
	while ((ent = readdir(d)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;
		char *p = path_join(dir, ent->d_name);
		char *cfg = path_join(p, "config.json");
		if (access(cfg, R_OK) == 0)
			printf("  %s\n", ent->d_name);
		free(cfg);
		free(p);
	}
	closedir(d);
	return 0;
}

static const char *derive_data_dir(void)
{
	const char *env = getenv("MECHSIM_AUDIO_DIR");
	if (env)
		return env;

	static char buf[2048];
	char exe[1024];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n > 0) {
		exe[n] = 0;
		char *slash = strrchr(exe, '/');
		if (slash) {
			*slash = 0;
			snprintf(buf, sizeof(buf), "%s/audio", exe);
			if (access(buf, R_OK) == 0)
				return buf;
			snprintf(buf, sizeof(buf), "%s/../share/mechsim/audio", exe);
			if (access(buf, R_OK) == 0)
				return buf;
		}
	}
	snprintf(buf, sizeof(buf), "audio");
	if (access(buf, R_OK) == 0)
		return buf;
	snprintf(buf, sizeof(buf), "/usr/share/mechsim/audio");
	return buf;
}

static void usage(const char *prog)
{
	printf("mechsim-fast - mechanical keyboard sounds, single-stream audio\n\n");
	printf("Usage: %s [OPTIONS]\n\n", prog);
	printf("  -s, --sound NAME      Sound pack (default: eg-oreo)\n");
	printf("  -V, --volume N        Volume 0-100 (default: 50)\n");
	printf("  -d, --data-dir DIR    Directory holding the sound packs\n");
	printf("  -l, --list            List available sound packs\n");
	printf("  -v, --verbose         Report loads, devices and mix details\n");
	printf("  -m, --no-mouse        Ignore mouse buttons and the wheel\n");
	printf("  -h, --help            Show this help\n");
	printf("\nNeeds no root: read access to /dev/input/event* via the 'input' group.\n");
}

int main(int argc, char **argv)
{
	const char *pack = "eg-oreo";
	int volume = 50;
	int do_list = 0;

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] == '-' && a[1] == '-') {
			const char *name = a + 2;
			const char *eq = strchr(name, '=');
			size_t nlen = eq ? (size_t)(eq - name) : strlen(name);
			const char *val = eq ? eq + 1 : NULL;
#define IS(lit) (nlen == strlen(lit) && strncmp(name, lit, nlen) == 0)
			if (IS("sound")) pack = val ? val : (i + 1 < argc ? argv[++i] : pack);
			else if (IS("volume")) volume = atoi(val ? val : (i + 1 < argc ? argv[++i] : "50"));
			else if (IS("data-dir")) g_data_dir = val ? val : (i + 1 < argc ? argv[++i] : NULL);
			else if (IS("list")) do_list = 1;
			else if (IS("verbose")) g_verbose = 1;
			else if (IS("mouse")) g_mouse = 1;
			else if (IS("no-mouse")) g_mouse = 0;
			else if (IS("help")) { usage(argv[0]); return 0; }
			else { fprintf(stderr, "mechsim: unknown option %s\n", a); return 1; }
#undef IS
		} else if (a[0] == '-') {
			for (const char *p = a + 1; *p; p++) {
				switch (*p) {
				case 's': pack = argv[++i]; break;
				case 'V': volume = atoi(argv[++i]); break;
				case 'd': g_data_dir = argv[++i]; break;
				case 'l': do_list = 1; break;
				case 'v': g_verbose = 1; break;
				case 'm': g_mouse = 0; break;
				case 'h': usage(argv[0]); return 0;
				default:
					fprintf(stderr, "mechsim: unknown option -%c\n", *p);
					usage(argv[0]);
					return 1;
				}
			}
		}
	}

	if (volume < 0) volume = 0;
	if (volume > 100) volume = 100;
	g_gain = (float)volume / 100.0f;

	if (!g_data_dir)
		g_data_dir = derive_data_dir();

	g_trace = getenv("MECHSIM_TRACE") != NULL;

	if (do_list)
		return list_packs(g_data_dir);

	char *pack_dir = path_join(g_data_dir, pack);
	if (access(pack_dir, R_OK) != 0) {
		fprintf(stderr, "mechsim: no such sound pack '%s' in %s\n", pack, g_data_dir);
		free(pack_dir);
		return 1;
	}

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (load_pack(pack_dir) != 0) {
		free(pack_dir);
		return 1;
	}
	free(pack_dir);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double load_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;

	int any = 0;
	for (int i = 0; i < MAX_KEYS; i++)
		if (g_press[i] || g_release[i]) { any = 1; break; }
	if (!any && g_generic_n == 0) {
		fprintf(stderr, "mechsim: pack '%s' yielded no usable sounds\n", pack);
		return 1;
	}

	printf("pack: %s  volume: %d%%  load: %.0f ms  files: %zu",
	       pack, volume, load_ms, g_cache_n);
	if (g_missing)
		printf("  (%zu referenced files absent)", g_missing);
	printf("\n");
	fflush(stdout);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	pthread_t ath, ith;
	if (pthread_create(&ath, NULL, audio_thread, NULL) != 0) {
		fprintf(stderr, "mechsim: cannot start audio thread\n");
		return 1;
	}
	if (pthread_create(&ith, NULL, input_thread, NULL) != 0) {
		fprintf(stderr, "mechsim: cannot start input thread\n");
		g_stop = 1;
		pthread_join(ath, NULL);
		return 1;
	}

	while (!g_stop)
		usleep(200 * 1000);

	pthread_join(ith, NULL);
	pthread_join(ath, NULL);
	return g_input_failed ? 3 : 0;
}
