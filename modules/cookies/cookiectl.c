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
#include <libknot/rrtype/opt-cookie.h>
#include <libknot/db/db_lmdb.h>
#include <stdlib.h>
#include <string.h>

#include "lib/cookies/alg_containers.h"
#include "modules/cookies/cookiectl.h"

#define NAME_CLIENT_ENABLED "client_enabled"
#define NAME_CLIENT_SECRET "client_secret"
#define NAME_CLIENT_COOKIE_ALG "client_cookie_alg"
#define NAME_AVAILABLE_CLIENT_COOKIE_ALGS "available_client_cookie_algs"

#define NAME_SERVER_ENABLED "server_enabled"
#define NAME_SERVER_SECRET "server_secret"
#define NAME_SERVER_COOKIE_ALG "server_cookie_alg"
#define NAME_AVAILABLE_SERVER_COOKIE_ALGS "available_server_cookie_algs"

/**
 * @brief Initialises cookie control context.
 * @param ctx cookie control context
 */
static void kr_cookie_ctx_init(struct kr_cookie_ctx *ctx)
{
	if (!ctx) {
		return;
	}

	memset(ctx, 0, sizeof(*ctx));

	ctx->clnt.current.alg_id = ctx->clnt.recent.alg_id = -1;
	ctx->srvr.current.alg_id = ctx->srvr.recent.alg_id = -1;
}

static bool aply_enabled(bool *enabled, const JsonNode *node)
{
	assert(enabled && node);

	if (node->tag == JSON_BOOL) {
		*enabled = node->bool_;
		return true;
	}

	return false;
}

static struct kr_cookie_secret *new_cookie_secret(size_t size, bool zero)
{
	if (!size) {
		return NULL;
	}

	struct kr_cookie_secret *sq = malloc(sizeof(*sq) + size);
	if (!sq) {
		return NULL;
	}

	sq->size = size;
	if (zero) {
		memset(sq->data, 0, size);
	}
	return sq;
}

static struct kr_cookie_secret *new_sq_str(const JsonNode *node)
{
	assert(node && node->tag == JSON_STRING);

	size_t len = strlen(node->string_);

	struct kr_cookie_secret *sq = new_cookie_secret(len, false);
	if (!sq) {
		return NULL;
	}
	memcpy(sq->data, node->string_, len);

	return sq;
}

#define holds_char(x) ((x) >= 0 && (x) <= 255)

static struct kr_cookie_secret *new_sq_array(const JsonNode *node)
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

	struct kr_cookie_secret *sq = new_cookie_secret(cnt, false);
	if (!sq) {
		return NULL;
	}

	cnt = 0;
	json_foreach(element, node) {
		sq->data[cnt++] = (uint8_t) element->number_;
	}

	return sq;
}

static bool apply_secret(struct kr_cookie_secret **sec, const JsonNode *node)
{
	assert(sec && node);

	struct kr_cookie_secret *sq = NULL;

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

	/* Overwrite data. */
	*sec = sq;

	return true;
}

static bool apply_hash_func(int *alg_id, const JsonNode *node,
                            const knot_lookup_t table[])
{
	assert(alg_id && node && table);

	if (node->tag == JSON_STRING) {
		const knot_lookup_t *lookup = knot_lookup_by_name(table,
		                                                  node->string_);
		if (!lookup) {
			return false;
		}
		*alg_id = lookup->id;
		return true;
	}

	return false;
}

static bool apply_configuration(struct kr_cookie_ctx *cntrl,
                                const JsonNode *node)
{
	assert(cntrl && node);

	if (!node->key) {
		/* All top most nodes must have names. */
		return false;
	}

	if (strcmp(node->key, NAME_CLIENT_ENABLED) == 0) {
		return aply_enabled(&cntrl->clnt.enabled, node);
	} else if (strcmp(node->key, NAME_CLIENT_SECRET) == 0) {
		return apply_secret(&cntrl->clnt.current.secr, node);
	} else  if (strcmp(node->key, NAME_CLIENT_COOKIE_ALG) == 0) {
		return apply_hash_func(&cntrl->clnt.current.alg_id, node,
		                       kr_cc_alg_names);
	} else if (strcmp(node->key, NAME_SERVER_ENABLED) == 0) {
		return aply_enabled(&cntrl->srvr.enabled, node);
	} else if (strcmp(node->key, NAME_SERVER_SECRET) == 0) {
		return apply_secret(&cntrl->srvr.current.secr, node);
	} else if (strcmp(node->key, NAME_SERVER_COOKIE_ALG) == 0) {
		return apply_hash_func(&cntrl->srvr.current.alg_id, node,
		                       kr_sc_alg_names);
	}

	return false;
}

static bool read_secret(JsonNode *root, const char *node_name,
                        const struct kr_cookie_secret *secret)
{
	assert(root && node_name && secret);

	JsonNode *array = json_mkarray();
	if (!array) {
		return false;
	}

	for (size_t i = 0; i < secret->size; ++i) {
		JsonNode *element = json_mknumber(secret->data[i]);
		if (!element) {
			goto fail;
		}
		json_append_element(array, element);
	}

	json_append_member(root, node_name, array);

	return true;

fail:
	if (array) {
		json_delete(array);
	}
	return false;
}

static bool read_available_hashes(JsonNode *root, const char *root_name,
                                  const knot_lookup_t table[])
{
	assert(root && root_name && table);

	JsonNode *array = json_mkarray();
	if (!array) {
		return false;
	}

	const knot_lookup_t *aux_ptr = table;
	while (aux_ptr && (aux_ptr->id >= 0) && aux_ptr->name) {
		JsonNode *element = json_mkstring(aux_ptr->name);
		if (!element) {
			goto fail;
		}
		json_append_element(array, element);
		++aux_ptr;
	}

	json_append_member(root, root_name, array);

	return true;

fail:
	if (array) {
		json_delete(array);
	}
	return false;
}

static bool settings_equal(const struct kr_cookie_comp *s1,
                           const struct kr_cookie_comp *s2)
{
	assert(s1 && s2 && s1->secr && s2->secr);

	if (s1->alg_id != s2->alg_id || s1->secr->size != s2->secr->size) {
		return false;
	}

	return 0 == memcmp(s1->secr->data, s2->secr->data, s1->secr->size);
}

static void apply_from_copy(struct kr_cookie_ctx *running,
                            const struct kr_cookie_ctx *shallow)
{
	assert(running && shallow);

	if (!settings_equal(&running->clnt.current, &shallow->clnt.current)) {
		free(running->clnt.recent.secr); /* Delete old secret. */
		running->clnt.recent = running->clnt.current;
		running->clnt.current = shallow->clnt.current;
		/* Shallow will be deleted after this function call. */
	}

	if (!settings_equal(&running->srvr.current, &shallow->srvr.current)) {
		free(running->srvr.recent.secr); /* Delete old secret. */
		running->srvr.recent = running->srvr.current;
		running->srvr.current = shallow->srvr.current;
		/* Shallow will be deleted after this function call. */
	}

	/* Direct application. */
	running->clnt.enabled = shallow->clnt.enabled;
	running->srvr.enabled = shallow->srvr.enabled;
}

bool config_apply(struct kr_cookie_ctx *ctx, const char *args)
{
	if (!ctx) {
		return false;
	}

	if (!args || !strlen(args)) {
		return true;
	}

	struct kr_cookie_ctx shallow_copy = *ctx;
	bool success = true;

	if (!args || !strlen(args)) {
		return success;
	}

	JsonNode *node;
	JsonNode *root_node = json_decode(args);
	json_foreach (node, root_node) {
		success = apply_configuration(&shallow_copy, node);
		if (!success) {
			break;
		}
	}
	json_delete(root_node);

	if (success) {
		apply_from_copy(ctx, &shallow_copy);
	} else {
		/* Clean newly allocated data. */
		if (shallow_copy.clnt.current.secr != ctx->clnt.current.secr) {
			free(shallow_copy.clnt.current.secr);
		}
		if (shallow_copy.srvr.current.secr != ctx->srvr.current.secr) {
			free(shallow_copy.srvr.current.secr);
		}
	}

	return success;
}

char *config_read(struct kr_cookie_ctx *ctx)
{
	if (!ctx) {
		return NULL;
	}

	const knot_lookup_t *lookup;
	char *result;
	JsonNode *root_node = json_mkobject();
	if (!root_node) {
		return NULL;
	}

	json_append_member(root_node, NAME_CLIENT_ENABLED,
	                   json_mkbool(ctx->clnt.enabled));

	read_secret(root_node, NAME_CLIENT_SECRET, ctx->clnt.current.secr);

	lookup = knot_lookup_by_id(kr_cc_alg_names, ctx->clnt.current.alg_id);
	if (lookup) {
		json_append_member(root_node, NAME_CLIENT_COOKIE_ALG,
		                   json_mkstring(lookup->name));
	}

	read_available_hashes(root_node, NAME_AVAILABLE_CLIENT_COOKIE_ALGS,
	                      kr_cc_alg_names);

	json_append_member(root_node, NAME_SERVER_ENABLED,
	                   json_mkbool(ctx->srvr.enabled));

	read_secret(root_node, NAME_SERVER_SECRET, ctx->srvr.current.secr);

	lookup = knot_lookup_by_id(kr_sc_alg_names, ctx->srvr.current.alg_id);
	if (lookup) {
		json_append_member(root_node, NAME_SERVER_COOKIE_ALG,
		                   json_mkstring(lookup->name));
	}

	read_available_hashes(root_node, NAME_AVAILABLE_SERVER_COOKIE_ALGS,
	                      kr_sc_alg_names);

	result = json_encode(root_node);
	json_delete(root_node);
	return result;
}

int config_init(struct kr_cookie_ctx *ctx)
{
	if (!ctx) {
		return kr_error(EINVAL);
	}

	kr_cookie_ctx_init(ctx);

	struct kr_cookie_secret *cs = new_cookie_secret(KNOT_OPT_COOKIE_CLNT,
	                                                true);
	struct kr_cookie_secret *ss = new_cookie_secret(KNOT_OPT_COOKIE_CLNT,
	                                                true);
	if (!cs || !ss) {
		free(cs);
		free(ss);
		return kr_error(ENOMEM);
	}

	const knot_lookup_t *clookup = knot_lookup_by_name(kr_cc_alg_names,
	                                                   "FNV-64");
	const knot_lookup_t *slookup = knot_lookup_by_name(kr_sc_alg_names,
	                                                   "FNV-64");
	if (!clookup || !slookup) {
		free(cs);
		free(ss);
		return kr_error(ENOKEY);
	}

	ctx->clnt.current.secr = cs;
	ctx->clnt.current.alg_id = clookup->id;

	ctx->srvr.current.secr = ss;
	ctx->srvr.current.alg_id = slookup->id;

	return kr_ok();
}

void config_deinit(struct kr_cookie_ctx *ctx)
{
	if (!ctx) {
		return;
	}

	ctx->clnt.enabled = false;

	free(ctx->clnt.recent.secr);
	ctx->clnt.recent.secr = NULL;

	free(ctx->clnt.current.secr);
	ctx->clnt.current.secr = NULL;

	ctx->srvr.enabled = false;

	free(ctx->srvr.recent.secr);
	ctx->srvr.recent.secr = NULL;

	free(ctx->srvr.current.secr);
	ctx->srvr.current.secr = NULL;
}