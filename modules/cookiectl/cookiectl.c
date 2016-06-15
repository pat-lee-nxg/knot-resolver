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
#include "lib/cookies/alg_clnt.h"
#include "lib/cookies/alg_srvr.h"
#include "lib/cookies/control.h"
#include "lib/layer.h"

#define DEBUG_MSG(qry, fmt...) QRDEBUG(qry, "cookiectl",  fmt)

#define NAME_CLIENT_ENABLED "client_enabled"
#define NAME_CLIENT_SECRET "client_secret"
#define NAME_CLIENT_COOKIE_ALG "client_cookie_alg"
#define NAME_AVAILABLE_CLIENT_COOKIE_ALGS "available_client_cookie_algs"
#define NAME_CACHE_TTL "cache_ttl"

#define NAME_SERVER_ENABLED "server_enabled"
#define NAME_SERVER_SECRET "server_secret"
#define NAME_SERVER_COOKIE_ALG "server_cookie_alg"
#define NAME_AVAILABLE_SERVER_COOKIE_ALGS "available_server_cookie_algs"

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

static bool apply_client_hash_func(struct kr_cookie_ctx *cntrl,
                                   const JsonNode *node)
{
	if (node->tag == JSON_STRING) {
		const struct kr_clnt_cookie_alg_descr *cc_alg = kr_clnt_cookie_alg(kr_clnt_cookie_algs,
		                                                                   node->string_);
		if (!cc_alg) {
			return false;
		}
		cntrl->clnt.current.calg = cc_alg;
		return true;
	}

	return false;
}

static bool apply_server_hash_func(struct kr_cookie_ctx *cntrl,
                                   const JsonNode *node)
{
	if (node->tag == JSON_STRING) {
		const struct kr_srvr_cookie_alg_descr *sc_alg = kr_srvr_cookie_alg(kr_srvr_cookie_algs,
		                                                                   node->string_);
		if (!sc_alg) {
			return false;
		}
		cntrl->srvr.current.salg = sc_alg;
		return true;
	}

	return false;
}

static bool apply_cache_ttl(struct kr_cookie_ctx *cntrl, const JsonNode *node)
{
	if (node->tag == JSON_NUMBER) {
		cntrl->clnt.cache_ttl = node->number_;
		return true;
	}

	return false;
}

static bool apply_configuration(struct kr_cookie_ctx *cntrl, const JsonNode *node)
{
	assert(cntrl && node);

	if (!node->key) {
		/* All top most nodes must have names. */
		return false;
	}

	if (strcmp(node->key, NAME_CLIENT_ENABLED) == 0) {
		return aply_enabled(&cntrl->clnt.enabled, node);
	} else if (strcmp(node->key, NAME_CLIENT_SECRET) == 0) {
		return apply_secret(&cntrl->clnt.current.csec, node);
	} else  if (strcmp(node->key, NAME_CLIENT_COOKIE_ALG) == 0) {
		return apply_client_hash_func(cntrl, node);
	} else if (strcmp(node->key, NAME_CACHE_TTL) == 0) {
		return apply_cache_ttl(cntrl, node);
	} else if (strcmp(node->key, NAME_SERVER_ENABLED) == 0) {
		return aply_enabled(&cntrl->srvr.enabled, node);
	} else if (strcmp(node->key, NAME_SERVER_SECRET) == 0) {
		return apply_secret(&cntrl->srvr.current.ssec, node);
	} else if (strcmp(node->key, NAME_SERVER_COOKIE_ALG) == 0) {
		return apply_server_hash_func(cntrl, node);
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

static bool read_available_cc_hashes(JsonNode *root)
{
	assert(root);

	JsonNode *array = json_mkarray();
	if (!array) {
		return false;
	}

	const struct kr_clnt_cookie_alg_descr *aux_ptr = kr_clnt_cookie_algs;
	while (aux_ptr && aux_ptr->func) {
		assert(aux_ptr->name);
		JsonNode *element = json_mkstring(aux_ptr->name);
		if (!element) {
			goto fail;
		}
		json_append_element(array, element);
		++aux_ptr;
	}

	json_append_member(root, NAME_AVAILABLE_CLIENT_COOKIE_ALGS, array);

	return true;

fail:
	if (array) {
		json_delete(array);
	}
	return false;
}

static bool read_available_sc_hashes(JsonNode *root)
{
	assert(root);

	JsonNode *array = json_mkarray();
	if (!array) {
		return false;
	}

	const struct kr_srvr_cookie_alg_descr *aux_ptr = kr_srvr_cookie_algs;
	while (aux_ptr && aux_ptr->gen_func) {
		assert(aux_ptr->name);
		JsonNode *element = json_mkstring(aux_ptr->name);
		if (!element) {
			goto fail;
		}
		json_append_element(array, element);
		++aux_ptr;
	}

	json_append_member(root, NAME_AVAILABLE_SERVER_COOKIE_ALGS, array);

	return true;

fail:
	if (array) {
		json_delete(array);
	}
	return false;
}

static bool clnt_settings_equal(const struct kr_clnt_cookie_settings *s1,
                                const struct kr_clnt_cookie_settings *s2)
{
	assert(s1 && s2 && s1->csec && s2->csec);

	if (s1->calg != s2->calg || s1->csec->size != s2->csec->size) {
		return false;
	}

	return 0 == memcmp(s1->csec->data, s2->csec->data, s1->csec->size);
}

static bool srvr_settings_equal(const struct kr_srvr_cookie_settings *s1,
                                const struct kr_srvr_cookie_settings *s2)
{
	assert(s1 && s2 && s1->ssec && s2->ssec);

	if (s1->salg != s2->salg || s1->ssec->size != s2->ssec->size) {
		return false;
	}

	return 0 == memcmp(s1->ssec->data, s2->ssec->data, s1->ssec->size);
}

static void apply_from_copy(struct kr_cookie_ctx *running,
                            const struct kr_cookie_ctx *shallow)
{
	assert(running && shallow);

	if (!clnt_settings_equal(&running->clnt.current,
	                         &shallow->clnt.current)) {
		free(running->clnt.recent.csec); /* Delete old secret. */
		running->clnt.recent = running->clnt.current;
		running->clnt.current = shallow->clnt.current;
		/* Shallow will be deleted after this function call. */
	}

	if (!srvr_settings_equal(&running->srvr.current,
	                         &shallow->srvr.current)) {
		free(running->srvr.recent.ssec); /* Delete old secret. */
		running->srvr.recent = running->srvr.current;
		running->srvr.current = shallow->srvr.current;
		/* Shallow will be deleted after this function call. */
	}

	/* Direct application. */
	running->clnt.cache_ttl = shallow->clnt.cache_ttl;
	running->clnt.enabled = shallow->clnt.enabled;
	running->srvr.enabled = shallow->srvr.enabled;
}

static bool apply_config(struct kr_cookie_ctx *ctx, const char *args)
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
		if (shallow_copy.clnt.current.csec != ctx->clnt.current.csec) {
			free(shallow_copy.clnt.current.csec);
		}
		if (shallow_copy.srvr.current.ssec != ctx->srvr.current.ssec) {
			free(shallow_copy.srvr.current.ssec);
		}
	}

	return success;
}

char *read_config(struct kr_cookie_ctx *ctx)
{
	if (!ctx) {
		return NULL;
	}

	char *result = NULL;
	JsonNode *root_node = json_mkobject();

	json_append_member(root_node, NAME_CLIENT_ENABLED,
	                   json_mkbool(ctx->clnt.enabled));

	read_secret(root_node, NAME_CLIENT_SECRET, ctx->clnt.current.csec);

	assert(ctx->clnt.current.calg->name);
	json_append_member(root_node, NAME_CLIENT_COOKIE_ALG,
	                   json_mkstring(ctx->clnt.current.calg->name));

	read_available_cc_hashes(root_node);

	json_append_member(root_node, NAME_CACHE_TTL,
	                   json_mknumber(ctx->clnt.cache_ttl));

	json_append_member(root_node, NAME_SERVER_ENABLED,
	                   json_mkbool(ctx->srvr.enabled));

	read_secret(root_node, NAME_SERVER_SECRET, ctx->srvr.current.ssec);

	assert(ctx->srvr.current.salg->name);
	json_append_member(root_node, NAME_SERVER_COOKIE_ALG,
	                   json_mkstring(ctx->srvr.current.salg->name));

	read_available_sc_hashes(root_node);

	result = json_encode(root_node);
	json_delete(root_node);
	return result;
}

/**
 * Get/set DNS cookie related stuff.
 *
 * Input: { name: value, ... }
 * Output: current configuration
 */
static char *cookiectl_config(void *env, struct kr_module *module, const char *args)
{
	/* Apply configuration, if any. */
	apply_config(&kr_glob_cookie_ctx, args);

	/* Return current configuration. */
	return read_config(&kr_glob_cookie_ctx);
}

/*
 * Module implementation.
 */

KR_EXPORT
int cookiectl_init(struct kr_module *module)
{
	struct engine *engine = module->data;

	memset(&kr_glob_cookie_ctx, 0, sizeof(kr_glob_cookie_ctx));

	struct kr_cookie_secret *cs = new_cookie_secret(KNOT_OPT_COOKIE_CLNT,
	                                                true);
	struct kr_cookie_secret *ss = new_cookie_secret(KNOT_OPT_COOKIE_CLNT,
	                                                true);
	if (!cs || !ss) {
		free(cs);
		free(ss);
		return kr_error(ENOMEM);
	}

	kr_glob_cookie_ctx.clnt.enabled = false;
	kr_glob_cookie_ctx.clnt.current.csec = cs;
	kr_glob_cookie_ctx.clnt.current.calg = kr_clnt_cookie_alg(kr_clnt_cookie_algs,
	                                                          "FNV-64");
	kr_glob_cookie_ctx.clnt.cache_ttl = DFLT_COOKIE_TTL;

	kr_glob_cookie_ctx.srvr.enabled = false;
	kr_glob_cookie_ctx.srvr.current.ssec = ss;
	kr_glob_cookie_ctx.srvr.current.salg = kr_srvr_cookie_alg(kr_srvr_cookie_algs,
	                                                          "HMAC-SHA256-64");

	module->data = NULL;

	return kr_ok();
}

KR_EXPORT
int cookiectl_deinit(struct kr_module *module)
{
	kr_glob_cookie_ctx.clnt.enabled = false;

	free(kr_glob_cookie_ctx.clnt.recent.csec);
	kr_glob_cookie_ctx.clnt.recent.csec = NULL;

	free(kr_glob_cookie_ctx.clnt.current.csec);
	kr_glob_cookie_ctx.clnt.current.csec = NULL;

	kr_glob_cookie_ctx.srvr.enabled = false;

	free(kr_glob_cookie_ctx.srvr.recent.ssec);
	kr_glob_cookie_ctx.srvr.recent.ssec = NULL;

	free(kr_glob_cookie_ctx.srvr.current.ssec);
	kr_glob_cookie_ctx.srvr.current.ssec = NULL;

	return kr_ok();
}

KR_EXPORT
struct kr_prop *cookiectl_props(void)
{
	static struct kr_prop prop_list[] = {
	    { &cookiectl_config, "config", "Empty value to return current configuration.", },
	    { NULL, NULL, NULL }
	};
	return prop_list;
}

KR_MODULE_EXPORT(cookiectl);