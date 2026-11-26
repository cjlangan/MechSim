/* minijson - a tiny read-only JSON parser, enough for MechSim sound pack configs.
 * Avoids a json-c dependency. ~200 lines, no allocations beyond the tree. */
#ifndef MINIJSON_H
#define MINIJSON_H

#include <stddef.h>

typedef enum { MJ_NULL, MJ_BOOL, MJ_NUM, MJ_STR, MJ_ARR, MJ_OBJ } mj_type;

typedef struct mj_val mj_val;
struct mj_val {
	mj_type type;
	double num;
	int boolean;
	char *str;
	/* array + object payload */
	size_t n;
	char **keys;      /* objects only */
	mj_val **items;   /* array elements, or object values */
};

mj_val *mj_parse(const char *text);
void mj_free(mj_val *v);

mj_val *mj_get(const mj_val *obj, const char *key);
const char *mj_str(const mj_val *v);
double mj_num(const mj_val *v, double dflt);
int mj_bool(const mj_val *v, int dflt);
size_t mj_len(const mj_val *v);
mj_val *mj_at(const mj_val *v, size_t i);
const char *mj_key_at(const mj_val *obj, size_t i);

#endif
