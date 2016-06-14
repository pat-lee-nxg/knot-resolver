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
#include <libknot/error.h>
#include <libknot/mm_ctx.h>
#include <libknot/packet/pkt.h>
#include <libknot/rrtype/opt_cookie.h> // branch dns-cookies-wip
#include <stdlib.h>
#include <string.h>

#include "daemon/engine.h"
#include "lib/cookies/alg_clnt.h"
#include "lib/cookies/alg_srvr.h"
#include "lib/cookies/cache.h"
#include "lib/cookies/control.h"
#include "lib/module.h"
#include "lib/layer.h"

#include "lib/layer/__/print_pkt.h"

#define DEBUG_MSG(qry, fmt...) QRDEBUG(qry, "cookiemonster",  fmt)

/* TODO -- The context must store sent cookies and server addresses in order
 * to make the process more reliable. */

/**
 * Obtain address from query/response context if if can be obtained.
 * @param qry query context
 * @return pointer to where the server socket address, NULL if not provided within context
 */
static const struct sockaddr *passed_server_sockaddr(const struct kr_query *qry)
{
	assert(qry);

	const struct sockaddr *tmp_sockaddr = NULL;
	if (qry->rsource.ip4.sin_family == AF_INET ||
	    qry->rsource.ip4.sin_family == AF_INET6) {
		tmp_sockaddr = (struct sockaddr *) &qry->rsource.ip4;
	}

	return tmp_sockaddr;
}

/**
 * Tries to guess the name server address from the reputation mechanism.
 * @param nsrep name server reputation context
 * @param cc client cookie data
 * @param csecr client secret
 * @param cc_alg client cookie algorithm
 * @return pointer to address if a matching found, NULL if none matches
 */
static const struct sockaddr *guess_server_addr(const struct kr_nsrep *nsrep,
                                                const uint8_t cc[KNOT_OPT_COOKIE_CLNT],
                                                const struct kr_cookie_secret *csecr,
                                                const struct kr_clnt_cookie_alg_descr *cc_alg)
{
	assert(nsrep && cc && csecr && cc_alg);

	const struct sockaddr *sockaddr = NULL;

	struct kr_clnt_cookie_input input = {
		.clnt_sockaddr = NULL,
		.srvr_sockaddr = NULL,
		.secret_data = csecr->data,
		.secret_len = csecr->size
	};

	/* Abusing name server reputation mechanism to obtain IP addresses. */
	for (int i = 0; i < KR_NSREP_MAXADDR; ++i) {
		if (nsrep->addr[i].ip.sa_family == AF_UNSPEC) {
			break;
		}

		input.srvr_sockaddr = &nsrep->addr[i];
		int ret = kr_clnt_cookie_check(cc, &input, cc_alg);
		if (ret == kr_ok()) {
			sockaddr = (struct sockaddr *) &nsrep->addr[i];
			break;
		}
	}

	return sockaddr;
}

/**
 * Obtain pointer to server socket address that matches obtained cookie.
 * @param sockaddr pointer to socket address to be set
 * @param is_current set to true if the cookie was generate from current secret
 * @param cc client cookie from the response
 * @param cntr cookie control structure
 * @return kr_ok() if matching address found, error code else
 */
static int srvr_sockaddr_cc_check(const struct sockaddr **sockaddr,
                                  bool *is_current, const struct kr_query *qry,
                                  const uint8_t cc[KNOT_OPT_COOKIE_CLNT],
                                  const struct kr_cookie_ctx *cntrl)
{
	assert(sockaddr && is_current && qry && cc && cntrl);

	const struct sockaddr *tmp_sockaddr = passed_server_sockaddr(qry);

	/* The address must correspond with the client cookie. */
	if (tmp_sockaddr) {
		assert(cntrl->current_cs);

		struct kr_clnt_cookie_input input = {
			.clnt_sockaddr = NULL,
			.srvr_sockaddr = tmp_sockaddr,
			.secret_data = cntrl->current_cs->data,
			.secret_len = cntrl->current_cs->size
		};
		int ret = kr_clnt_cookie_check(cc, &input, cntrl->cc_alg);
		bool have_current = (ret == kr_ok());
		if ((ret != kr_ok()) && cntrl->recent_cs) {
			input.secret_data = cntrl->recent_cs->data;
			input.secret_len = cntrl->recent_cs->size;
			ret = kr_clnt_cookie_check(cc, &input, cntrl->cc_alg);
		}
		if (ret == kr_ok()) {
			*sockaddr = tmp_sockaddr;
			*is_current = have_current;
		}
		return ret;
	}

	if (!cc || !cntrl) {
		return kr_error(EINVAL);
	}

	DEBUG_MSG(NULL, "%s\n",
	          "guessing response address from ns reputation");

	/* Abusing name server reputation mechanism to guess IP addresses. */
	const struct kr_nsrep *ns = &qry->ns;
	tmp_sockaddr = guess_server_addr(ns, cc, cntrl->current_cs,
	                                 cntrl->cc_alg);
	bool have_current = (tmp_sockaddr != NULL);
	if (!tmp_sockaddr && cntrl->recent_cs) {
		/* Try recent client secret to check obtained cookie. */
		tmp_sockaddr = guess_server_addr(ns, cc, cntrl->recent_cs,
		                                 cntrl->cc_alg);
	}
	if (tmp_sockaddr) {
		*sockaddr = tmp_sockaddr;
		*is_current = have_current;
	}

	return tmp_sockaddr ? kr_ok() : kr_error(EINVAL);
}

/**
 * Obtain cookie from cache.
 * @note The ttl and current time are respected. Outdated entries are ignored.
 * @param cache cache context
 * @param sockaddr key value
 * @param timestamp current time
 * @param remove_outdated true if outdated entries should be removed
 * @param cookie_opt entire EDNS cookie option (including header)
 * @return true if a cookie exists in cache
 */
static bool materialise_cookie_opt(struct kr_cache *cache,
                                   const struct sockaddr *sockaddr,
                                   uint32_t timestamp, bool remove_outdated,
                                   uint8_t cookie_opt[KR_COOKIE_OPT_MAX_LEN])
{
	assert(cache && sockaddr);

	bool found = false;
	struct timed_cookie timed_cookie = { 0, };

	int ret = kr_cookie_cache_peek_cookie(cache, sockaddr, &timed_cookie,
	                                      &timestamp);
	if (ret != kr_ok()) {
		return false;
	}
	assert(timed_cookie.cookie_opt);

	if (remove_outdated && (timed_cookie.ttl < timestamp)) {
		/* Outdated entries must be removed. */
		DEBUG_MSG(NULL, "%s\n", "removing outdated entry from cache");
		kr_cookie_cache_remove_cookie(cache, sockaddr);
		return false;
	}

	size_t cookie_opt_size = KNOT_EDNS_OPTION_HDRLEN +
	                         knot_edns_opt_get_length(timed_cookie.cookie_opt);
	assert(cookie_opt_size <= KR_COOKIE_OPT_MAX_LEN);

	if (cookie_opt) {
		memcpy(cookie_opt, timed_cookie.cookie_opt, cookie_opt_size);
	}
	return true;
}

/**
 * Check whether the supplied cookie is cached under the given key.
 * @param cache cache context
 * @param sockaddr key value
 * @param timestamp current time
 * @param cookie_opt cookie option to search for
 */
static bool is_cookie_cached(struct kr_cache *cache,
                             const struct sockaddr *sockaddr,
                             uint32_t timestamp,
                             const uint8_t *cookie_opt)
{
	assert(cache && sockaddr && cookie_opt);

	uint8_t cached_opt[KR_COOKIE_OPT_MAX_LEN];

	bool have_cached = materialise_cookie_opt(cache, sockaddr, timestamp,
	                                          false, cached_opt);
	if (!have_cached) {
		return false;
	}

	uint16_t cookie_opt_size = KNOT_EDNS_OPTION_HDRLEN +
	                           knot_edns_opt_get_length(cookie_opt);
	uint16_t cached_opt_size = KNOT_EDNS_OPTION_HDRLEN +
	                           knot_edns_opt_get_length(cached_opt);

	if (cookie_opt_size != cached_opt_size) {
		return false;
	}

	return memcmp(cookie_opt, cached_opt, cookie_opt_size) == 0;
}

/**
 * Check cookie content and store it to cache.
 */
static bool check_cookie_content_and_cache(struct kr_cookie_ctx *cntrl,
                                           struct kr_query *qry,
                                           uint8_t *pkt_cookie_opt,
                                           struct kr_cache *cache)
{
	assert(cntrl && qry && pkt_cookie_opt && cache);

	uint8_t *pkt_cookie_data = knot_edns_opt_get_data(pkt_cookie_opt);
	uint16_t pkt_cookie_len = knot_edns_opt_get_length(pkt_cookie_opt);
	assert(pkt_cookie_data && pkt_cookie_len);

	const uint8_t *pkt_cc = NULL, *pkt_sc = NULL;
	uint16_t pkt_cc_len = 0, pkt_sc_len = 0;

	int ret = knot_edns_opt_cookie_parse(pkt_cookie_data, pkt_cookie_len,
	                                     &pkt_cc, &pkt_cc_len,
	                                     &pkt_sc, &pkt_sc_len);
	if (ret != KNOT_EOK || !pkt_sc) {
		DEBUG_MSG(NULL, "%s\n",
		          "got malformed DNS cookie or server cookie missing");
		return false;
	}
	assert(pkt_cc_len == KNOT_OPT_COOKIE_CLNT);

	/* Check server address against received client cookie. */
	const struct sockaddr *srvr_sockaddr = NULL;
	bool returned_current = false;
	ret = srvr_sockaddr_cc_check(&srvr_sockaddr, &returned_current, qry,
	                             pkt_cc, cntrl);
	if (ret != kr_ok()) {
		DEBUG_MSG(NULL, "%s\n", "could not match received cookie");
		return false;
	}
	assert(srvr_sockaddr);

	/* Don't cache received cookies that don't match the current secret. */
	if (returned_current &&
	    !is_cookie_cached(cache, srvr_sockaddr, qry->timestamp.tv_sec,
	                      pkt_cookie_opt)) {
		struct timed_cookie timed_cookie = { cntrl->cache_ttl, pkt_cookie_opt };

		ret = kr_cookie_cache_insert_cookie(cache, srvr_sockaddr,
		                                    &timed_cookie,
		                                    qry->timestamp.tv_sec);
		if (ret != kr_ok()) {
			DEBUG_MSG(NULL, "%s\n", "failed caching cookie");
		} else {
			DEBUG_MSG(NULL, "%s\n", "cookie cached");
		}
	}

	return true;
}

/** Process incoming response. */
static int check_response(knot_layer_t *ctx, knot_pkt_t *pkt)
{
	struct kr_request *req = ctx->data;
	struct kr_query *qry = req->current_query;

	if (!kr_glob_cookie_ctx.enabled || (qry->flags & QUERY_TCP)) {
		return ctx->state;
	}

	/* Obtain cookie if present in response. Don't check actual content. */
	uint8_t *pkt_cookie_opt = NULL;
	if (knot_pkt_has_edns(pkt)) {
		pkt_cookie_opt = knot_edns_get_option(pkt->opt_rr,
		                                      KNOT_EDNS_OPTION_COOKIE);
	}

	struct kr_cache *cookie_cache = &req->ctx->cache;

	const struct sockaddr *srvr_sockaddr = passed_server_sockaddr(qry);

	if (!pkt_cookie_opt && srvr_sockaddr &&
	    materialise_cookie_opt(cookie_cache, srvr_sockaddr,
	                           qry->timestamp.tv_sec, true, NULL)) {
		/* We haven't received any cookies although we should. */
		DEBUG_MSG(NULL, "%s\n",
		          "expected to receive a cookie but none received");
		return KNOT_STATE_FAIL;
	}

	if (!pkt_cookie_opt) {
		/* Don't do anything if no cookies expected and received. */
		return ctx->state;
	}

	if (!check_cookie_content_and_cache(&kr_glob_cookie_ctx, qry,
	                                    pkt_cookie_opt, cookie_cache)) {
		return KNOT_STATE_FAIL;
	}

	uint16_t rcode = knot_pkt_get_ext_rcode(pkt);
	if (rcode == KNOT_RCODE_BADCOOKIE) {
		struct kr_query *next = NULL;
		if (!(qry->flags & QUERY_BADCOOKIE_AGAIN)) {
			/* Received first BADCOOKIE, regenerate query. */
			next = kr_rplan_push(&req->rplan, qry->parent,
			                     qry->sname,  qry->sclass,
			                     qry->stype);
		}

		if (next) {
			DEBUG_MSG(NULL, "%s\n", "BADCOOKIE querying again");
			qry->flags |= QUERY_BADCOOKIE_AGAIN;
		} else {
			/* Either the planning of second request failed or
			 * BADCOOKIE received for the second time.
			 * Fall back to TCP. */
			DEBUG_MSG(NULL, "%s\n", "falling back to TCP");
			qry->flags &= ~QUERY_BADCOOKIE_AGAIN;
			qry->flags |= QUERY_TCP;
		}

		return KNOT_STATE_CONSUME;
	}

	return ctx->state;
}

static int check_request(knot_layer_t *ctx, void *module_param)
{
	if (!kr_glob_cookie_ctx.enabled) {
		/* TODO -- IS there a way how to determine whether the original
		 * request came via TCP? */
		return ctx->state;
	}

	struct kr_request *req = ctx->data;
	const knot_rrset_t *req_opt_rr = req->qsource.opt;

	if (!req_opt_rr) {
		return ctx->state;
	}

	uint8_t *req_cookie_opt = knot_edns_get_option(req_opt_rr,
	                                               KNOT_EDNS_OPTION_COOKIE);
	if (!req_cookie_opt) {
		return ctx->state;
	}

	const uint8_t *req_cookie_data = knot_edns_opt_get_data(req_cookie_opt);
	uint16_t req_cookie_len = knot_edns_opt_get_length(req_cookie_opt);
	assert(req_cookie_data && req_cookie_len);

	const uint8_t *req_cc = NULL, *req_sc = NULL;
	uint16_t req_cc_len = 0, req_sc_len = 0;
	int ret = knot_edns_opt_cookie_parse(req_cookie_data, req_cookie_len,
	                                     &req_cc, &req_cc_len,
	                                     &req_sc, &req_sc_len);
	if (ret != KNOT_EOK) {
		/* TODO -- Generate BADCOOKIE response. */
		DEBUG_MSG(NULL, "%s\n", "got malformed DNS cookie in request");
		return ctx->state;
	}
	assert(req_cc_len == KNOT_OPT_COOKIE_CLNT);

	if (!req_sc) {
		/* TODO -- BADCOOKIE? Occasionally ignore? */
		DEBUG_MSG(NULL, "%s\n", "no server cookie in request");
		return ctx->state;
	}

	/* Check server cookie obtained in request. */

	const struct kr_cookie_ctx *cntrl = &kr_glob_cookie_ctx;

	if (!req->qsource.addr || !cntrl->current_ss) {
		/* TODO -- SERVFAIL? */
		DEBUG_MSG(NULL, "%s\n", "no server DNS cookie context data");
		return ctx->state;
	}

	struct kr_srvr_cookie_check_ctx check_ctx = {
		.clnt_sockaddr = req->qsource.addr,
		.secret_data = cntrl->current_ss->data,
		.secret_len = cntrl->current_ss->size
	};

	ret = kr_srvr_cookie_check(req_cc, req_sc, req_sc_len, &check_ctx,
	                           cntrl->sc_alg);
	if (ret != kr_ok()) {
		/* TODO -- BADCOOKIE? Occasionally ignore? */
		DEBUG_MSG(NULL, "%s\n", "invalid server DNS cookie data");
		return ctx->state;
	}

	/* Server cookie OK. */

	return ctx->state;
}

/** Module implementation. */

KR_EXPORT
const knot_layer_api_t *cookiemonster_layer(struct kr_module *module)
{
	static knot_layer_api_t _layer = {
		.begin = &check_request,
		.consume = &check_response
	};
	/* Store module reference */
	_layer.data = module;
	return &_layer;
}

KR_MODULE_EXPORT(cookiemonster)
