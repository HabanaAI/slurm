/*****************************************************************************\
 *  data.c - generic data_t structures
 *****************************************************************************
 *  Copyright (C) 2019 SchedMD LLC.
 *  Written by Nathan Rini <nate@schedmd.com>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#define _ISOC99_SOURCE	/* needed for lrint */

#include <ctype.h>
#include <math.h>

#include "src/common/data.h"
#include "src/common/list.h"
#include "src/common/log.h"
#include "src/common/read_config.h"
#include "src/common/xassert.h"
#include "src/common/xmalloc.h"
#include "src/common/xstring.h"

#define DATA_MAGIC 0x1992189F
#define DATA_LIST_MAGIC 0x1992F89F
#define DATA_LIST_NODE_MAGIC 0x1921F89F

typedef struct data_list_node_s data_list_node_t;
struct data_list_node_s {
	int magic;
	data_list_node_t *next;

	data_t *data;
	char *key; /* key for dictionary (only) */
};

/* single forward linked list */
struct data_list_s {
	int magic;
	size_t count;

	data_list_node_t *begin;
	data_list_node_t *end;
};

typedef struct {
	char *path;
	char *at;
	const char *token;
} merge_path_strings_t;

typedef struct {
	const data_t *a;
	const data_t *b;
	bool mask;
} find_dict_match_t;

typedef struct {
	size_t count;
	data_type_t match;
} convert_args_t;

static void _check_magic(const data_t *data);
static void _release(data_t *data);
static void _release_data_list_node(data_list_t *dl, data_list_node_t *dn);

extern void data_fini(void)
{
}

extern int data_init(void)
{
	return SLURM_SUCCESS;
}

static data_list_t *_data_list_new(void)
{
	data_list_t *dl = xmalloc(sizeof(*dl));
	dl->magic = DATA_LIST_MAGIC;

	log_flag(DATA, "%s: new "PRINTF_DATA_LIST_T,
		 __func__, PRINTF_DATA_LIST_T_VAL(dl));

	return dl;
}

static void _check_data_list_node_magic(const data_list_node_t *dn)
{
	xassert(dn);
	xassert(dn->magic == DATA_LIST_NODE_MAGIC);
	/* make sure not linking to self */
	xassert(dn->next != dn);
	/* key can be NULL for list, but not NULL length string */
	xassert(!dn->key || dn->key[0]);
}

static void _check_data_list_magic(const data_list_t *dl)
{
#ifndef NDEBUG
	data_list_node_t *end = NULL;

	xassert(dl);
	xassert(dl->magic == DATA_LIST_MAGIC);

	if (dl->begin) {
		/* walk forwards verify */
		int c = 0;
		data_list_node_t *i = dl->begin;

		while (i) {
			c++;
			_check_data_list_node_magic(i);
			end = i;
			i = i->next;
		}

		xassert(c == dl->count);
	}

	xassert(end == dl->end);
#endif /* !NDEBUG */
}

/* verify node is in parent list */
static void _check_data_list_node_parent(const data_list_t *dl,
					 const data_list_node_t *dn)
{
#ifndef NDEBUG
	data_list_node_t *i = dl->begin;
	while (i) {
		if (i == dn)
			return;
		i = i->next;
	}

	/* found an orphan? */
	fatal_abort("%s: unexpected orphan node", __func__);
#endif /* !NDEBUG */
}

static void _release_data_list_node(data_list_t *dl, data_list_node_t *dn)
{
	_check_data_list_magic(dl);
	_check_data_list_node_magic(dn);
	_check_data_list_node_parent(dl, dn);
	data_list_node_t *prev;

	log_flag(DATA, "%s: free "PRINTF_DATA_LIST_T,
		 __func__, PRINTF_DATA_LIST_T_VAL(dl));

	/* walk list to find new previous */
	for (prev = dl->begin; prev && prev->next != dn; ) {
		_check_data_list_node_magic(prev);
		prev = prev->next;
		if (prev)
			_check_data_list_node_magic(prev);
	}

	if (dn == dl->begin) {
		/* at the beginning */
		xassert(!prev);
		dl->begin = dn->next;

		if (dl->end == dn) {
			dl->end = NULL;
			xassert(!dn->next);
		}
	} else if (dn == dl->end) {
		/* at the end */
		xassert(!dn->next);
		xassert(prev);
		dl->end = prev;
		prev->next = NULL;
	} else {
		/* somewhere in middle */
		xassert(prev);
		xassert(prev != dn);
		xassert(prev->next == dn);
		xassert(dl->begin != dn);
		xassert(dl->end != dn);
		prev->next = dn->next;
	}

	dl->count--;
	FREE_NULL_DATA(dn->data);
	xfree(dn->key);

	dn->magic = ~DATA_LIST_NODE_MAGIC;
	xfree(dn);
}

static void _release_data_list(data_list_t *dl)
{
	data_list_node_t *n = dl->begin, *i;
#ifndef NDEBUG
	int count = 0;
	const int init_count = dl->count;
#endif

	_check_data_list_magic(dl);

	if (!n) {
		xassert(!dl->count);
		xassert(!dl->end);
		goto finish;
	}

	xassert(dl->end);

	while((i = n)) {
		n = i->next;
		_release_data_list_node(dl, i);

#ifndef NDEBUG
		count++;
#endif
	}


#ifndef NDEBUG
	xassert(count == init_count);
#endif

finish:
	dl->magic = ~DATA_LIST_MAGIC;
	xfree(dl);
}

/*
 * Create new data list node entry
 * IN d - data type to take ownership of
 * IN key - dictionary key to dup or NULL
 */
static data_list_node_t *_new_data_list_node(data_t *d, const char *key)
{
	data_list_node_t *dn = xmalloc(sizeof(*dn));
	dn->magic = DATA_LIST_NODE_MAGIC;
	_check_magic(d);

	dn->data = d;
	if (key)
		dn->key = xstrdup(key);

	log_flag(DATA, "%s: new "PRINTF_DATA_T_INDEX"->"PRINTF_DATA_LIST_NODE_T,
		 __func__, PRINTF_DATA_T_INDEX_VAL(d, key),
		 PRINTF_DATA_LIST_NODE_T_VAL(dn));

	return dn;
}

static void _data_list_append(data_list_t *dl, data_t *d, const char *key)
{
	data_list_node_t *n = _new_data_list_node(d, key);
	_check_data_list_magic(dl);
	_check_magic(d);

	if (dl->end) {
		xassert(!dl->end->next);
		_check_data_list_node_magic(dl->end);
		_check_data_list_node_magic(dl->begin);

		dl->end->next = n;
		dl->end = n;
	} else {
		xassert(!dl->count);
		dl->end = n;
		dl->begin = n;
	}

	dl->count++;

	log_flag(DATA, "%s: append "PRINTF_DATA_T_INDEX"->"PRINTF_DATA_LIST_NODE_T,
		 __func__, PRINTF_DATA_T_INDEX_VAL(d, key),
		 PRINTF_DATA_LIST_NODE_T_VAL(n));
}

static void _data_list_prepend(data_list_t *dl, data_t *d, const char *key)
{
	data_list_node_t *n = _new_data_list_node(d, key);
	_check_data_list_magic(dl);
	_check_magic(d);

	if (dl->begin) {
		_check_data_list_node_magic(dl->begin);
		n->next = dl->begin;
		dl->begin = n;
	} else {
		xassert(!dl->count);
		dl->begin = n;
		dl->end = n;
	}

	dl->count++;

	log_flag(DATA, "%s: prepend "PRINTF_DATA_T_INDEX"->"PRINTF_DATA_LIST_NODE_T,
		 __func__, PRINTF_DATA_T_INDEX_VAL(d, key),
		 PRINTF_DATA_LIST_NODE_T_VAL(n));
}

extern data_t *data_new(void)
{
	data_t *data = xmalloc(sizeof(*data));
	data->magic = DATA_MAGIC;
	data->type = DATA_TYPE_NULL;

	log_flag(DATA, "%s: new "PRINTF_DATA_T,
		 __func__, PRINTF_DATA_T_VAL(data));

	return data;
}

static void _check_magic(const data_t *data)
{
	if (!data)
		return;

	xassert(data->type > DATA_TYPE_NONE);
	xassert(data->type < DATA_TYPE_MAX);
	xassert(data->magic == DATA_MAGIC);

	if (data->type == DATA_TYPE_NULL)
		/* make sure NULL type has a NULL value */
		xassert(data->data.list_u == NULL);
	if (data->type == DATA_TYPE_LIST)
		_check_data_list_magic(data->data.list_u);
	if (data->type == DATA_TYPE_DICT)
		_check_data_list_magic(data->data.dict_u);
}

static void _release(data_t *data)
{
	_check_magic(data);

	switch (data->type) {
	case DATA_TYPE_LIST:
		_release_data_list(data->data.list_u);
		break;
	case DATA_TYPE_DICT:
		_release_data_list(data->data.dict_u);
		break;
	case DATA_TYPE_STRING:
		xfree(data->data.string_u);
		break;
	default:
		/* other types don't need to be freed */
		break;
	}

	data->type = DATA_TYPE_NONE;
	/* always zero data in debug mode */
	xassert(memset(&data->data, 0, sizeof(data->data)));
}

extern void data_free(data_t *data)
{
	if (!data)
		return;

	log_flag(DATA, "%s: free "PRINTF_DATA_T,
		 __func__, PRINTF_DATA_T_VAL(data));

	_check_magic(data);
	_release(data);

	data->magic = ~DATA_MAGIC;
	data->type = DATA_TYPE_NONE;
	xfree(data);
}

extern data_type_t data_get_type(const data_t *data)
{
	if (!data)
		return DATA_TYPE_NONE;

	_check_magic(data);

	return data->type;
}

extern data_t *data_set_float(data_t *data, double value)
{
	_check_magic(data);
	if (!data)
		return NULL;

	data->type = DATA_TYPE_FLOAT;
	data->data.float_u = value;

	log_flag(DATA, "%s: set "PRINTF_DATA_T"=%e",
		 __func__, PRINTF_DATA_T_VAL(data), value);

	return data;
}

extern data_t *data_set_null(data_t *data)
{
	_check_magic(data);
	if (!data)
		return NULL;
	_release(data);

	data->type = DATA_TYPE_NULL;
	xassert((memset(&data->data, 0, sizeof(data->data))));

	log_flag(DATA, "%s: set "PRINTF_DATA_T"=null",
		 __func__, PRINTF_DATA_T_VAL(data));

	return data;
}

extern data_t *data_set_bool(data_t *data, bool value)
{
	_check_magic(data);
	if (!data)
		return NULL;
	_release(data);

	data->type = DATA_TYPE_BOOL;
	data->data.bool_u = value;

	log_flag(DATA, "%s: set "PRINTF_DATA_T"=%s",
		 __func__, PRINTF_DATA_T_VAL(data),
		 (value ? "true" : "false"));

	return data;
}

extern data_t *data_set_int(data_t *data, int64_t value)
{
	_check_magic(data);
	if (!data)
		return NULL;
	_release(data);

	data->type = DATA_TYPE_INT_64;
	data->data.int_u = value;

	log_flag(DATA, "%s: set "PRINTF_DATA_T"=%"PRId64,
		 __func__, PRINTF_DATA_T_VAL(data), value);

	return data;
}

extern data_t *data_set_string(data_t *data, const char *value)
{
	_check_magic(data);

	if (!data)
		return NULL;
	_release(data);

	if (!value) {
		data->type = DATA_TYPE_NULL;

		log_flag(DATA, "%s: set "PRINTF_DATA_T"=null",
			 __func__, PRINTF_DATA_T_VAL(data));
		return data;
	}

	data->type = DATA_TYPE_STRING;
	data->data.string_u = xstrdup(value);

	log_flag_hex(DATA, value, strlen(value), "%s: set "PRINTF_DATA_T,
		 __func__, PRINTF_DATA_T_VAL(data), value);

	return data;
}

extern data_t *_data_set_string_own(data_t *data, char **value_ptr)
{
	_check_magic(data);

	if (!data)
		return NULL;

	if (!*value_ptr) {
		data->type = DATA_TYPE_NULL;

		log_flag(DATA, "%s: set "PRINTF_DATA_T"=null",
			 __func__, PRINTF_DATA_T_VAL(data));
		return data;
	}

	/* check that the string was xmalloc()ed and actually has contents */
	xassert(xsize(*value_ptr));

#ifndef NDEBUG
	/*
	 * catch use after free by the caller by using the existing xfree()
	 * functionality
	 */
	char *nv = xstrdup(*value_ptr);
	xfree(*value_ptr);
	*value_ptr = nv;
#endif

	_release(data);

	data->type = DATA_TYPE_STRING;
	/* take ownership of string */
	data->data.string_u = *value_ptr;

	log_flag_hex(DATA, *value_ptr, strlen(*value_ptr),
		     "%s: set "PRINTF_DATA_T,
		     __func__, PRINTF_DATA_T_VAL(data), *value_ptr);

	*value_ptr = NULL;
	return data;
}

extern data_t *data_set_dict(data_t *data)
{
	_check_magic(data);

	if (!data)
		return NULL;
	_release(data);

	data->type = DATA_TYPE_DICT;
	data->data.dict_u = _data_list_new();

	log_flag(DATA, "%s: set "PRINTF_DATA_T" to dictionary",
		 __func__, PRINTF_DATA_T_VAL(data));

	return data;
}

extern data_t *data_set_list(data_t *data)
{
	_check_magic(data);

	if (!data)
		return NULL;
	_release(data);

	data->type = DATA_TYPE_LIST;
	data->data.list_u = _data_list_new();

	log_flag(DATA, "%s: set "PRINTF_DATA_T" to list",
		 __func__, PRINTF_DATA_T_VAL(data));

	return data;
}

extern data_t *data_list_append(data_t *data)
{
	data_t *ndata = NULL;
	_check_magic(data);

	xassert(data && (data->type == DATA_TYPE_LIST));
	if (!data || data->type != DATA_TYPE_LIST)
		return NULL;

	ndata = data_new();
	_data_list_append(data->data.list_u, ndata, NULL);

	if (slurm_conf.debug_flags & DEBUG_FLAG_DATA) {
		char *index = xstrdup_printf("#%zu", data->data.list_u->count);

		log_flag(DATA, "%s: appended "PRINTF_DATA_T_INDEX"="PRINTF_DATA_T,
			 __func__, PRINTF_DATA_T_INDEX_VAL(data, index),
			 PRINTF_DATA_T_VAL(ndata));

		xfree(index);
	}

	return ndata;
}

extern data_t *data_list_prepend(data_t *data)
{
	data_t *ndata = NULL;
	_check_magic(data);

	if (!data || data->type != DATA_TYPE_LIST)
		return NULL;

	ndata = data_new();
	_data_list_prepend(data->data.list_u, ndata, NULL);

	log_flag(DATA, "%s: list prepend data (0x%"PRIXPTR") to (0x%"PRIXPTR")",
		 __func__, (uintptr_t) ndata, (uintptr_t) data);

	if (slurm_conf.debug_flags & DEBUG_FLAG_DATA) {
		char *index = xstrdup_printf("#%zu", data->data.list_u->count);

		log_flag(DATA, "%s: appended "PRINTF_DATA_T_INDEX"="PRINTF_DATA_T,
			 __func__, PRINTF_DATA_T_INDEX_VAL(data, index),
			 PRINTF_DATA_T_VAL(ndata));

		xfree(index);
	}

	return ndata;
}

extern data_t *data_list_dequeue(data_t *data)
{
	data_list_node_t *n;
	data_t *ret = NULL;
	_check_magic(data);

	if (!data || data->type != DATA_TYPE_LIST)
		return NULL;

	if (!(n = data->data.list_u->begin))
		return NULL;

	_check_data_list_node_magic(n);

	/* extract out data for caller */
	SWAP(ret, n->data);

	/* remove node from list */
	_release_data_list_node(data->data.list_u, n);

	if (slurm_conf.debug_flags & DEBUG_FLAG_DATA) {
		char *index = xstrdup_printf("#%zu", data->data.list_u->count);

		log_flag(DATA, "%s: dequeued "PRINTF_DATA_T_INDEX"="PRINTF_DATA_T,
			 __func__, PRINTF_DATA_T_INDEX_VAL(data, index),
			 PRINTF_DATA_T_VAL(ret));

		xfree(index);
	}

	return ret;
}

static data_for_each_cmd_t _data_list_join(const data_t *src, void *arg)
{
	data_t *dst = (data_t *) arg;
	data_t *dst_entry;
	_check_magic(src);
	_check_magic(dst);
	xassert(data_get_type(dst) == DATA_TYPE_LIST);

	log_flag(DATA, "%s: list join data (0x%"PRIXPTR") to (0x%"PRIXPTR")",
		 __func__, (uintptr_t) src, (uintptr_t) dst);

	dst_entry = data_list_append(dst);
	data_copy(dst_entry, src);

	if (slurm_conf.debug_flags & DEBUG_FLAG_DATA) {
		char *index = xstrdup_printf("#%zu", dst->data.list_u->count);

		log_flag(DATA, "%s: list join "PRINTF_DATA_T" to "PRINTF_DATA_T_INDEX"="PRINTF_DATA_T,
			 __func__, PRINTF_DATA_T_VAL(src),
			 PRINTF_DATA_T_INDEX_VAL(dst, index),
			 PRINTF_DATA_T_VAL(dst_entry));

		xfree(index);
	}

	 return DATA_FOR_EACH_CONT;
}

extern data_t *data_list_join(const data_t **data, bool flatten_lists)
{
	data_t *dst = data_set_list(data_new());

	for (size_t i = 0; data[i]; i++) {
		if (slurm_conf.debug_flags & DEBUG_FLAG_DATA) {
			char *index = xstrdup_printf("#%zu", dst->data.list_u->count);

			log_flag(DATA, "%s: %s list join "PRINTF_DATA_T" to "PRINTF_DATA_T_INDEX,
				 __func__, (flatten_lists ? "flattened" : ""),
				 PRINTF_DATA_T_VAL(data[i]),
				 PRINTF_DATA_T_INDEX_VAL(dst, index));

			xfree(index);
		}

		if (flatten_lists && (data_get_type(data[i]) == DATA_TYPE_LIST))
			(void) data_list_for_each_const(data[i],
							_data_list_join, dst);
		else /* simple join */
			_data_list_join(data[i], dst);
	}

	return dst;
}

const data_t *data_key_get_const(const data_t *data, const char *key)
{
	const data_list_node_t *i;

	_check_magic(data);
	if (!data)
		return NULL;

	xassert(data->type == DATA_TYPE_DICT);
	if (!key || data->type != DATA_TYPE_DICT)
		return NULL;

	/* don't bother searching empty dictionary */
	if (!data->data.dict_u->count)
		return NULL;

	_check_data_list_magic(data->data.dict_u);
	i = data->data.dict_u->begin;
	while (i) {
		_check_data_list_node_magic(i);

		if (!xstrcmp(key, i->key))
			break;

		i = i->next;
	}

	if (i)
		return i->data;
	else
		return NULL;
}

static bool _match_string(const char *key, data_t *data, void *needle_ptr)
{
	const char *needle = needle_ptr;
	return !xstrcmp(key, needle);
}

extern data_t *data_key_get(data_t *data, const char *key)
{
	return data_dict_find_first(data, _match_string, (void *) key);
}

extern data_t *data_key_get_int(data_t *data, int64_t key)
{
	char *key_str = xstrdup_printf("%"PRId64, key);
	data_t *node = data_key_get(data, key_str);

	xfree(key_str);

	return node;
}

extern data_t *data_list_find_first(
	data_t *data,
	bool (*match)(const data_t *data, void *needle),
	void *needle)
{
	data_list_node_t *i;

	_check_magic(data);
	if (!data)
		return NULL;

	xassert(data->type == DATA_TYPE_LIST);
	if (data->type != DATA_TYPE_LIST)
		return NULL;

	/* don't bother searching empty list */
	if (!data->data.list_u->count)
		return NULL;

	_check_data_list_magic(data->data.list_u);
	i = data->data.list_u->begin;
	while (i) {
		_check_data_list_node_magic(i);

		if (match(i->data, needle))
			break;

		i = i->next;
	}

	if (i)
		return i->data;
	else
		return NULL;
}

extern data_t *data_dict_find_first(
	data_t *data,
	bool (*match)(const char *key, data_t *data, void *needle),
	void *needle)
{
	data_list_node_t *i;

	_check_magic(data);
	if (!data)
		return NULL;

	xassert(data->type == DATA_TYPE_DICT);
	if (data->type != DATA_TYPE_DICT)
		return NULL;

	/* don't bother searching empty dictionary */
	if (!data->data.dict_u->count)
		return NULL;

	_check_data_list_magic(data->data.dict_u);
	i = data->data.dict_u->begin;
	while (i) {
		_check_data_list_node_magic(i);

		if (match(i->key, i->data, needle))
			break;

		i = i->next;
	}

	if (i)
		return i->data;
	else
		return NULL;
}

extern data_t *data_key_set(data_t *data, const char *key)
{
	data_t *d;

	_check_magic(data);

	if (!data)
		return NULL;

	xassert(data->type == DATA_TYPE_DICT);
	xassert(key && key[0]);
	if (!key || !key[0] || data->type != DATA_TYPE_DICT)
		return NULL;

	if ((d = data_key_get(data, key))) {
		log_flag(DATA, "%s: overwrite existing key in "PRINTF_DATA_T_INDEX"="PRINTF_DATA_T,
			 __func__, PRINTF_DATA_T_INDEX_VAL(data, key),
			 PRINTF_DATA_T_VAL(d));
		return d;
	}

	d = data_new();
	_data_list_append(data->data.dict_u, d, key);

	log_flag(DATA, "%s: populate new key in "PRINTF_DATA_T_INDEX"="PRINTF_DATA_T,
		 __func__, PRINTF_DATA_T_INDEX_VAL(data, key),
		 PRINTF_DATA_T_VAL(d));

	return d;
}

extern data_t *data_key_set_int(data_t *data, int64_t key)
{
	char *key_str = xstrdup_printf("%"PRId64, key);
	data_t *node = data_key_set(data, key_str);

	xfree(key_str);

	return node;
}

extern bool data_key_unset(data_t *data, const char *key)
{
	data_list_node_t *i;

	_check_magic(data);
	if (!data)
		return false;

	xassert(data->type == DATA_TYPE_DICT);
	if (!key || data->type != DATA_TYPE_DICT)
		return NULL;

	_check_data_list_magic(data->data.dict_u);
	i = data->data.dict_u->begin;
	while (i) {
		_check_data_list_node_magic(i);

		if (!xstrcmp(key, i->key))
			break;

		i = i->next;
	}

	if (!i) {
		log_flag(DATA, "%s: remove non-existent key in "PRINTF_DATA_T_INDEX,
			 __func__, PRINTF_DATA_T_INDEX_VAL(data, key));
		return false;
	}

	log_flag(DATA, "%s: remove existing key in "PRINTF_DATA_T_INDEX"="PRINTF_DATA_LIST_NODE_T ,
		 __func__, PRINTF_DATA_T_INDEX_VAL(data, key),
		 PRINTF_DATA_LIST_NODE_T_VAL(i));

	_release_data_list_node(data->data.dict_u, i);

	return true;
}

extern double data_get_float(const data_t *data)
{
	_check_magic(data);

	if (!data)
		return NAN;

	xassert(data->type == DATA_TYPE_FLOAT);
	return data->data.float_u;
}

extern bool data_get_bool(const data_t *data)
{
	_check_magic(data);

	if (!data)
		return false;

	xassert(data->type == DATA_TYPE_BOOL);
	return data->data.bool_u;
}

extern int64_t data_get_int(const data_t *data)
{
	_check_magic(data);

	if (!data)
		return 0;

	xassert(data->type == DATA_TYPE_INT_64);
	return data->data.int_u;
}

extern char *data_get_string(data_t *data)
{
	_check_magic(data);

	if (!data)
		return NULL;

	xassert(data->type == DATA_TYPE_STRING);
	return data->data.string_u;
}

extern const char *data_get_string_const(const data_t *data)
{
	_check_magic(data);

	if (!data)
		return NULL;

	xassert(data->type == DATA_TYPE_STRING);
	return data->data.string_u;
}

extern int data_get_string_converted(const data_t *d, char **buffer)
{
	_check_magic(d);
	char *_buffer = NULL;
	bool cloned;

	if (!d || !buffer)
		return ESLURM_DATA_PTR_NULL;

	if (data_get_type(d) != DATA_TYPE_STRING) {
		/* copy the data and then convert it to a string type */
		data_t *dclone = data_new();
		data_copy(dclone, d);
		if (data_convert_type(dclone, DATA_TYPE_STRING) ==
		    DATA_TYPE_STRING)
			SWAP(_buffer, dclone->data.string_u);
		FREE_NULL_DATA(dclone);
		cloned = true;
	} else {
		_buffer = xstrdup(data_get_string_const(d));
		if (!_buffer)
			_buffer = xstrdup("");
		cloned = false;
	}

	if (_buffer) {
		*buffer = _buffer;

		log_flag_hex(DATA, _buffer, strlen(_buffer),
			     "%s:["PRINTF_DATA_T"] string %sat string[%zu]=0x%"PRIxPTR,
			     __func__, PRINTF_DATA_T_VAL(d),
			     (cloned ? "conversion and cloned " : ""),
			     strlen(_buffer), (uintptr_t) _buffer);

		return SLURM_SUCCESS;
	}

	log_flag(DATA, "%s: "PRINTF_DATA_T" string conversion failed",
		 __func__, PRINTF_DATA_T_VAL(d));

	return ESLURM_DATA_CONV_FAILED;
}

extern int data_copy_bool_converted(const data_t *d, bool *buffer)
{
	_check_magic(d);
	int rc = ESLURM_DATA_CONV_FAILED;

	if (!d || !buffer)
		return ESLURM_DATA_PTR_NULL;

	if (data_get_type(d) != DATA_TYPE_BOOL) {
		data_t *dclone = data_new();
		data_copy(dclone, d);
		if (data_convert_type(dclone, DATA_TYPE_BOOL) ==
		    DATA_TYPE_BOOL) {
			*buffer = data_get_bool(dclone);
			rc = SLURM_SUCCESS;
		}
		FREE_NULL_DATA(dclone);

		log_flag(DATA, "%s:["PRINTF_DATA_T"] converted value=%s",
			 __func__, PRINTF_DATA_T_VAL(d),
			 (*buffer ? "true" : "false"));
		return rc;
	}

	*buffer = data_get_bool(d);
	return SLURM_SUCCESS;
}

extern int data_get_bool_converted(data_t *d, bool *buffer)
{
	int rc;
	_check_magic(d);

	if (!d || !buffer)
		return ESLURM_DATA_PTR_NULL;

	/* assign value if converted successfully */
	rc = data_copy_bool_converted(d, buffer);
	if (!rc)
		data_set_bool(d, *buffer);

	return rc;
}

extern int data_get_int_converted(const data_t *d, int64_t *buffer)
{
	_check_magic(d);
	int rc = SLURM_SUCCESS;

	if (!d || !buffer)
		return ESLURM_DATA_PTR_NULL;

	if (data_get_type(d) != DATA_TYPE_INT_64) {
		data_t *dclone = data_new();
		data_copy(dclone, d);
		if (data_convert_type(dclone, DATA_TYPE_INT_64) ==
		    DATA_TYPE_INT_64)
			*buffer = data_get_int(dclone);
		else
			rc = ESLURM_DATA_CONV_FAILED;
		FREE_NULL_DATA(dclone);
	} else {
		*buffer = data_get_int(d);
	}

	log_flag(DATA, "%s: converted "PRINTF_DATA_T"=%"PRId64,
		 __func__, PRINTF_DATA_T_VAL(d), *buffer);

	return rc;
}

extern size_t data_get_dict_length(const data_t *data)
{
	_check_magic(data);

	if (!data)
		return 0;

	xassert(data->type == DATA_TYPE_DICT);
	return data->data.dict_u->count;
}

extern size_t data_get_list_length(const data_t *data)
{
	_check_magic(data);

	if (!data)
		return 0;

	xassert(data->type == DATA_TYPE_LIST);
	return data->data.list_u->count;
}

extern data_t *data_get_list_last(data_t *data)
{
	data_list_node_t *i;
	_check_magic(data);

	if (!data)
		return NULL;

	xassert(data->type == DATA_TYPE_LIST);
	if (data->type != DATA_TYPE_LIST)
		return NULL;

	if (!data->data.list_u->count)
		return NULL;

	i = data->data.list_u->begin;
	_check_data_list_magic(data->data.list_u);
	while (i) {
		_check_data_list_node_magic(i);
		xassert(!i->key);

		if (!i->next) {
			log_flag(DATA, "%s: "PRINTF_DATA_T_INDEX"="PRINTF_DATA_T,
				 __func__,
				 PRINTF_DATA_T_INDEX_VAL(data, i->key),
				 PRINTF_DATA_T_VAL(i->data));
			return i->data;
		}

		i = i->next;
	}

	fatal_abort("%s: malformed data list", __func__);
}

extern int data_list_split_str(data_t *dst, const char *src, const char *token)
{
	char *save_ptr = NULL;
	char *tok = NULL;
	char *str = xstrdup(src);

	if (data_get_type(dst) == DATA_TYPE_NULL)
		data_set_list(dst);

	xassert(data_get_type(dst) == DATA_TYPE_LIST);
	if (data_get_type(dst) != DATA_TYPE_LIST)
		return SLURM_ERROR;

	tok = strtok_r(str, "/", &save_ptr);
	while (tok) {
		data_t *e = data_list_append(dst);
		xstrtrim(tok);

		data_set_string(e, tok);

		if (slurm_conf.debug_flags & DEBUG_FLAG_DATA) {
			char *index = xstrdup_printf("#%zu",
						     dst->data.list_u->count);

			log_flag_hex(DATA, tok, strlen(tok),
				     "%s: split string from 0x%"PRIxPTR" to "PRINTF_DATA_T_INDEX"="PRINTF_DATA_T,
				     __func__, (uintptr_t) src,
				     PRINTF_DATA_T_INDEX_VAL(dst, index),
				     PRINTF_DATA_T_VAL(e));

			xfree(index);
		}

		tok = strtok_r(NULL, "/", &save_ptr);
	}
	xfree(str);

	return SLURM_SUCCESS;
}

static data_for_each_cmd_t _foreach_join_str(const data_t *data, void *arg)
{
	char *b = NULL;
	merge_path_strings_t *args = arg;

	data_get_string_converted(data, &b);

	xstrfmtcatat(args->path, &args->at, "%s%s%s",
		     (!args->path ? args->token : ""),
		     (args->at ? args->token : ""), b);

	xfree(b);

	return DATA_FOR_EACH_CONT;
}

extern int data_list_join_str(char **dst, const data_t *src, const char *token)
{
	merge_path_strings_t args = {
		.token = token,
	};

	xassert(!*dst);
	xassert(data_get_type(src) == DATA_TYPE_LIST);

	if (data_list_for_each_const(src, _foreach_join_str, &args) < 0) {
		xfree(args.path);
		return SLURM_ERROR;
	}

	*dst = args.path;

	log_flag_hex(DATA, *dst, strlen(*dst),
		     "%s: "PRINTF_DATA_T" string joined with token %s",
		     __func__, PRINTF_DATA_T_VAL(src), token);

	return SLURM_SUCCESS;
}

extern int data_list_for_each_const(const data_t *d, DataListForFConst f, void *arg)
{
	int count = 0;
	const data_list_node_t *i;

	_check_magic(d);

	if (!d || data_get_type(d) != DATA_TYPE_LIST) {
		error("%s: for each attempted on non-list object (0x%"PRIXPTR")",
		      __func__, (uintptr_t) d);
		return -1;
	}

	i = d->data.list_u->begin;
	_check_data_list_magic(d->data.list_u);
	while (i) {
		_check_data_list_node_magic(i);

		xassert(!i->key);
		data_for_each_cmd_t cmd = f(i->data, arg);
		count++;

		xassert(cmd > DATA_FOR_EACH_INVALID);
		xassert(cmd < DATA_FOR_EACH_MAX);

		switch (cmd) {
		case DATA_FOR_EACH_CONT:
			break;
		case DATA_FOR_EACH_DELETE:
			fatal_abort("%s: delete attempted against const",
				    __func__);
			break;
		case DATA_FOR_EACH_FAIL:
			count *= -1;
			/* fall through */
		case DATA_FOR_EACH_STOP:
			i = NULL;
			break;
		default:
			fatal_abort("%s: invalid cmd", __func__);
		}

		if (i)
			i = i->next;
	}

	return count;
}

extern int data_list_for_each(data_t *d, DataListForF f, void *arg)
{
	int count = 0;
	data_list_node_t *i;

	_check_magic(d);

	if (!d || data_get_type(d) != DATA_TYPE_LIST) {
		error("%s: for each attempted on non-list "PRINTF_DATA_T,
		      __func__, PRINTF_DATA_T_VAL(d));
		return -1;
	}

	i = d->data.list_u->begin;
	_check_data_list_magic(d->data.list_u);
	while (i) {
		_check_data_list_node_magic(i);

		xassert(!i->key);
		data_for_each_cmd_t cmd = f(i->data, arg);
		count++;

		xassert(cmd > DATA_FOR_EACH_INVALID);
		xassert(cmd < DATA_FOR_EACH_MAX);

		switch (cmd) {
		case DATA_FOR_EACH_CONT:
			break;
		case DATA_FOR_EACH_DELETE:
			_release_data_list_node(d->data.list_u, i);
			break;
		case DATA_FOR_EACH_FAIL:
			count *= -1;
			/* fall through */
		case DATA_FOR_EACH_STOP:
			i = NULL;
			break;
		default:
			fatal_abort("%s: invalid cmd", __func__);
		}

		if (i)
			i = i->next;
	}

	return count;
}

extern int data_dict_for_each_const(const data_t *d, DataDictForFConst f, void *arg)
{
	int count = 0;
	const data_list_node_t *i;

	_check_magic(d);

	if (!d || data_get_type(d) != DATA_TYPE_DICT) {
		error("%s: for each attempted on non-dict "PRINTF_DATA_T,
		      __func__, PRINTF_DATA_T_VAL(d));
		return -1;
	}

	i = d->data.dict_u->begin;
	_check_data_list_magic(d->data.dict_u);
	while (i) {
		data_for_each_cmd_t cmd;

		_check_data_list_node_magic(i);

		cmd = f(i->key, i->data, arg);
		count++;

		xassert(cmd > DATA_FOR_EACH_INVALID);
		xassert(cmd < DATA_FOR_EACH_MAX);

		switch (cmd) {
		case DATA_FOR_EACH_CONT:
			break;
		case DATA_FOR_EACH_DELETE:
			fatal_abort("%s: delete attempted against const",
				    __func__);
			break;
		case DATA_FOR_EACH_FAIL:
			count *= -1;
			/* fall through */
		case DATA_FOR_EACH_STOP:
			i = NULL;
			break;
		default:
			fatal_abort("%s: invalid cmd", __func__);
		}

		if (i)
			i = i->next;
	}

	return count;
}

extern int data_dict_for_each(data_t *d, DataDictForF f, void *arg)
{
	int count = 0;
	data_list_node_t *i;

	_check_magic(d);

	if (!d || data_get_type(d) != DATA_TYPE_DICT) {
		error("%s: for each attempted on non-dict "PRINTF_DATA_T,
		      __func__, PRINTF_DATA_T_VAL(d));
		return -1;
	}

	i = d->data.dict_u->begin;
	_check_data_list_magic(d->data.dict_u);
	while (i) {
		_check_data_list_node_magic(i);

		data_for_each_cmd_t cmd = f(i->key, i->data, arg);
		count++;

		xassert(cmd > DATA_FOR_EACH_INVALID);
		xassert(cmd < DATA_FOR_EACH_MAX);

		switch (cmd) {
		case DATA_FOR_EACH_CONT:
			break;
		case DATA_FOR_EACH_DELETE:
			_release_data_list_node(d->data.dict_u, i);
			break;
		case DATA_FOR_EACH_FAIL:
			count *= -1;
			/* fall through */
		case DATA_FOR_EACH_STOP:
			i = NULL;
			break;
		default:
			fatal_abort("%s: invalid cmd", __func__);
		}

		if (i)
			i = i->next;
	}

	return count;
}

static int _convert_data_string(data_t *data)
{
	_check_magic(data);

	switch (data->type) {
	case DATA_TYPE_STRING:
		return SLURM_SUCCESS;
	case DATA_TYPE_BOOL:
		data_set_string(data, (data->data.bool_u ? "true" : "false"));
		return SLURM_SUCCESS;
	case DATA_TYPE_NULL:
		data_set_string(data, "");
		return SLURM_SUCCESS;
	case DATA_TYPE_FLOAT:
	{
		char *str = xstrdup_printf("%lf", data->data.float_u);
		data_set_string(data, str);
		xfree(str);
		return SLURM_SUCCESS;
	}
	case DATA_TYPE_INT_64:
	{
		char *str = xstrdup_printf("%"PRId64, data->data.int_u);
		data_set_string(data, str);
		xfree(str);
		return SLURM_SUCCESS;
	}
	default:
		return ESLURM_DATA_CONV_FAILED;
	}

	return ESLURM_DATA_CONV_FAILED;
}

static int _convert_data_force_bool(data_t *data)
{
	_check_magic(data);

	/* attempt to detect the type first */
	data_convert_type(data, DATA_TYPE_NONE);

	switch (data->type) {
	case DATA_TYPE_STRING:
		/* non-empty string but not recognized format */
		data_set_bool(data, true);
		return SLURM_SUCCESS;
	case DATA_TYPE_BOOL:
		return SLURM_SUCCESS;
	case DATA_TYPE_NULL:
		data_set_bool(data, false);
		return SLURM_SUCCESS;
	case DATA_TYPE_FLOAT:
		data_set_bool(data, data->data.float_u != 0);
		return SLURM_SUCCESS;
	case DATA_TYPE_INT_64:
		data_set_bool(data, data->data.int_u != 0);
		return SLURM_SUCCESS;
	default:
		return ESLURM_DATA_CONV_FAILED;
	}

	return ESLURM_DATA_CONV_FAILED;
}

static int _convert_data_null(data_t *data)
{
	_check_magic(data);

	switch (data->type) {
	case DATA_TYPE_STRING:
	{
		const char *str = data->data.string_u;

		xassert(data->data.string_u);

		if (!str[0])
			goto convert;

		if (str[0] == '~')
			goto convert;

		if (!xstrcasecmp(str, "null"))
			goto convert;

		goto fail;
	}
	case DATA_TYPE_NULL:
		return SLURM_SUCCESS;
	default:
		return ESLURM_DATA_CONV_FAILED;
	}
fail:
	return ESLURM_DATA_CONV_FAILED;
convert:
	log_flag_hex(DATA, data->data.string_u, strlen(data->data.string_u),
		     "%s: converted "PRINTF_DATA_T"->null",
		     __func__, PRINTF_DATA_T_VAL(data));
	data_set_null(data);
	return SLURM_SUCCESS;
}

static int _convert_data_bool(data_t *data)
{
	const char *str = NULL;

	_check_magic(data);

	switch (data->type) {
	case DATA_TYPE_STRING:
	{
		str = data->data.string_u;

		if (tolower(str[0]) == 'y') {
			if (!str[1] || ((tolower(str[1]) == 'e') &&
					(tolower(str[2]) == 's') &&
					(str[3] == '\0'))) {
				data_set_bool(data, true);
				goto converted;
			}
			goto fail;
		} else if (tolower(str[0]) == 't') {
			if (!str[1] || ((tolower(str[1]) == 'r') &&
					(tolower(str[2]) == 'u') &&
					(tolower(str[3]) == 'e') &&
					(str[4] == '\0'))) {
				data_set_bool(data, true);
				goto converted;
			}
			goto fail;
		} else if ((tolower(str[0]) == 'o') &&
			   (tolower(str[1]) == 'n') &&
			   (str[2] == '\0')) {
			data_set_bool(data, true);
			goto converted;
		} else if (tolower(str[0]) == 'n') {
			if (!str[1] || ((tolower(str[1]) == 'o') &&
					(str[2] == '\0'))) {
				data_set_bool(data, false);
				goto converted;
			}
			goto fail;
		} else if (tolower(str[0]) == 'f') {
			if (!str[1] || ((tolower(str[1]) == 'a') &&
					(tolower(str[2]) == 'l') &&
					(tolower(str[3]) == 's') &&
					(tolower(str[4]) == 'e') &&
					(str[5] == '\0'))) {
				data_set_bool(data, false);
				goto converted;
			}
			goto fail;
		} else if ((tolower(str[0]) == 'o') &&
			   (tolower(str[1]) == 'f') &&
			   (tolower(str[2]) == 'f') &&
			   (str[3] == '\0')) {
			data_set_bool(data, false);
			goto converted;
		}

		goto fail;
	}
	case DATA_TYPE_BOOL:
		return SLURM_SUCCESS;
	default:
		goto fail;
	}

	goto fail;

converted:
	log_flag_hex(DATA, str, strlen(str),
		     "%s: converted "PRINTF_DATA_T"->%s",
		 __func__, PRINTF_DATA_T_VAL(data),
		 (data_get_bool(data) ? "true" : "false"));
	return SLURM_SUCCESS;

fail:
	if (str)
		log_flag_hex(DATA, str, strlen(str),
			     "%s: converting "PRINTF_DATA_T" to bool failed",
			 __func__, PRINTF_DATA_T_VAL(data));
	else
		log_flag(DATA, "%s: converting "PRINTF_DATA_T" to bool failed",
			 __func__, PRINTF_DATA_T_VAL(data));
	return ESLURM_DATA_CONV_FAILED;
}

static int _convert_data_int(data_t *data, bool force)
{
	_check_magic(data);

	switch (data->type) {
	case DATA_TYPE_STRING:
	{
		int64_t x;
		const char *str = data->data.string_u;

		if (!str[0])
			goto string_fail;

		if ((str[0] == '0') && (tolower(str[1]) == 'x')) {
			if ((sscanf(str, "%"SCNx64, &x) == 1)) {
				log_flag_hex(DATA, str, strlen(str),
					     "%s: converted hex number "PRINTF_DATA_T"->%"PRId64,
					 __func__, PRINTF_DATA_T_VAL(data), x);
				data_set_int(data, x);
				return SLURM_SUCCESS;
			}
		} else if (sscanf(str, "%"SCNd64, &x) == 1) {
			log_flag_hex(DATA, str, strlen(str),
				     "%s: converted "PRINTF_DATA_T"->%"PRId64,
				 __func__, PRINTF_DATA_T_VAL(data), x);
			data_set_int(data, x);
			return SLURM_SUCCESS;
		}

		goto string_fail;
	}
	case DATA_TYPE_FLOAT:
		if (force) {
			data_set_int(data, lrint(data_get_float(data)));
			return SLURM_SUCCESS;
		}
		return ESLURM_DATA_CONV_FAILED;
	case DATA_TYPE_INT_64:
		return SLURM_SUCCESS;
	default:
		return ESLURM_DATA_CONV_FAILED;
	}

string_fail:
	log_flag_hex(DATA, data->data.string_u, strlen(data->data.string_u),
		     "%s: convert "PRINTF_DATA_T" to int failed: %s",
		     __func__, PRINTF_DATA_T_VAL(data));
	return ESLURM_DATA_CONV_FAILED;
}

static int _convert_data_float_from_string(data_t *data)
{
	const char *str = data->data.string_u;
	int i = 0;
	bool negative = false;

	xassert(str);

	if (str[i] == '+') {
		i++;
	} else if (str[i] == '-') {
		i++;
		negative = true;
	}

	if ((tolower(str[i]) == 'i')) {
		i++;

		if (!xstrcasecmp(&str[i], "nf") ||
		    !xstrcasecmp(&str[i], "nfinity")) {
			if (negative)
				data_set_float(data, -INFINITY);
			else
				data_set_float(data, INFINITY);

			goto converted;
		}

		goto fail;
	}

	if ((tolower(str[i]) == 'n')) {
		i++;

		if (!xstrcasecmp(&str[i], "an")) {
			if (negative)
				data_set_float(data, -NAN);
			else
				data_set_float(data, NAN);

			goto converted;
		}

		goto fail;
	}

	if ((str[i] >= '0') && (str[i] <= '9')) {
		double x;

		if (sscanf(&str[i], "%lf", &x) == 1) {
			data_set_float(data, x);
			goto converted;
		}
	}

	goto fail;

converted:
	log_flag(DATA, "%s: converted "PRINTF_DATA_T" to float: %s->%lf",
		 __func__, PRINTF_DATA_T_VAL(data), str, data_get_float(data));
	return SLURM_SUCCESS;

fail:
	log_flag_hex(DATA, str, strlen(str),
		     "%s: convert "PRINTF_DATA_T" to double float failed",
		     __func__, PRINTF_DATA_T_VAL(data));
	return ESLURM_DATA_CONV_FAILED;
}

static int _convert_data_float(data_t *data)
{
	_check_magic(data);

	switch (data->type) {
	case DATA_TYPE_STRING:
		return _convert_data_float_from_string(data);
	case DATA_TYPE_INT_64:
		if (data_get_int(data) == INFINITE64)
			data_set_float(data, HUGE_VAL);
		else if (data_get_int(data) == NO_VAL64)
			data_set_float(data, NAN);
		else /* attempt normal fp conversion */
			data_set_float(data, data_get_int(data));
		return SLURM_SUCCESS;
	case DATA_TYPE_FLOAT:
		return SLURM_SUCCESS;
	default:
		return ESLURM_DATA_CONV_FAILED;
	}

	return ESLURM_DATA_CONV_FAILED;
}

extern data_type_t data_convert_type(data_t *data, data_type_t match)
{
	_check_magic(data);

	if (!data)
		return DATA_TYPE_NONE;

	switch (match) {
	case DATA_TYPE_STRING:
		return _convert_data_string(data) ? DATA_TYPE_NONE :
						    DATA_TYPE_STRING;
	case DATA_TYPE_BOOL:
		return _convert_data_force_bool(data) ? DATA_TYPE_NONE :
							DATA_TYPE_BOOL;
	case DATA_TYPE_INT_64:
		return _convert_data_int(data, true) ? DATA_TYPE_NONE :
						       DATA_TYPE_INT_64;
	case DATA_TYPE_FLOAT:
		return _convert_data_float(data) ? DATA_TYPE_NONE :
						   DATA_TYPE_FLOAT;
	case DATA_TYPE_NULL:
		return _convert_data_null(data) ? DATA_TYPE_NONE :
						  DATA_TYPE_NULL;
	case DATA_TYPE_NONE:
		if (!_convert_data_null(data))
			return DATA_TYPE_NULL;

		if (!_convert_data_int(data, false))
			return DATA_TYPE_INT_64;

		if (!_convert_data_float(data))
			return DATA_TYPE_FLOAT;

		if (!_convert_data_bool(data))
			return DATA_TYPE_BOOL;

		return DATA_TYPE_NONE;
	case DATA_TYPE_DICT:
	case DATA_TYPE_LIST:
		/* data_parser should be used for this conversion instead. */
		return DATA_TYPE_NONE;
	case DATA_TYPE_MAX:
		break;
	}

	xassert(false);
	return DATA_TYPE_NONE;
}

static data_for_each_cmd_t _convert_list_entry(data_t *data, void *arg)
{
	convert_args_t *args = arg;

	args->count += data_convert_tree(data, args->match);

	return DATA_FOR_EACH_CONT;
}

static data_for_each_cmd_t _convert_dict_entry(const char *key, data_t *data,
					       void *arg)
{
	convert_args_t *args = arg;

	args->count += data_convert_tree(data, args->match);

	return DATA_FOR_EACH_CONT;
}

extern size_t data_convert_tree(data_t *data, const data_type_t match)
{
	convert_args_t args = { .match = match };
	_check_magic(data);

	if (!data)
		return 0;

	switch (data_get_type(data)) {
	case DATA_TYPE_DICT:
		(void)data_dict_for_each(data, _convert_dict_entry, &args);
		break;
	case DATA_TYPE_LIST:
		(void)data_list_for_each(data, _convert_list_entry, &args);
		break;
	default:
		if (match == data_convert_type(data, match))
			args.count++;
		break;
	}

	return args.count;
}

static data_for_each_cmd_t _find_dict_match(const char *key, const data_t *a,
					    void *arg)
{
	bool rc;
	find_dict_match_t *p = arg;
	const data_t *b = data_key_get_const(p->b, key);

	rc = data_check_match(a, b, p->mask);

	log_flag(DATA, "dictionary compare: %s(0x%"PRIXPTR")=%s(0x%"PRIXPTR") %s %s(0x%"PRIXPTR")=%s(0x%"PRIXPTR")",
		 key, (uintptr_t) p->a, data_type_to_string(data_get_type(a)),
		 (uintptr_t) a, (rc ? "\u2261" : "\u2260"), key,
		 (uintptr_t) p->b, data_type_to_string(data_get_type(b)),
		 (uintptr_t) b);

	return rc ? DATA_FOR_EACH_CONT : DATA_FOR_EACH_FAIL;
}

static bool _data_match_dict(const data_t *a, const data_t *b, bool mask)
{
	find_dict_match_t p = {
		.mask = mask,
		.a = a,
		.b = b,
	};

	if (!a || data_get_type(a) != DATA_TYPE_DICT)
		return false;

	if (!b || data_get_type(b) != DATA_TYPE_DICT)
		return false;

	_check_magic(a);
	_check_magic(b);

	if (a->data.dict_u->count != b->data.dict_u->count)
		return false;

	/* match by key and not order with dictionary */
	return (data_dict_for_each_const(a, _find_dict_match, &p) >= 0);
}

static bool _data_match_lists(const data_t *a, const data_t *b, bool mask)
{
	bool fail = false;
	const data_list_node_t *ptr_a;
	const data_list_node_t *ptr_b;

	if (!a || data_get_type(a) != DATA_TYPE_LIST)
		return false;
	if (!b || data_get_type(b) != DATA_TYPE_LIST)
		return false;

	_check_magic(a);
	_check_magic(b);

	if (a->data.list_u->count != b->data.list_u->count)
		return false;

	ptr_a = a->data.list_u->begin;
	ptr_b = b->data.list_u->begin;

	while (ptr_a && !fail) {
		_check_data_list_node_magic(ptr_a);

		if (!ptr_b && mask)
			/* ignore a if b is NULL when masking */
			continue;

		_check_data_list_node_magic(ptr_b);
		if (data_check_match(ptr_a->data, ptr_b->data, mask)) {
			ptr_a = ptr_a->next;
			ptr_b = ptr_b->next;
		} else
			fail = true;
	}

	return !fail;
}

extern bool data_check_match(const data_t *a, const data_t *b, bool mask)
{
	bool rc;

	if (a == NULL && b == NULL)
		return true;

	if (a == NULL || b == NULL)
		return false;

	_check_magic(a);
	_check_magic(b);

	if (data_get_type(a) != data_get_type(b)) {
		log_flag(DATA, "type mismatch: %s(0x%"PRIXPTR") != %s(0x%"PRIXPTR")",
			 data_type_to_string(data_get_type(a)), (uintptr_t) a,
			 data_type_to_string(data_get_type(b)), (uintptr_t) b);
		return false;
	}

	switch (data_get_type(a)) {
	case DATA_TYPE_NULL:
		rc = (data_get_type(b) == DATA_TYPE_NULL);
		log_flag(DATA, "compare: %s(0x%"PRIXPTR") %s %s(0x%"PRIXPTR")",
			 data_type_to_string(data_get_type(a)), (uintptr_t) a,
			 (rc ? "=" : "!="),
			 data_type_to_string(data_get_type(b)), (uintptr_t) b);
		return rc;
	case DATA_TYPE_STRING:
		rc = !xstrcmp(data_get_string_const(a),
			      data_get_string_const(b));
		log_flag(DATA, "compare: %s(0x%"PRIXPTR")=%s %s %s(0x%"PRIXPTR")=%s",
			 data_type_to_string(data_get_type(a)), (uintptr_t) a,
			 data_get_string_const(a), (rc ? "=" : "!="),
			 data_type_to_string(data_get_type(b)),
			 (uintptr_t) b, data_get_string_const(b));
		return rc;
	case DATA_TYPE_BOOL:
		rc = (data_get_bool(a) == data_get_bool(b));
		log_flag(DATA, "compare: %s(0x%"PRIXPTR")=%s %s %s(0x%"PRIXPTR")=%s",
			 data_type_to_string(data_get_type(a)), (uintptr_t) a,
			 (data_get_bool(a) ? "True" : "False"),
			 (rc ? "=" : "!="),
			 data_type_to_string(data_get_type(b)), (uintptr_t) b,
			 (data_get_bool(b) ? "True" : "False"));
		return rc;
	case DATA_TYPE_INT_64:
		rc = data_get_int(a) == data_get_int(b);
		log_flag(DATA, "compare: %s(0x%"PRIXPTR")=%"PRId64" %s %s(0x%"PRIXPTR")=%"PRId64,
			 data_type_to_string(data_get_type(a)), (uintptr_t) a,
			 data_get_int(a), (rc ? "=" : "!="),
			 data_type_to_string(data_get_type(b)), (uintptr_t) b,
			 data_get_int(b));
		return rc;
	case DATA_TYPE_FLOAT:
		if (!(rc = (data_get_float(a) == data_get_float(b))) ||
		    !(rc = fuzzy_equal(data_get_float(a), data_get_float(b)))) {
			if (isnan(data_get_float(a)) ==
			    isnan(data_get_float(a)))
				rc = true;
			else if (signbit(data_get_float(a)) !=
				 signbit(data_get_float(b)))
				rc = false;
			else if (isinf(data_get_float(a)) !=
				 isinf(data_get_float(b)))
				rc = false;
			else
				rc = false;
		}

		log_flag(DATA, "compare: %s(0x%"PRIXPTR")=%e %s %s(0x%"PRIXPTR")=%e",
			 data_type_to_string(data_get_type(a)), (uintptr_t) a,
			 data_get_float(a), (rc ? "=" : "!="),
			 data_type_to_string(data_get_type(b)), (uintptr_t) b,
			 data_get_float(b));
		return rc;
	case DATA_TYPE_DICT:
		rc = _data_match_dict(a, b, mask);
		log_flag(DATA, "compare dictionary: %s(0x%"PRIXPTR")[%zd] %s %s(0x%"PRIXPTR")[%zd]",
			 data_type_to_string(data_get_type(a)), (uintptr_t) a,
			 data_get_dict_length(a), (rc ? "=" : "!="),
			 data_type_to_string(data_get_type(b)), (uintptr_t) b,
			 data_get_dict_length(b));
		return rc;
	case DATA_TYPE_LIST:
		rc = _data_match_lists(a, b, mask);
		log_flag(DATA, "compare list: %s(0x%"PRIXPTR")[%zd] %s %s(0x%"PRIXPTR")[%zd]",
			 data_type_to_string(data_get_type(a)), (uintptr_t) a,
			 data_get_list_length(a), (rc ? "=" : "!="),
			 data_type_to_string(data_get_type(b)), (uintptr_t) b,
			 data_get_list_length(b));
		return rc;
	default:
		fatal_abort("%s: unexpected data type", __func__);
	}

	return rc;
}

extern data_t *data_resolve_dict_path(data_t *data, const char *path)
{
	data_t *found = data;
	char *save_ptr = NULL;
	char *token = NULL;
	char *str;

	_check_magic(data);

	if (!data)
		return NULL;

	str = xstrdup(path);

	token = strtok_r(str, "/", &save_ptr);
	while (token && found) {
		xstrtrim(token);

		if (!found || (data_get_type(found) != DATA_TYPE_DICT)) {
			found = NULL;
			break;
		}

		if (!(found = data_key_get(found, token)))
			break;

		token = strtok_r(NULL, "/", &save_ptr);
	}
	xfree(str);

	if (found)
		log_flag_hex(DATA, path, strlen(path),
			     "%s: "PRINTF_DATA_T" resolved dictionary path to "PRINTF_DATA_T,
			     __func__, PRINTF_DATA_T_VAL(data),
			     PRINTF_DATA_T_VAL(found));
	else
		log_flag_hex(DATA, path, strlen(path),
			     "%s: "PRINTF_DATA_T" failed to resolve dictionary path",
			     __func__, PRINTF_DATA_T_VAL(data));
	return found;
}

extern const data_t *data_resolve_dict_path_const(const data_t *data,
						  const char *path)
{
	const data_t *found = data;
	char *save_ptr = NULL;
	char *token = NULL;
	char *str;

	_check_magic(data);

	if (!data)
		return NULL;

	str = xstrdup(path);

	token = strtok_r(str, "/", &save_ptr);
	while (token && found) {
		xstrtrim(token);

		if (!found || (data_get_type(found) != DATA_TYPE_DICT)) {
			found = false;
			break;
		}

		if (!(found = data_key_get_const(found, token)))
			break;

		token = strtok_r(NULL, "/", &save_ptr);
	}
	xfree(str);

	if (found)
		log_flag_hex(DATA, path, strlen(path),
			     "%s: data "PRINTF_DATA_T" resolved dictionary path to "PRINTF_DATA_T,
			     __func__, PRINTF_DATA_T_VAL(data),
			     PRINTF_DATA_T_VAL(found));
	else
		log_flag_hex(DATA, path, strlen(path),
			     "%s: data (0x%"PRIXPTR") failed to resolve dictionary path",
			     __func__, PRINTF_DATA_T_VAL(data));

	return found;
}

extern data_t *data_define_dict_path(data_t *data, const char *path)
{
	data_t *found = data;
	char *save_ptr = NULL;
	char *token = NULL;
	char *str;

	_check_magic(data);

	if (!data)
		return NULL;

	str = xstrdup(path);

	token = strtok_r(str, "/", &save_ptr);
	while (token && found) {
		xstrtrim(token);

		if (data_get_type(found) == DATA_TYPE_NULL)
			data_set_dict(found);
		else if (data_get_type(found) != DATA_TYPE_DICT) {
			found = NULL;
			break;
		}

		if (!(found = data_key_set(found, token)))
			break;

		token = strtok_r(NULL, "/", &save_ptr);
	}
	xfree(str);

	if (found)
		log_flag_hex(DATA, path, strlen(path),
			     "%s: "PRINTF_DATA_T" defined dictionary path to "PRINTF_DATA_T,
			     __func__, PRINTF_DATA_T_VAL(data),
			     PRINTF_DATA_T_VAL(found));
	else
		log_flag_hex(DATA, path, strlen(path),
			     "%s: "PRINTF_DATA_T" failed to define dictionary path",
			     __func__, PRINTF_DATA_T_VAL(data));

	return found;
}

extern data_t *data_copy(data_t *dest, const data_t *src)
{
	if (!src)
		return NULL;

	if (!dest)
		dest = data_new();

	_check_magic(src);
	_check_magic(dest);

	log_flag(DATA, "%s: copy data "PRINTF_DATA_T" to "PRINTF_DATA_T,
	       __func__, PRINTF_DATA_T_VAL(src), PRINTF_DATA_T_VAL(dest));

	switch (data_get_type(src)) {
	case DATA_TYPE_STRING:
		return data_set_string(dest, data_get_string_const(src));
	case DATA_TYPE_BOOL:
		return data_set_bool(dest, data_get_bool(src));
	case DATA_TYPE_INT_64:
		return data_set_int(dest, data_get_int(src));
	case DATA_TYPE_FLOAT:
		return data_set_float(dest, data_get_float(src));
	case DATA_TYPE_NULL:
		return data_set_null(dest);
	case DATA_TYPE_LIST:
	{
		data_list_node_t *i = src->data.list_u->begin;

		data_set_list(dest);

		while (i) {
			_check_data_list_node_magic(i);
			xassert(!i->key);
			data_copy(data_list_append(dest), i->data);
			i = i->next;
		}

		return dest;
	}
	case DATA_TYPE_DICT:
	{
		data_list_node_t *i = src->data.dict_u->begin;

		data_set_dict(dest);

		while (i) {
			_check_data_list_node_magic(i);
			data_copy(data_key_set(dest, i->key), i->data);
			i = i->next;
		}

		return dest;
	}
	default:
		fatal_abort("%s: unexpected data type", __func__);
		return NULL;
	}
}

extern data_t *data_move(data_t *dest, data_t *src)
{
	if (!src)
		return NULL;

	if (!dest)
		dest = data_new();

	_check_magic(src);
	_check_magic(dest);

	log_flag(DATA, "%s: move data "PRINTF_DATA_T" to "PRINTF_DATA_T,
		 __func__, PRINTF_DATA_T_VAL(src), PRINTF_DATA_T_VAL(dest));

	memmove(&dest->data, &src->data, sizeof(src->data));
	dest->type = src->type;
	src->type = DATA_TYPE_NULL;
	xassert((memset(&src->data, 0, sizeof(src->data))));

	return dest;
}

extern int data_retrieve_dict_path_string(const data_t *data, const char *path,
					  char **ptr_buffer)
{
	const data_t *d = NULL;
	int rc;

	_check_magic(data);
	if (!(d = data_resolve_dict_path_const(data, path)))
		return ESLURM_DATA_PATH_NOT_FOUND;

	rc = data_get_string_converted(d, ptr_buffer);

	if (rc)
		log_flag(DATA, "%s: data "PRINTF_DATA_T" failed to resolve string at path:%s",
			 __func__, PRINTF_DATA_T_VAL(data), path);
	else
		log_flag_hex(DATA, *ptr_buffer, strlen(*ptr_buffer),
			 "%s: data "PRINTF_DATA_T" resolved string at path:%s",
			 __func__, PRINTF_DATA_T_VAL(data), path);

	return rc;
}

extern int data_retrieve_dict_path_bool(const data_t *data, const char *path,
					bool *ptr_buffer)
{
	const data_t *d = NULL;
	int rc;

	_check_magic(data);
	if (!(d = data_resolve_dict_path_const(data, path)))
		return ESLURM_DATA_PATH_NOT_FOUND;

	rc = data_copy_bool_converted(d, ptr_buffer);

	log_flag(DATA, "%s: data "PRINTF_DATA_T" resolved string at path %s=%s: %s",
		 __func__, PRINTF_DATA_T_VAL(data), path,
		 (*ptr_buffer ? "true" : "false"), slurm_strerror(rc));

	return rc;
}

extern int data_retrieve_dict_path_int(const data_t *data, const char *path,
				       int64_t *ptr_buffer)
{
	const data_t *d = NULL;
	int rc;

	_check_magic(data);
	if (!(d = data_resolve_dict_path_const(data, path)))
		return ESLURM_DATA_PATH_NOT_FOUND;

	rc = data_get_int_converted(d, ptr_buffer);

	log_flag(DATA, "%s: data "PRINTF_DATA_T" resolved string at path %s to %"PRId64": %s",
		 __func__, PRINTF_DATA_T_VAL(data), path, *ptr_buffer,
		 slurm_strerror(rc));

	return rc;
}

extern char *data_type_to_string(data_type_t type)
{
	switch(type) {
		case DATA_TYPE_NULL:
			return "null";
		case DATA_TYPE_LIST:
			return "list";
		case DATA_TYPE_DICT:
			return "dictionary";
		case DATA_TYPE_INT_64:
			return "64 bit integer";
		case DATA_TYPE_STRING:
			return "string";
		case DATA_TYPE_FLOAT:
			return "floating point number";
		case DATA_TYPE_BOOL:
			return "boolean";
		default:
			return "INVALID";
	}
}
