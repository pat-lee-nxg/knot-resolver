/*  Copyright (C) 2016 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <ccan/json/json.h>
#include <libknot/db/db_lmdb.h>
#include <stdlib.h>
#include <string.h>

#include "daemon/engine.h"
#include "lib/cookies/control.h"
#include "lib/layer.h"

#define DEBUG_MSG(qry, fmt...) QRDEBUG(qry, "cookies_control",  fmt)

/** Find storage API with given prefix. */
static struct storage_api *find_storage_api(const storage_registry_t *registry,
                                            const char *prefix)
{
	assert(registry);
	assert(prefix);

	for (unsigned i = 0; i < registry->len; ++i) {
		struct storage_api *storage = &registry->at[i];
		if (strcmp(storage->prefix, "lmdb://") == 0) {
			return storage;
		}
	}

	return NULL;
}

#define NAME_ENABLED "enabled"
#define NAME_CLIENT_SECRET "client_secret"

static bool aply_enabled(struct cookies_control *cntrl, const JsonNode *node)
{
	if (node->tag == JSON_BOOL) {
		cntrl->enabled = node->bool_;
		return true;
	}

	return false;
}

static struct secret_quantity *new_sq_str(const JsonNode *node)
{
	assert(node && node->tag == JSON_STRING);

	size_t len = strlen(node->string_);

	struct secret_quantity *sq = malloc(sizeof(*sq) + len);
	if (!sq) {
		return NULL;
	}
	sq->size = len;
	memcpy(sq->data, node->string_, len);

	return sq;
}

#define holds_char(x) ((x) >= 0 && (x) <= 255)

static struct secret_quantity *new_sq_array(const JsonNode *node)
{
	assert(node && node->tag == JSON_ARRAY);

	const JsonNode *element = NULL;
	size_t cnt = 0;
	json_foreach(element, node) {
		if (element->tag != JSON_NUMBER || !holds_char(element->number_)) {
			return NULL;
		}
		++cnt;
	}
	if (cnt == 0) {
		return NULL;
	}

	struct secret_quantity *sq = malloc(sizeof(*sq) + cnt);
	if (!sq) {
		return NULL;
	}

	sq->size = cnt;
	cnt = 0;
	json_foreach(element, node) {
		sq->data[cnt++] = (uint8_t) element->number_;
	}

	return sq;
}

static bool apply_client_secret(struct cookies_control *cntrl, const JsonNode *node)
{
	struct secret_quantity *sq = NULL;

	switch (node->tag) {
	case JSON_STRING:
		sq = new_sq_str(node);
		break;
	case JSON_ARRAY:
		sq = new_sq_array(node);
		break;
	default:
		break;
	}

	if (!sq) {
		return false;
	}

	if (sq->size == cntrl->current_cs->size &&
	    memcmp(sq->data, cntrl->current_cs->data, sq->size) == 0) {
		/* Ignore same values. */
		free(sq);
		return true;
	}

	struct secret_quantity *tmp = cntrl->recent_cs;
	cntrl->recent_cs = cntrl->current_cs;
	cntrl->current_cs = sq;

	if (tmp && tmp != &dflt_cs) {
		free(tmp);
	}

	return true;
}

static bool apply_configuration(struct cookies_control *cntrl, const JsonNode *node)
{
	assert(cntrl && node);

	if (!node->key) {
		/* All top most nodes must have names. */
		return false;
	}

	if (strcmp(node->key, NAME_ENABLED) == 0) {
		return aply_enabled(cntrl, node);
	} else if (strcmp(node->key, NAME_CLIENT_SECRET) == 0) {
		return apply_client_secret(cntrl, node);
	}

	return false;
}

static bool read_secret(JsonNode *root, struct cookies_control *cntrl)
{
	assert(root && cntrl);

	JsonNode *array = json_mkarray();
	if (!array) {
		return false;
	}

	for (size_t i = 0; i < cntrl->current_cs->size; ++i) {
		JsonNode *element = json_mknumber(cntrl->current_cs->data[i]);
		if (!element) {
			goto fail;
		}
		json_append_element(array, element);
	}

	json_append_member(root, NAME_CLIENT_SECRET, array);

	return true;

fail:
	if (array) {
		json_delete(array);
	}
	return false;
}

/**
 * Get/set DNS cookie related stuff.
 *
 * Input: { name: value, ... }
 * Output: current configuration
 */
static char *cookies_control_config(void *env, struct kr_module *module, const char *args)
{
	if (args && strlen(args) > 0) {
		JsonNode *node;
		JsonNode *root_node = json_decode(args);
		json_foreach (node, root_node) {
			apply_configuration(&kr_cookies_control, node);
		}
		json_delete(root_node);
	}

	/* Return current configuration. */
	char *result = NULL;
	JsonNode *root_node = json_mkobject();
	json_append_member(root_node, NAME_ENABLED, json_mkbool(kr_cookies_control.enabled));
	read_secret(root_node, &kr_cookies_control);
	result = json_encode(root_node);
	json_delete(root_node);
	return result;
}

/*
 * Module implementation.
 */

KR_EXPORT
int cookies_control_init(struct kr_module *module)
{
	const char *storage_prefix = "lmdb://";
	struct engine *engine = module->data;
	DEBUG_MSG(NULL, "initialising with engine %p\n", (void *) engine);

	memset(&kr_cookies_control, 0, sizeof(kr_cookies_control));

	kr_cookies_control.enabled = false;

	kr_cookies_control.current_cs = &dflt_cs;

	memset(&kr_cookies_control.cache, 0, sizeof(kr_cookies_control.cache));

	struct storage_api *lmdb_storage_api = find_storage_api(&engine->storage_registry,
	                                                        storage_prefix);
	DEBUG_MSG(NULL, "found storage API %p for prefix '%s'\n",
	          (void *) lmdb_storage_api, storage_prefix);

	struct knot_db_lmdb_opts opts = KNOT_DB_LMDB_OPTS_INITIALIZER;
	opts.path = "cookies_db";
	//opts.dbname = "cookies";
	opts.mapsize = 1024 * 1024 * 1024;
	opts.maxdbs = 2;
	opts.flags.env = 0x80000 | 0x100000; /* MDB_WRITEMAP|MDB_MAPASYNC */

	errno = 0;
	int ret = kr_cache_open(&kr_cookies_control.cache,
	                        lmdb_storage_api->api(), &opts, engine->pool);
	DEBUG_MSG(NULL, "cache_open retval %d: %s\n", ret, kr_strerror(ret));

	module->data = NULL;

	return kr_ok();
}

KR_EXPORT
int cookies_control_deinit(struct kr_module *module)
{
	kr_cookies_control.enabled = false;

	if (kr_cookies_control.recent_cs &&
	    kr_cookies_control.recent_cs != &dflt_cs) {
		free(kr_cookies_control.recent_cs);
	}
	kr_cookies_control.recent_cs = NULL;

	if (kr_cookies_control.current_cs &&
	    kr_cookies_control.current_cs != &dflt_cs) {
		free(kr_cookies_control.current_cs);
	}
	kr_cookies_control.current_cs = &dflt_cs;

	kr_cache_close(&kr_cookies_control.cache);

	return kr_ok();
}

KR_EXPORT
struct kr_prop *cookies_control_props(void)
{
	static struct kr_prop prop_list[] = {
	    { &cookies_control_config, "config", "Empty value to return current configuration.", },
	    { NULL, NULL, NULL }
	};
	return prop_list;
}

KR_MODULE_EXPORT(cookies_control);
