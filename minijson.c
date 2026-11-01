#include "minijson.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>

struct parser {
	const char *p;
};

static void skip_ws(struct parser *ps)
{
	while (*ps->p && (unsigned char)*ps->p <= 0x20)
		ps->p++;
}

static mj_val *parse_value(struct parser *ps);

static mj_val *new_val(mj_type t)
{
	mj_val *v = calloc(1, sizeof(*v));
	if (v)
		v->type = t;
	return v;
}

void mj_free(mj_val *v)
{
	if (!v)
		return;
	for (size_t i = 0; i < v->n; i++) {
		if (v->items)
			mj_free(v->items[i]);
		if (v->keys)
			free(v->keys[i]);
	}
	free(v->items);
	free(v->keys);
	free(v->str);
	free(v);
}

static int push(mj_val *v, char *key, mj_val *item)
{
	mj_val **ni = realloc(v->items, (v->n + 1) * sizeof(*ni));
	if (!ni)
		return 0;
	v->items = ni;
	if (v->type == MJ_OBJ) {
		char **nk = realloc(v->keys, (v->n + 1) * sizeof(*nk));
		if (!nk)
			return 0;
		v->keys = nk;
		v->keys[v->n] = key;
	}
	v->items[v->n] = item;
	v->n++;
	return 1;
}

/* Decode one JSON string literal. Returns malloc'd, unescaped text. */
static char *parse_string(struct parser *ps)
{
	if (*ps->p != '"')
		return NULL;
	ps->p++;

	size_t cap = 32, len = 0;
	char *out = malloc(cap);
	if (!out)
		return NULL;

	while (*ps->p && *ps->p != '"') {
		unsigned char c = (unsigned char)*ps->p++;
		if (c == '\\') {
			switch (*ps->p) {
			case 'n': c = '\n'; ps->p++; break;
			case 't': c = '\t'; ps->p++; break;
			case 'r': c = '\r'; ps->p++; break;
			case 'b': c = '\b'; ps->p++; break;
			case 'f': c = '\f'; ps->p++; break;
			case '"': c = '"'; ps->p++; break;
			case '\\': c = '\\'; ps->p++; break;
			case '/': c = '/'; ps->p++; break;
			case 'u': {
				/* Only ASCII-range escapes matter for file names. */
				unsigned cp = 0;
				ps->p++;
				for (int i = 0; i < 4 && isxdigit((unsigned char)*ps->p); i++, ps->p++)
					cp = cp * 16 + (unsigned)(isdigit((unsigned char)*ps->p) ?
						*ps->p - '0' : (tolower((unsigned char)*ps->p) - 'a' + 10));
				c = cp < 0x80 ? (unsigned char)cp : '?';
				break;
			}
			default: ps->p++; break;
			}
		}
		if (len + 1 >= cap) {
			cap *= 2;
			char *no = realloc(out, cap);
			if (!no) { free(out); return NULL; }
			out = no;
		}
		out[len++] = (char)c;
	}
	if (*ps->p == '"')
		ps->p++;
	out[len] = '\0';
	return out;
}

static mj_val *parse_array(struct parser *ps)
{
	mj_val *v = new_val(MJ_ARR);
	if (!v)
		return NULL;
	ps->p++; /* [ */
	skip_ws(ps);
	if (*ps->p == ']') { ps->p++; return v; }
	for (;;) {
		mj_val *item = parse_value(ps);
		if (!item || !push(v, NULL, item)) { mj_free(item); mj_free(v); return NULL; }
		skip_ws(ps);
		if (*ps->p == ',') { ps->p++; skip_ws(ps); continue; }
		if (*ps->p == ']') { ps->p++; break; }
		mj_free(v);
		return NULL;
	}
	return v;
}

static mj_val *parse_object(struct parser *ps)
{
	mj_val *v = new_val(MJ_OBJ);
	if (!v)
		return NULL;
	ps->p++; /* { */
	skip_ws(ps);
	if (*ps->p == '}') { ps->p++; return v; }
	for (;;) {
		char *key = parse_string(ps);
		if (!key) { mj_free(v); return NULL; }
		skip_ws(ps);
		if (*ps->p != ':') { free(key); mj_free(v); return NULL; }
		ps->p++;
		skip_ws(ps);
		mj_val *item = parse_value(ps);
		if (!item || !push(v, key, item)) { free(key); mj_free(item); mj_free(v); return NULL; }
		skip_ws(ps);
		if (*ps->p == ',') { ps->p++; skip_ws(ps); continue; }
		if (*ps->p == '}') { ps->p++; break; }
		mj_free(v);
		return NULL;
	}
	return v;
}

static mj_val *parse_value(struct parser *ps)
{
	skip_ws(ps);
	switch (*ps->p) {
	case '{': return parse_object(ps);
	case '[': return parse_array(ps);
	case '"': {
		mj_val *v = new_val(MJ_STR);
		if (!v) return NULL;
		v->str = parse_string(ps);
		if (!v->str) { free(v); return NULL; }
		return v;
	}
	case 't':
	case 'f': {
		mj_val *v = new_val(MJ_BOOL);
		if (!v) return NULL;
		v->boolean = (*ps->p == 't');
		while (isalpha((unsigned char)*ps->p)) ps->p++;
		return v;
	}
	case 'n': {
		mj_val *v = new_val(MJ_NULL);
		while (isalpha((unsigned char)*ps->p)) ps->p++;
		return v;
	}
	default: {
		char *end = NULL;
		double d = strtod(ps->p, &end);
		if (end == ps->p)
			return NULL;
		ps->p = end;
		mj_val *v = new_val(MJ_NUM);
		if (!v) return NULL;
		v->num = d;
		return v;
	}
	}
}

mj_val *mj_parse(const char *text)
{
	struct parser ps = { text };
	return parse_value(&ps);
}

mj_val *mj_get(const mj_val *obj, const char *key)
{
	if (!obj || obj->type != MJ_OBJ)
		return NULL;
	for (size_t i = 0; i < obj->n; i++)
		if (obj->keys[i] && strcmp(obj->keys[i], key) == 0)
			return obj->items[i];
	return NULL;
}

const char *mj_str(const mj_val *v)
{
	return (v && v->type == MJ_STR) ? v->str : NULL;
}

double mj_num(const mj_val *v, double dflt)
{
	return (v && v->type == MJ_NUM) ? v->num : dflt;
}

int mj_bool(const mj_val *v, int dflt)
{
	if (!v) return dflt;
	if (v->type == MJ_BOOL) return v->boolean;
	if (v->type == MJ_NUM) return v->num != 0;
	return dflt;
}

size_t mj_len(const mj_val *v)
{
	return v ? v->n : 0;
}

mj_val *mj_at(const mj_val *v, size_t i)
{
	return (v && i < v->n) ? v->items[i] : NULL;
}

const char *mj_key_at(const mj_val *obj, size_t i)
{
	return (obj && obj->keys && i < obj->n) ? obj->keys[i] : NULL;
}
