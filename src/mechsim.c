#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/inotify.h>
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
#define MAX_VOICES    64
#define MAX_DEVICES   64
#define MAX_GENERIC   5
#define MAX_CACHE     512
#define NAME_LEN      256

/* Wheel clicks have no evdev key code of their own; give them synthetic ones
 * well above the real BTN_* range so a pack can map them numerically. */
#define CODE_WHEEL_UP    1001
#define CODE_WHEEL_DOWN  1002
#define CODE_WHEEL_LEFT  1003
#define CODE_WHEEL_RIGHT 1004

/* A view of PCM data at OUT_RATE. frames == 0 means "no sound". Sample data is
 * never freed: it lives exactly as long as the process. */
typedef struct {
	const int16_t *data;
	size_t frames;
	int ch;
} sample_t;

typedef struct {
	sample_t smp;
	size_t pos;
} voice_t;

typedef struct {
	char pack[NAME_LEN];
	char mousepack[NAME_LEN];	/* empty: borrow clicks from the keyboard pack */
	int volume;
	int mouse;
	int notify;
} config_t;

static char *g_config_path;
static char *g_state_path;	/* locked by the running player; holds its settings */
static char *g_lock_path;	/* serialises commands that change state */
static char *g_log_path;
static char *g_audio_dir;
static char *g_mouse_dir;
static int g_cmd_lock = -1;

/* ---------------------------------------------------------------- utilities */

static void die(const char *what)
{
	fprintf(stderr, "mechsim: %s\n", what);
	exit(1);
}

static void *xmalloc(size_t n)
{
	void *p = malloc(n);
	if (!p)
		die("out of memory");
	return p;
}

__attribute__((format(printf, 1, 2)))
static char *format(const char *fmt, ...)
{
	char *s;
	va_list ap;
	va_start(ap, fmt);
	int n = vasprintf(&s, fmt, ap);
	va_end(ap);
	if (n < 0)
		die("out of memory");
	return s;
}

static char *path_join(const char *dir, const char *name)
{
	return name[0] == '/' ? format("%s", name) : format("%s/%s", dir, name);
}

static int is_dir(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* ------------------------------------------------------------------- config */

enum { CF_PACK, CF_MOUSEPACK, CF_VOLUME, CF_MOUSE, CF_NOTIFY, CF_COUNT };

static const char *const CF_NAMES[CF_COUNT] = {
	"PACK", "MOUSEPACK", "VOLUME", "MOUSE", "NOTIFY"
};

static const char DEFAULT_CONFIG[] =
	"# mechsim settings. Change them with mechsim commands, or edit this file\n"
	"# and run \"mechsim rest\" to apply.\n"
	"\n"
	"# Keyboard sound pack (see \"mechsim packs\").\n"
	"PACK=eg-oreo\n"
	"# Separate pack for mouse clicks. Empty borrows from the keyboard pack.\n"
	"MOUSEPACK=\"\"\n"
	"# 0-100\n"
	"VOLUME=50\n"
	"# 0 ignores mouse buttons and the wheel.\n"
	"MOUSE=1\n"
	"# 0 suppresses desktop notifications.\n"
	"NOTIFY=1\n";

/* If line is "KEY=value" for a known key, return the key and point at value. */
static int match_setting(const char *line, const char **value)
{
	line += strspn(line, " \t");
	if (strncmp(line, "export ", 7) == 0)
		line += 7;
	for (int k = 0; k < CF_COUNT; k++) {
		size_t n = strlen(CF_NAMES[k]);
		if (strncmp(line, CF_NAMES[k], n) == 0 && line[n] == '=') {
			*value = line + n + 1;
			return k;
		}
	}
	return -1;
}

/* Returns -1 unless s is a plain non-negative number; large values cap at 101. */
static int parse_volume(const char *s)
{
	if (!*s || s[strspn(s, "0123456789")])
		return -1;
	long v = strtol(s, NULL, 10);
	return v > 100 ? 101 : (int)v;
}

static int parse_bool(const char *s)
{
	return strcmp(s, "0") && strcmp(s, "off") && strcmp(s, "no") && strcmp(s, "false");
}

/* The file is parsed, never executed, but accepts the shell-style quoting and
 * trailing comments that existing config files use. */
static void read_settings(FILE *f, config_t *c)
{
	char *line = NULL;
	size_t cap = 0;

	while (getline(&line, &cap, f) > 0) {
		const char *raw;
		int key = match_setting(line, &raw);
		if (key < 0)
			continue;

		char val[NAME_LEN];
		int quoted = *raw == '"' || *raw == '\'';
		const char closing[] = { *raw, '\n', 0 };
		size_t n = strcspn(raw + quoted, quoted ? closing : " \t\r\n");
		snprintf(val, sizeof(val), "%.*s", (int)n, raw + quoted);

		switch (key) {
		case CF_PACK:
			if (*val)
				snprintf(c->pack, sizeof(c->pack), "%s", val);
			break;
		case CF_MOUSEPACK:
			snprintf(c->mousepack, sizeof(c->mousepack), "%s", val);
			break;
		case CF_VOLUME: {
			int v = parse_volume(val);
			if (v < 0)
				fprintf(stderr, "mechsim: ignoring bad VOLUME in config: %s\n", val);
			else
				c->volume = v > 100 ? 100 : v;
			break;
		}
		case CF_MOUSE:
			c->mouse = parse_bool(val);
			break;
		case CF_NOTIFY:
			c->notify = parse_bool(val);
			break;
		}
	}
	free(line);
}

static config_t load_config(void)
{
	config_t c = { .pack = "eg-oreo", .volume = 50, .mouse = 1, .notify = 1 };
	FILE *f = fopen(g_config_path, "r");
	if (f) {
		read_settings(f, &c);
		fclose(f);
	}
	return c;
}

static char *format_setting(const config_t *c, int key)
{
	if (key == CF_PACK || key == CF_MOUSEPACK) {
		const char *v = key == CF_PACK ? c->pack : c->mousepack;
		int plain = *v && !v[strspn(v, "abcdefghijklmnopqrstuvwxyz"
				"ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-+")];
		return format(plain ? "%s=%s\n" : "%s=\"%s\"\n", CF_NAMES[key], v);
	}
	int v = key == CF_VOLUME ? c->volume : key == CF_MOUSE ? c->mouse : c->notify;
	return format("%s=%d\n", CF_NAMES[key], v);
}

/* Write a commented default config if there is none yet. */
static int ensure_config(void)
{
	char *dir = format("%s", g_config_path);
	for (char *p = strchr(dir + 1, '/'); p; p = strchr(p + 1, '/')) {
		*p = 0;
		mkdir(dir, 0755);
		*p = '/';
	}
	free(dir);

	int fd = open(g_config_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (fd < 0)
		return errno == EEXIST ? 0 : -1;
	ssize_t len = (ssize_t)strlen(DEFAULT_CONFIG);
	int ok = write(fd, DEFAULT_CONFIG, (size_t)len) == len;
	return close(fd) == 0 && ok ? 0 : -1;
}

/* Rewrite only the settings named in the keys bitmask, keeping everything else
 * (comments, order, unknown lines). Replaced atomically; symlinks survive. */
static int save_config(const config_t *c, unsigned keys)
{
	char *real = NULL, *tmp = NULL, *line = NULL;
	FILE *in = NULL, *out = NULL;
	struct stat st;
	size_t cap = 0;
	unsigned seen = 0;
	int newline = 1, rc = -1;

	if (ensure_config() != 0 || !(real = realpath(g_config_path, NULL)))
		goto done;
	tmp = format("%s.tmp", real);
	if (!(in = fopen(real, "r")) || fstat(fileno(in), &st) != 0 ||
	    !(out = fopen(tmp, "w")) || fchmod(fileno(out), st.st_mode & 0777) != 0)
		goto done;

	ssize_t len;
	while ((len = getline(&line, &cap, in)) > 0) {
		const char *value;
		int key = match_setting(line, &value);
		if (key >= 0 && (keys & (1u << key))) {
			char *s = format_setting(c, key);
			fputs(s, out);
			free(s);
			seen |= 1u << key;
			newline = 1;
		} else {
			fputs(line, out);
			newline = line[len - 1] == '\n';
		}
	}
	for (int key = 0; key < CF_COUNT; key++) {
		if (!(keys & ~seen & (1u << key)))
			continue;
		char *s = format_setting(c, key);
		fprintf(out, "%s%s", newline ? "" : "\n", s);
		free(s);
		newline = 1;
	}

	int bad = ferror(in) || ferror(out);
	bad |= fclose(out) != 0;
	out = NULL;
	if (!bad && rename(tmp, real) == 0)
		rc = 0;
done:
	if (rc != 0) {
		fprintf(stderr, "mechsim: cannot save %s: %s\n", g_config_path, strerror(errno));
		if (tmp)
			unlink(tmp);
	}
	if (in)
		fclose(in);
	if (out)
		fclose(out);
	free(line);
	free(tmp);
	free(real);
	return rc;
}

/* -------------------------------------------------------------- directories */

/* $env if set, else the first of: next to the binary, the matching share/
 * directory of its install prefix, /usr/local/share, /usr/share. */
static char *find_data_dir(const char *env, const char *leaf)
{
	const char *override = getenv(env);
	if (override && *override)
		return format("%s", override);

	char exe[4096];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	exe[n > 0 ? n : 0] = 0;
	char *slash = strrchr(exe, '/');
	if (slash)
		*slash = 0;

	char *dirs[] = {
		format("%s/%s", exe, leaf),
		format("%s/../share/mechsim/%s", exe, leaf),
		format("/usr/local/share/mechsim/%s", leaf),
		format("/usr/share/mechsim/%s", leaf),
	};
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
		if (is_dir(dirs[i]))
			return dirs[i];
	return dirs[0];
}

static void init_paths(void)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && *xdg)
		g_config_path = format("%s/mechsim/config", xdg);
	else if (home && *home)
		g_config_path = format("%s/.config/mechsim/config", home);
	else
		die("HOME is not set");

	g_audio_dir = find_data_dir("MECHSIM_AUDIO_DIR", "audio");
	g_mouse_dir = find_data_dir("MECHSIM_MOUSE_DIR", "mousepacks");
}

static void init_runtime_paths(void)
{
	const char *env = getenv("XDG_RUNTIME_DIR");
	char *dir = env && *env ? format("%s", env)
				: format("/run/user/%u", (unsigned)getuid());
	if (!is_dir(dir)) {
		/* /tmp is shared, so insist on a private directory of our own. */
		free(dir);
		dir = format("/tmp/mechsim-%u", (unsigned)getuid());
		mkdir(dir, 0700);
		struct stat st;
		if (lstat(dir, &st) != 0 || !S_ISDIR(st.st_mode) ||
		    st.st_uid != getuid() || (st.st_mode & 077))
			die("no safe runtime directory (set XDG_RUNTIME_DIR)");
	}
	g_state_path = format("%s/mechsim.pid", dir);
	g_lock_path = format("%s/mechsim.lock", dir);
	g_log_path = format("%s/mechsim.log", dir);
	free(dir);
}

/* A pack is a directory holding a config.json. */
static int pack_exists(const char *dir, const char *name)
{
	if (!*name || name[0] == '.' || strpbrk(name, "/\"\n"))
		return 0;
	char *cfg = format("%s/%s/config.json", dir, name);
	int ok = access(cfg, R_OK) == 0;
	free(cfg);
	return ok;
}

/* Sorted names of the packs in dir. */
static size_t find_packs(const char *dir, char ***names)
{
	struct dirent **ent = NULL;
	int total = scandir(dir, &ent, NULL, alphasort);
	size_t n = 0;

	*names = xmalloc((size_t)(total > 0 ? total : 1) * sizeof(**names));
	for (int i = 0; i < total; i++) {
		if (pack_exists(dir, ent[i]->d_name))
			(*names)[n++] = format("%s", ent[i]->d_name);
		free(ent[i]);
	}
	free(ent);
	return n;
}

/* List packs, marking the configured one and the one actually playing. */
static size_t print_packs(FILE *out, const char *dir, const char *selected,
			  const char *playing)
{
	char **names;
	size_t n = find_packs(dir, &names);
	for (size_t i = 0; i < n; i++) {
		fprintf(out, "  %c %s%s\n", strcmp(names[i], selected) ? ' ' : '*', names[i],
			playing && strcmp(names[i], playing) == 0 ? "  (playing)" : "");
		free(names[i]);
	}
	free(names);
	if (!n)
		fprintf(out, "no packs found in %s\n", dir);
	return n;
}

/* ------------------------------------------------------------------- sounds */

static sample_t g_press[MAX_KEYS];
static sample_t g_release[MAX_KEYS];
static sample_t g_generic[MAX_GENERIC];
static size_t g_generic_n;
static sample_t g_generic_release;
static sample_t g_mouse_press;
static sample_t g_mouse_release;
static size_t g_missing;
static int g_loaded;

static const int16_t *resample_linear(const int16_t *in, size_t in_frames, int ch,
				      int in_rate, size_t *out_frames)
{
	double step = (double)in_rate / (double)OUT_RATE;
	size_t out_n = (size_t)((double)in_frames / step) + 1;
	int16_t *out = xmalloc(out_n * (size_t)ch * sizeof(int16_t));

	for (size_t i = 0; i < out_n; i++) {
		double src = (double)i * step;
		size_t i0 = (size_t)src;
		if (i0 >= in_frames)
			i0 = in_frames - 1;
		size_t i1 = i0 + 1 < in_frames ? i0 + 1 : i0;
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

static sample_t load_file(const char *path)
{
	sample_t s = { 0 };
	SF_INFO info;
	memset(&info, 0, sizeof(info));
	SNDFILE *sf = sf_open(path, SFM_READ, &info);
	if (!sf)
		return s;
	if (info.frames <= 0 || info.channels <= 0 || info.samplerate <= 0) {
		sf_close(sf);
		return s;
	}

	size_t in_ch = (size_t)info.channels;
	int16_t *raw = xmalloc((size_t)info.frames * in_ch * sizeof(int16_t));
	sf_count_t got = sf_readf_short(sf, raw, info.frames);
	sf_close(sf);
	if (got <= 0) {
		free(raw);
		return s;
	}

	/* Keep only the first two channels. */
	if (in_ch > 2) {
		for (size_t i = 0; i < (size_t)got; i++) {
			raw[i * 2 + 0] = raw[i * in_ch + 0];
			raw[i * 2 + 1] = raw[i * in_ch + 1];
		}
	}
	s.ch = in_ch > 2 ? 2 : (int)in_ch;

	if (info.samplerate == OUT_RATE) {
		s.data = raw;
		s.frames = (size_t)got;
	} else {
		s.data = resample_linear(raw, (size_t)got, s.ch, info.samplerate, &s.frames);
		free(raw);
	}
	return s;
}

/* Load a pack file, sharing the data between entries that name the same one. */
static sample_t load_cached(const char *dir, const char *file)
{
	static struct { char *path; sample_t smp; } cache[MAX_CACHE];
	static size_t n;
	char *path = path_join(dir, file);

	for (size_t i = 0; i < n; i++) {
		if (strcmp(cache[i].path, path) == 0) {
			free(path);
			return cache[i].smp;
		}
	}
	sample_t s = load_file(path);
	/* Sound packs in the wild reference files they do not ship; that is a
	 * pack defect, not a fatal error. */
	if (!s.frames)
		g_missing++;
	if (s.frames && n < MAX_CACHE) {
		cache[n].path = path;
		cache[n++].smp = s;
	} else {
		free(path);
	}
	return s;
}

static int is_mouse(int code)
{
	return (code >= BTN_MOUSE && code <= BTN_TASK) ||
	       (code >= CODE_WHEEL_UP && code <= CODE_WHEEL_RIGHT);
}

/* Pack keys are numeric evdev codes (MechSim) or names (MechVibes V2), either
 * one optionally suffixed "-up" or "_up" for the release sound. */
static int key_name_to_code(const char *k, int *is_release)
{
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
	char base[64];
	snprintf(base, sizeof(base), "%s", k);

	size_t len = strlen(base);
	*is_release = len > 3 && (strcmp(base + len - 3, "-up") == 0 ||
				  strcmp(base + len - 3, "_up") == 0);
	if (*is_release)
		base[len - 3] = 0;

	if (base[0] >= '0' && base[0] <= '9')
		return atoi(base);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (strcmp(base, names[i].name) == 0)
			return names[i].code;
	return -1;
}

static void map_sound(int code, int release, sample_t s)
{
	if (code >= 0 && code < MAX_KEYS && s.frames)
		(release ? g_release : g_press)[code] = s;
}

/* A view of [start_ms, start_ms + dur_ms) inside an already-loaded sample. */
static sample_t slice_ms(sample_t all, double start_ms, double dur_ms)
{
	sample_t s = { 0 };
	double start = start_ms * OUT_RATE / 1000.0;
	double len = dur_ms * OUT_RATE / 1000.0;

	if (start < 0 || len < 1 || start >= (double)all.frames)
		return s;
	size_t first = (size_t)start;
	s.data = all.data + first * (size_t)all.ch;
	s.frames = len < (double)(all.frames - first) ? (size_t)len : all.frames - first;
	s.ch = all.ch;
	return s;
}

/* The generic key sound: one file, or a "name{0-4}.wav" / "name%d.wav" pattern
 * naming numbered variants to pick from at random. */
static void load_generic(const char *dir, const char *pattern)
{
	const char *start = strchr(pattern, '{');
	const char *end = start ? strchr(start, '}') : NULL;

	if (end) {
		end++;
	} else if ((start = strstr(pattern, "%d"))) {
		end = start + 2;
	} else {
		sample_t s = load_cached(dir, pattern);
		if (s.frames)
			g_generic[g_generic_n++] = s;
		return;
	}

	for (int i = 0; i < MAX_GENERIC; i++) {
		char *name = format("%.*s%d%s", (int)(start - pattern), pattern, i, end);
		char *path = path_join(dir, name);
		sample_t s = { 0 };
		if (access(path, R_OK) == 0)
			s = load_cached(dir, name);
		free(path);
		free(name);
		if (!s.frames)
			break;
		g_generic[g_generic_n++] = s;
	}
}

/* MechVibes V2 (definitions with timing pairs), used by the mouse pack format.
 * Timing is [[press_start, press_end]] with an optional second release pair. */
static void load_v2(const char *dir, mj_val *root, int mouse_only)
{
	const char *audio = mj_str(mj_get(root, "audio_file"));
	mj_val *defs = mj_get(root, "definitions");
	if (!defs)
		defs = mj_get(root, "defs");

	for (size_t i = 0; defs && i < mj_len(defs); i++) {
		const char *name = mj_key_at(defs, i);
		mj_val *entry = mj_at(defs, i);
		if (!name || !entry)
			continue;

		int is_rel = 0;
		int code = key_name_to_code(name, &is_rel);
		const char *own = mj_str(mj_get(entry, "audio_file"));
		const char *file = own ? own : audio;
		mj_val *timing = mj_get(entry, "timing");
		if (code < 0 || !file || !timing || (mouse_only && !is_mouse(code)))
			continue;

		sample_t base = load_cached(dir, file);
		for (size_t seg = 0; seg < mj_len(timing) && seg < 2; seg++) {
			mj_val *pair = mj_at(timing, seg);
			if (!pair || mj_len(pair) < 2)
				continue;
			double a = mj_num(mj_at(pair, 0), 0);
			double b = mj_num(mj_at(pair, 1), 0);
			map_sound(code, is_rel || seg == 1, slice_ms(base, a, b - a));
		}
	}
}

/* MechVibes V1: "multi" maps each key to a file, "single" to [start, length]
 * within one file. With mouse_only, keyboard entries are left untouched. */
static int load_v1(const char *dir, mj_val *root, int mouse_only)
{
	const char *type = mj_str(mj_get(root, "key_define_type"));
	int multi = type && strcmp(type, "multi") == 0;
	const char *sound = mj_str(mj_get(root, "sound"));
	const char *soundup = mj_str(mj_get(root, "soundup"));
	sample_t all = { 0 };

	if (!multi) {
		if (sound)
			all = load_cached(dir, sound);
		if (!all.frames) {
			fprintf(stderr, "mechsim: %s: missing or unreadable \"sound\" file\n", dir);
			return -1;
		}
	} else if (!mouse_only) {
		if (sound)
			load_generic(dir, sound);
		if (soundup)
			g_generic_release = load_cached(dir, soundup);
	}

	mj_val *defs = mj_get(root, "defines");
	for (size_t i = 0; defs && i < mj_len(defs); i++) {
		const char *k = mj_key_at(defs, i);
		mj_val *v = mj_at(defs, i);
		if (!k || !v)
			continue;
		int is_rel = 0;
		int code = key_name_to_code(k, &is_rel);
		if (code < 0 || (mouse_only && !is_mouse(code)))
			continue;

		sample_t s = { 0 };
		if (multi) {
			const char *file = mj_str(v);
			if (file)
				s = load_cached(dir, file);
		} else if (mj_len(v) >= 2) {
			s = slice_ms(all, mj_num(mj_at(v, 0), 0), mj_num(mj_at(v, 1), 0));
		}
		map_sound(code, is_rel, s);
	}
	return 0;
}

static char *read_whole_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	long sz = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
	if (sz < 0) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	char *buf = xmalloc((size_t)sz + 1);
	buf[fread(buf, 1, (size_t)sz, f)] = 0;
	fclose(f);
	return buf;
}

static int load_pack(const char *packs_dir, const char *name, int mouse_only)
{
	char *dir = path_join(packs_dir, name);
	char *cfg = path_join(dir, "config.json");
	char *text = read_whole_file(cfg);
	mj_val *root = text ? mj_parse(text) : NULL;
	int rc = -1;

	if (!root) {
		fprintf(stderr, "mechsim: %s: %s\n", cfg, text ? "invalid JSON" : "cannot read");
	} else if (mj_get(root, "definition_method") ||
		   (mj_get(root, "definitions") && !mj_get(root, "defines"))) {
		load_v2(dir, root, mouse_only);
		rc = 0;
	} else {
		rc = load_v1(dir, root, mouse_only);
	}

	if (root)
		mj_free(root);
	free(text);
	free(cfg);
	free(dir);
	return rc;
}

/* Packs are mostly keyboard-only, so give unmapped mouse buttons a
 * representative click rather than leaving them silent. */
static void pick_mouse_fallback(void)
{
	for (int c = BTN_MOUSE; c <= BTN_TASK; c++) {
		if (!g_mouse_press.frames)
			g_mouse_press = g_press[c];
		if (!g_mouse_release.frames)
			g_mouse_release = g_release[c];
	}
	if (!g_mouse_press.frames)
		g_mouse_press = g_generic_n ? g_generic[0] :
				g_press[KEY_SPACE].frames ? g_press[KEY_SPACE] : g_press[KEY_A];
	if (!g_mouse_release.frames)
		g_mouse_release = g_generic_release;
}

/* Load everything c asks for. A broken mouse pack is dropped from c with a
 * warning; a broken keyboard pack is an error. */
static int load_sounds(config_t *c)
{
	if (g_loaded)
		return 0;

	memset(g_press, 0, sizeof(g_press));
	memset(g_release, 0, sizeof(g_release));
	g_generic_n = 0;
	g_generic_release = g_mouse_press = g_mouse_release = (sample_t){ 0 };
	g_missing = 0;

	if (!pack_exists(g_audio_dir, c->pack)) {
		fprintf(stderr, "mechsim: no such sound pack: %s\navailable:\n", c->pack);
		print_packs(stderr, g_audio_dir, c->pack, NULL);
		return -1;
	}
	if (load_pack(g_audio_dir, c->pack, 0) != 0)
		return -1;

	int usable = g_generic_n > 0;
	for (int i = 0; i < MAX_KEYS && !usable; i++)
		usable = g_press[i].frames || g_release[i].frames;
	if (!usable) {
		fprintf(stderr, "mechsim: pack '%s' yielded no usable sounds\n", c->pack);
		return -1;
	}

	if (c->mouse && c->mousepack[0] &&
	    (!pack_exists(g_mouse_dir, c->mousepack) ||
	     load_pack(g_mouse_dir, c->mousepack, 1) != 0)) {
		fprintf(stderr, "mechsim: mouse pack '%s' unavailable, borrowing clicks from %s\n",
			c->mousepack, c->pack);
		c->mousepack[0] = 0;
	}
	pick_mouse_fallback();

	if (g_missing)
		fprintf(stderr, "mechsim: note: %zu referenced sound file(s) could not be loaded\n",
			g_missing);
	g_loaded = 1;
	return 0;
}

/* -------------------------------------------------------------------- audio */

static pthread_mutex_t g_mix_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mix_cond = PTHREAD_COND_INITIALIZER;
static voice_t g_voices[MAX_VOICES];
static float g_gain;
static int g_trace;	/* MECHSIM_TRACE: log timestamps for latency measurement */

static void trace(const char *tag)
{
	if (!g_trace)
		return;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("%s %.3f\n", tag, (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6);
	fflush(stdout);
}

static size_t frames_left(const voice_t *v)
{
	return v->smp.frames - v->pos;
}

static void play_sample(sample_t s)
{
	if (!s.frames)
		return;

	pthread_mutex_lock(&g_mix_lock);
	/* Take an idle voice, or steal the one closest to finishing. */
	voice_t *v = &g_voices[0];
	for (int i = 1; i < MAX_VOICES; i++)
		if (frames_left(&g_voices[i]) < frames_left(v))
			v = &g_voices[i];
	v->smp = s;
	v->pos = 0;
	/* Wake the writer so the first period carrying this sound goes out at
	 * once, instead of waiting for the next tick of a free-running loop. */
	pthread_cond_signal(&g_mix_cond);
	pthread_mutex_unlock(&g_mix_lock);
}

/* Mix the next period. Returns 0 if nothing is playing. Caller holds the lock. */
static int mix_period(float *mix)
{
	int active = 0;
	memset(mix, 0, PERIOD_FRAMES * OUT_CH * sizeof(*mix));

	for (int i = 0; i < MAX_VOICES; i++) {
		voice_t *v = &g_voices[i];
		size_t n = frames_left(v);
		if (!n)
			continue;
		if (n > PERIOD_FRAMES)
			n = PERIOD_FRAMES;
		if (v->pos == 0)
			trace("MIXST");

		size_t ch = (size_t)v->smp.ch;
		const int16_t *src = v->smp.data + v->pos * ch;
		for (size_t f = 0; f < n; f++) {
			mix[f * 2 + 0] += (float)src[f * ch];
			mix[f * 2 + 1] += (float)src[f * ch + ch - 1];
		}
		v->pos += n;
		active = 1;
	}
	return active;
}

/* The audio server may still be starting (at login) or restarting. */
static pa_simple *audio_open(int *err)
{
	const pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = OUT_RATE, .channels = OUT_CH };
	const pa_buffer_attr attr = {
		.maxlength = (uint32_t)-1,
		.tlength = PERIOD_FRAMES * OUT_CH * 2 * 2,
		.prebuf = (uint32_t)-1,
		.minreq = (uint32_t)-1,
		.fragsize = (uint32_t)-1,
	};

	for (int tries = 0; tries < 40; tries++) {
		pa_simple *pa = pa_simple_new(NULL, "mechsim", PA_STREAM_PLAYBACK, NULL,
					      "keyboard", &ss, NULL, &attr, err);
		if (pa)
			return pa;
		usleep(500 * 1000);
	}
	return NULL;
}

static void *audio_thread(void *arg)
{
	pa_simple *pa = arg;
	static float mix[PERIOD_FRAMES * OUT_CH];
	static int16_t out[PERIOD_FRAMES * OUT_CH];

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
	for (;;) {
		pthread_mutex_lock(&g_mix_lock);
		while (!mix_period(mix))
			pthread_cond_wait(&g_mix_cond, &g_mix_lock);
		pthread_mutex_unlock(&g_mix_lock);

		for (int i = 0; i < PERIOD_FRAMES * OUT_CH; i++) {
			float v = mix[i] * g_gain;
			if (v > 32767.0f) v = 32767.0f;
			else if (v < -32768.0f) v = -32768.0f;
			out[i] = (int16_t)lrintf(v);
		}

		int err = 0;
		if (pa_simple_write(pa, out, sizeof(out), &err) < 0) {
			fprintf(stderr, "mechsim: audio write failed: %s; reconnecting\n",
				pa_strerror(err));
			pa_simple_free(pa);
			pa = audio_open(&err);
			if (!pa) {
				fprintf(stderr, "mechsim: cannot reopen audio output: %s\n",
					pa_strerror(err));
				_exit(1);
			}
			/* Do not replay what was typed while we were away. */
			pthread_mutex_lock(&g_mix_lock);
			memset(g_voices, 0, sizeof(g_voices));
			pthread_mutex_unlock(&g_mix_lock);
		}
	}
	return NULL;
}

/* -------------------------------------------------------------------- input */

static struct pollfd g_fds[1 + MAX_DEVICES];	/* [0] watches /dev/input */
static int g_event_num[1 + MAX_DEVICES];
static int g_is_keyboard[1 + MAX_DEVICES];
static nfds_t g_nfds = 1;
static int g_mouse_on;
static int g_logging;

/* A device worth reading: anything that reports letter keys, or (with mouse
 * sounds on) a mouse button or a wheel. Mice report BTN_LEFT and no letter keys
 * at all, so a keyboard-only test would skip them entirely. */
static int device_wanted(int fd, int *is_keyboard)
{
	uint8_t key[(KEY_CNT + 7) / 8] = { 0 };
	uint8_t rel[(REL_CNT + 7) / 8] = { 0 };
	int mouse = 0;
#define HAS(bits, code) (bits[(code) / 8] & (1u << ((code) % 8)))

	*is_keyboard = 0;
	/* EVIOCGBIT returns a positive byte count on success, not 0. */
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key)), key) >= 0) {
		*is_keyboard = HAS(key, KEY_A) || HAS(key, KEY_Z) || HAS(key, KEY_SPACE);
		mouse = HAS(key, BTN_LEFT) || HAS(key, BTN_RIGHT) || HAS(key, BTN_MIDDLE) ||
			HAS(key, BTN_SIDE) || HAS(key, BTN_EXTRA);
	}
	if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel) >= 0)
		mouse |= HAS(rel, REL_WHEEL) || HAS(rel, REL_HWHEEL);
#undef HAS
	return *is_keyboard || (g_mouse_on && mouse);
}

static void log_device(nfds_t i)
{
	char name[128] = "";
	ioctl(g_fds[i].fd, EVIOCGNAME(sizeof(name) - 1), name);
	printf("capturing event%d (%s)\n", g_event_num[i], name);
	fflush(stdout);
}

/* Open any wanted device that is not already open. */
static void scan_devices(void)
{
	DIR *d = opendir("/dev/input");
	if (!d)
		return;

	struct dirent *ent;
	while (g_nfds <= MAX_DEVICES && (ent = readdir(d)) != NULL) {
		int num;
		if (sscanf(ent->d_name, "event%d", &num) != 1)
			continue;
		nfds_t i = 1;
		while (i < g_nfds && g_event_num[i] != num)
			i++;
		if (i < g_nfds)
			continue;

		char *path = path_join("/dev/input", ent->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		free(path);
		if (fd < 0)
			continue;
		int keyboard;
		if (!device_wanted(fd, &keyboard)) {
			close(fd);
			continue;
		}
		g_fds[g_nfds] = (struct pollfd){ .fd = fd, .events = POLLIN };
		g_event_num[g_nfds] = num;
		g_is_keyboard[g_nfds] = keyboard;
		if (g_logging)
			log_device(g_nfds);
		g_nfds++;
	}
	closedir(d);
}

static int open_input(int mouse)
{
	g_mouse_on = mouse;

	/* Watch for hotplug before the first scan so nothing slips between them.
	 * IN_ATTRIB catches udev granting permissions after creating the node. */
	int watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (watch >= 0 &&
	    inotify_add_watch(watch, "/dev/input", IN_CREATE | IN_ATTRIB) < 0) {
		close(watch);
		watch = -1;
	}
	g_fds[0] = (struct pollfd){ .fd = watch, .events = POLLIN };

	/* Give a Bluetooth or late-enumerating keyboard a moment, then fail
	 * loudly: a player that runs but captures nothing looks identical to a
	 * broken one. */
	for (int tries = 0; tries <= 10; tries++) {
		scan_devices();
		for (nfds_t i = 1; i < g_nfds; i++)
			if (g_is_keyboard[i])
				return 0;
		usleep(200 * 1000);
	}
	fprintf(stderr, "mechsim: no readable keyboard devices - make sure you are in the\n"
		"'input' group (sudo usermod -aG input $USER) and have logged back in\n");
	return -1;
}

static void handle_key(int code, int value)
{
	if (code < 0 || code >= MAX_KEYS || (value != 0 && value != 1))
		return;		/* value 2 is autorepeat */

	int mouse = is_mouse(code);
	/* Other BTN_* codes (touchpad contact, tablet tools) are not clicks. */
	if (mouse ? !g_mouse_on : (code >= BTN_MISC && code < KEY_OK))
		return;
	if (value)
		trace("KEYIN");

	sample_t s = value ? g_press[code] : g_release[code];
	if (!s.frames && mouse)
		s = value ? g_mouse_press : g_mouse_release;
	if (!s.frames) {
		if (!value)
			s = g_generic_release;
		else if (g_generic_n)
			s = g_generic[(size_t)rand() % g_generic_n];
	}
	play_sample(s);
}

static void read_events(int fd)
{
	struct input_event ev;
	while (read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type == EV_KEY) {
			handle_key(ev.code, ev.value);
		} else if (ev.type == EV_REL &&
			   (ev.code == REL_WHEEL || ev.code == REL_HWHEEL)) {
			/* Wheel ticks are impulses with no release event. */
			int code = ev.code == REL_WHEEL
				? (ev.value > 0 ? CODE_WHEEL_UP : CODE_WHEEL_DOWN)
				: (ev.value > 0 ? CODE_WHEEL_RIGHT : CODE_WHEEL_LEFT);
			for (int k = 0; k < abs(ev.value); k++)
				handle_key(code, 1);
		}
	}
}

static void input_loop(void)
{
	for (;;) {
		/* Without a hotplug watch, fall back to rescanning once a second. */
		int r = poll(g_fds, g_nfds, g_fds[0].fd < 0 ? 1000 : -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			perror("mechsim: poll");
			_exit(1);
		}

		/* Backwards, so removing an entry only moves one already handled. */
		for (nfds_t i = g_nfds - 1; i >= 1; i--) {
			if (g_fds[i].revents & POLLIN)
				read_events(g_fds[i].fd);
			if (g_fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
				printf("event%d went away\n", g_event_num[i]);
				fflush(stdout);
				close(g_fds[i].fd);
				g_nfds--;
				g_fds[i] = g_fds[g_nfds];
				g_event_num[i] = g_event_num[g_nfds];
				g_is_keyboard[i] = g_is_keyboard[g_nfds];
			}
		}

		if (r == 0 || (g_fds[0].revents & POLLIN)) {
			char buf[4096];
			while (g_fds[0].fd >= 0 && read(g_fds[0].fd, buf, sizeof(buf)) > 0)
				;
			scan_devices();
		}
	}
}

/* ----------------------------------------------------------- player process */

static void notify(const config_t *c, const char *title, const char *body)
{
	if (!c->notify || !getenv("DBUS_SESSION_BUS_ADDRESS"))
		return;
	if (fork() == 0) {	/* fire and forget */
		int null = open("/dev/null", O_RDWR);
		dup2(null, 0);
		dup2(null, 1);
		dup2(null, 2);
		execlp("notify-send", "notify-send", "-a", "mechsim", "-t", "1500",
		       "-i", "audio-volume-high", title, body, (char *)NULL);
		_exit(127);
	}
}

/* The running player holds a write lock on the state file, so the kernel tells
 * us whether it is alive and what its pid is: no stale pidfiles, and no risk of
 * signalling an unrelated process. Optionally reports the settings in use. */
static pid_t player_pid(config_t *running)
{
	int fd = open(g_state_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;

	struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
	pid_t pid = 0;
	if (fcntl(fd, F_GETLK, &fl) == 0 && fl.l_type != F_UNLCK && fl.l_pid > 0)
		pid = fl.l_pid;

	FILE *f = pid && running ? fdopen(fd, "r") : NULL;
	if (f) {
		*running = load_config();
		read_settings(f, running);
		fclose(f);
	} else {
		close(fd);
	}
	return pid;
}

static int stop_player(void)
{
	const int signals[] = { SIGTERM, SIGKILL };
	for (int s = 0; s < 2; s++) {
		pid_t pid = player_pid(NULL);
		if (!pid)
			return 0;
		kill(pid, signals[s]);
		for (int i = 0; i < 50; i++) {
			if (!player_pid(NULL))
				return 0;
			usleep(20 * 1000);
		}
	}
	fprintf(stderr, "mechsim: could not stop the running player\n");
	return -1;
}

/* The background process. Sounds and input devices are already open; report
 * "ok" or a reason for failure on the report pipe once audio is settled. */
static void run_player(const config_t *c, int report)
{
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	signal(SIGINT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);	/* "off" just kills us; nothing to clean up */
	signal(SIGPIPE, SIG_IGN);
	setsid();
	close(g_cmd_lock);	/* otherwise we would block every later command */

	/* Never closed: closing it would drop the lock. */
	int state = open(g_state_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
	struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET };
	if (state < 0 || fcntl(state, F_SETLK, &fl) != 0 || ftruncate(state, 0) != 0) {
		dprintf(report, "cannot lock %s: %s", g_state_path, strerror(errno));
		_exit(1);
	}
	dprintf(state, "%d\n", (int)getpid());
	for (int key = 0; key < CF_COUNT; key++) {
		char *s = format_setting(c, key);
		dprintf(state, "%s", s);
		free(s);
	}

	int null = open("/dev/null", O_RDWR | O_CLOEXEC);
	int log = open(g_log_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	dup2(null, 0);
	dup2(log >= 0 ? log : null, 1);
	dup2(log >= 0 ? log : null, 2);
	close(null);
	if (log >= 0)
		close(log);
	if (chdir("/") != 0)
		perror("mechsim: chdir");

	g_gain = (float)c->volume / 100.0f;
	g_trace = getenv("MECHSIM_TRACE") != NULL;
	srand((unsigned)time(NULL));

	int err = 0;
	pa_simple *pa = audio_open(&err);
	if (!pa) {
		dprintf(report, "cannot open audio output: %s", pa_strerror(err));
		_exit(1);
	}
	/* Discard what was typed while starting, including the Enter release. */
	for (nfds_t i = 1; i < g_nfds; i++) {
		struct input_event ev;
		while (read(g_fds[i].fd, &ev, sizeof(ev)) > 0)
			;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, audio_thread, pa) != 0) {
		dprintf(report, "cannot start audio thread");
		_exit(1);
	}
	dprintf(report, "ok");
	close(report);

	g_logging = 1;
	printf("pack: %s  volume: %d%%\n", c->pack, c->volume);
	for (nfds_t i = 1; i < g_nfds; i++)
		log_device(i);
	input_loop();
}

/* Load sounds and open devices before touching any running player, so that
 * failures are reported directly and leave it as it was. */
static int prepare(config_t *c)
{
	if (load_sounds(c) != 0) {
		notify(c, "Keyboard sounds", "sound pack failed to load");
		return -1;
	}
	if (open_input(c->mouse) != 0) {
		notify(c, "Keyboard sounds", "no input access - add yourself to the input group");
		return -1;
	}
	return 0;
}

static int start_player(const config_t *c)
{
	int p[2];
	if (pipe2(p, O_CLOEXEC) != 0)
		die("cannot create pipe");
	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0)
		die("cannot fork");
	if (pid == 0) {
		close(p[0]);
		run_player(c, p[1]);
		_exit(1);
	}
	close(p[1]);

	char msg[512];
	size_t len = 0;
	ssize_t n;
	while ((n = read(p[0], msg + len, sizeof(msg) - 1 - len)) > 0)
		len += (size_t)n;
	msg[len] = 0;
	close(p[0]);

	if (strcmp(msg, "ok") != 0) {
		fprintf(stderr, "mechsim: failed to start: %s (log: %s)\n",
			len ? msg : "player exited unexpectedly", g_log_path);
		notify(c, "Keyboard sounds", "failed to start");
		return 1;
	}
	printf("on (%s, %d%%) pid %d\n", c->pack, c->volume, (int)pid);
	char *body = format("%s - %d%%", c->pack, c->volume);
	notify(c, "Keyboard sounds on", body);
	free(body);
	return 0;
}

/* ----------------------------------------------------------------- commands */

static const char *mouse_label(const config_t *c)
{
	return !c->mouse ? "off" : c->mousepack[0] ? c->mousepack : "borrowed";
}

static int turn_on(config_t *c)
{
	pid_t pid = player_pid(NULL);
	if (pid) {
		printf("already on (pid %d)\n", (int)pid);
		return 0;
	}
	if (ensure_config() != 0)
		fprintf(stderr, "mechsim: cannot create %s: %s\n", g_config_path, strerror(errno));
	return prepare(c) == 0 ? start_player(c) : 1;
}

static int turn_off(const config_t *c)
{
	if (!player_pid(NULL)) {
		puts("already off");
		return 0;
	}
	if (stop_player() != 0)
		return 1;
	puts("off");
	notify(c, "Keyboard sounds off", "");
	return 0;
}

static int restart(config_t *c)
{
	if (!player_pid(NULL))
		return turn_on(c);
	if (prepare(c) != 0 || stop_player() != 0)
		return 1;
	return start_player(c);
}

/* Save the settings named in keys and restart the player if it is on. */
static int apply(config_t *c, unsigned keys, const char *what, const char *value)
{
	int on = player_pid(NULL) != 0;

	if ((on && prepare(c) != 0) || save_config(c, keys) != 0)
		return 1;
	printf("%s -> %s\n", what, value);
	if (!on) {
		char *body = format("%s (will apply when turned on)", value);
		notify(c, what, body);
		free(body);
		return 0;
	}
	return stop_player() == 0 ? start_player(c) : 1;
}

static int cmd_status(void)
{
	config_t c;
	pid_t pid = player_pid(&c);
	if (!pid) {
		puts("off");
		return 1;
	}
	printf("on (pid %d, pack %s, mouse %s, volume %d%%)\n",
	       (int)pid, c.pack, mouse_label(&c), c.volume);
	return 0;
}

static int cmd_packs(const config_t *c)
{
	config_t running;
	int on = player_pid(&running) != 0;
	if (!print_packs(stdout, g_audio_dir, c->pack, on ? running.pack : NULL))
		return 1;
	if (!pack_exists(g_audio_dir, c->pack))
		fprintf(stderr, "mechsim: configured pack '%s' is not installed\n", c->pack);
	return 0;
}

static int cmd_pack(config_t *c, const char *name)
{
	if (!name) {
		printf("current: %s\n\n", c->pack);
		cmd_packs(c);
		puts("\nusage: mechsim pack NAME");
		return 0;
	}
	snprintf(c->pack, sizeof(c->pack), "%s", name);
	/* Loading also validates the pack, even while the player is off. */
	if (load_sounds(c) != 0)
		return 1;
	return apply(c, 1u << CF_PACK, "pack", c->pack);
}

/* next (step 1) or prev (step -1), skipping packs that fail to load. */
static int cmd_cycle(config_t *c, int step)
{
	char **names;
	int n = (int)find_packs(g_audio_dir, &names);
	int at = step > 0 ? -1 : n;	/* if the current pack is gone */

	for (int i = 0; i < n; i++)
		if (strcmp(names[i], c->pack) == 0)
			at = i;
	for (int tries = 0; tries < n; tries++) {
		at = (at + step + n) % n;
		snprintf(c->pack, sizeof(c->pack), "%s", names[at]);
		if (load_sounds(c) == 0)
			return apply(c, 1u << CF_PACK, "pack", c->pack);
	}
	fprintf(stderr, "mechsim: no usable packs in %s\n", g_audio_dir);
	return 1;
}

static int cmd_volume(config_t *c, const char *arg)
{
	if (!arg) {
		printf("volume: %d%%\n", c->volume);
		return 0;
	}
	int v = parse_volume(arg);
	if (v < 0 || v > 100) {
		fprintf(stderr, "mechsim: volume must be 0-100\n");
		return 2;
	}
	c->volume = v;
	char *text = format("%d%%", v);
	return apply(c, 1u << CF_VOLUME, "volume", text);
}

static int cmd_mouse(config_t *c, const char *arg)
{
	if (!arg) {
		printf("mouse sounds: %s\n\nusage: mechsim mouse on|off|toggle\n", mouse_label(c));
		return 0;
	}
	if (!strcmp(arg, "on") || !strcmp(arg, "enable") || !strcmp(arg, "1")) {
		c->mouse = 1;
	} else if (!strcmp(arg, "off") || !strcmp(arg, "disable") || !strcmp(arg, "0")) {
		c->mouse = 0;
	} else if (!strcmp(arg, "toggle")) {
		c->mouse = !c->mouse;
	} else {
		fprintf(stderr, "usage: mechsim mouse on|off|toggle\n");
		return 2;
	}
	/* Load first, so the label reflects a mouse pack that had to be dropped. */
	if (c->mouse && load_sounds(c) != 0)
		return 1;
	return apply(c, 1u << CF_MOUSE, "mouse sounds", mouse_label(c));
}

static int cmd_mousepack(config_t *c, const char *name)
{
	if (!name) {
		printf("current: %s\n\n", c->mousepack[0] ? c->mousepack : "none (borrowed)");
		print_packs(stdout, g_mouse_dir, c->mousepack, NULL);
		puts("\nusage: mechsim mousepack NAME|none");
		return 0;
	}

	unsigned keys = 1u << CF_MOUSEPACK;
	if (!strcmp(name, "none") || !strcmp(name, "off")) {
		c->mousepack[0] = 0;
	} else if (pack_exists(g_mouse_dir, name)) {
		snprintf(c->mousepack, sizeof(c->mousepack), "%s", name);
		if (!c->mouse) {
			c->mouse = 1;
			keys |= 1u << CF_MOUSE;
			puts("turning mouse sounds on");
		}
	} else {
		fprintf(stderr, "mechsim: no such mouse pack: %s\navailable:\n", name);
		print_packs(stderr, g_mouse_dir, c->mousepack, NULL);
		return 1;
	}
	/* Refuse a pack that does not load rather than saving it. */
	if (c->mousepack[0] && (load_sounds(c) != 0 || !c->mousepack[0]))
		return 1;
	return apply(c, keys, "mouse pack", c->mousepack[0] ? c->mousepack : "none (borrowed)");
}

static void usage(FILE *out)
{
	fprintf(out,
		"mechsim - mechanical keyboard sounds\n\n"
		"Usage: mechsim COMMAND [ARG]\n\n"
		"  on | off | toggle     start / stop / flip the background player\n"
		"  rest                  restart (re-reads the config)\n"
		"  status                show state; exit status 0 if on, 1 if off\n"
		"  packs                 list sound packs (* selected)\n"
		"  pack [NAME]           show or switch the sound pack\n"
		"  next | prev           cycle through sound packs\n"
		"  vol [N]               show or set the volume, 0-100\n"
		"  mouse [on|off|toggle] show or set click and wheel sounds\n"
		"  mousepack [NAME|none] show or set the pack used for clicks\n"
		"  help, -h, --help      show this help\n\n"
		"Config: %s\n"
		"  PACK, MOUSEPACK, VOLUME, MOUSE, NOTIFY\n"
		"Packs:  %s  (override: MECHSIM_AUDIO_DIR)\n"
		"Mouse:  %s  (override: MECHSIM_MOUSE_DIR)\n\n"
		"Needs no root: read access to /dev/input/event* via the 'input' group.\n",
		g_config_path, g_audio_dir, g_mouse_dir);
}

static int one_of(const char *s, const char *const *list)
{
	for (; *list; list++)
		if (strcmp(s, *list) == 0)
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	static const char *const help[] = { "help", "-h", "--help", NULL };
	static const char *const plain[] = { "on", "off", "toggle", "rest", "restart",
					     "status", "packs", "list", "next", "prev", NULL };
	static const char *const with_arg[] = { "pack", "vol", "volume", "mouse",
						"mousepack", NULL };

	/* Keep our own descriptors clear of stdin, stdout and stderr. */
	int fd;
	do
		fd = open("/dev/null", O_RDWR);
	while (fd >= 0 && fd <= 2);
	if (fd > 2)
		close(fd);

	init_paths();
	const char *cmd = argc > 1 ? argv[1] : "help";
	const char *arg = argc > 2 ? argv[2] : NULL;

	if (one_of(cmd, help)) {
		usage(stdout);
		return 0;
	}
	int takes_arg = one_of(cmd, with_arg);
	if ((!takes_arg && !one_of(cmd, plain)) || argc > 2 + takes_arg) {
		fprintf(stderr, "mechsim: invalid command\n\n");
		usage(stderr);
		return 2;
	}
	init_runtime_paths();

	/* Commands that change state run one at a time. A toggle that arrives
	 * while another command is busy (a double-tapped keybind) is dropped. */
	int read_only = !strcmp(cmd, "status") || !strcmp(cmd, "packs") ||
			!strcmp(cmd, "list") || (takes_arg && !arg);
	if (!read_only) {
		g_cmd_lock = open(g_lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
		if (g_cmd_lock < 0)
			die("cannot create lock file in the runtime directory");
		if (flock(g_cmd_lock, LOCK_EX | (strcmp(cmd, "toggle") ? 0 : LOCK_NB)) != 0)
			return 0;
	}

	config_t c = load_config();

	if (!strcmp(cmd, "on"))
		return turn_on(&c);
	if (!strcmp(cmd, "off"))
		return turn_off(&c);
	if (!strcmp(cmd, "toggle"))
		return player_pid(NULL) ? turn_off(&c) : turn_on(&c);
	if (!strcmp(cmd, "rest") || !strcmp(cmd, "restart"))
		return restart(&c);
	if (!strcmp(cmd, "status"))
		return cmd_status();
	if (!strcmp(cmd, "packs") || !strcmp(cmd, "list"))
		return cmd_packs(&c);
	if (!strcmp(cmd, "next"))
		return cmd_cycle(&c, 1);
	if (!strcmp(cmd, "prev"))
		return cmd_cycle(&c, -1);
	if (!strcmp(cmd, "pack"))
		return cmd_pack(&c, arg);
	if (!strcmp(cmd, "mouse"))
		return cmd_mouse(&c, arg);
	if (!strcmp(cmd, "mousepack"))
		return cmd_mousepack(&c, arg);
	return cmd_volume(&c, arg);
}
