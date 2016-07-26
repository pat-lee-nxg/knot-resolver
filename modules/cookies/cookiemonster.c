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
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <ccan/json/json.h>
#include <libknot/db/db_lmdb.h>
#include <libknot/error.h>
#include <libknot/mm_ctx.h>
#include <libknot/rrtype/opt-cookie.h>
#include <stdlib.h>
#include <string.h>

#include "lib/cookies/alg_containers.h"
#include "lib/cookies/control.h"
#include "lib/cookies/helper.h"
#include "lib/cookies/lru_cache.h"
#include "lib/cookies/nonce.h"
#include "lib/resolve.h"
#include "lib/rplan.h"
#include "modules/cookies/cookiemonster.h"

#define DEBUG_MSG(qry, fmt...) QRDEBUG(qry, "cookies",  fmt)

/**
 * Obtain address from query/response context if if can be obtained.
 * @param req resolution context
 * @return pointer to where the server socket address, NULL if not provided within context
 */
static const struct sockaddr *passed_server_sockaddr(const struct kr_request *req)
{
	if (!req || !req->upstream.addr) {
		return NULL;
	}

	if (req->upstream.addr->sa_family == AF_INET ||
	    req->upstream.addr->sa_family == AF_INET6) {
		return req->upstream.addr;
	}

	return NULL;
}

/**
 * Obtain pointer to server socket address that matches obtained cookie.
 * @param srvr_sa server socket address
 * @param cc         client cookie from the response
 * @param cc_len     client cookie size
 * @param clnt_sett  client cookie settings structure
 * @retval  1 if cookie matches current settings
 * @retval  0 if cookie matches recent settings
 * @return -1 if cookie does not match
 * @return -2 on any error
 */
static int srvr_sockaddr_cc_check(const struct sockaddr *srvr_sa,
                                  const uint8_t *cc, uint16_t cc_len,
                                  const struct kr_cookie_settings *clnt_sett)
{
	assert(cc && cc_len > 0 && clnt_sett);

	if (!srvr_sa) {
		return -2;
	}

	const struct knot_cc_alg *cc_alg = NULL;

	assert(clnt_sett->current.secr);

	/* The address must correspond with the client cookie. */
	struct knot_cc_input input = {
		.clnt_sockaddr = NULL, /* Not supported yet. */
		.srvr_sockaddr = srvr_sa,
		.secret_data = clnt_sett->current.secr->data,
		.secret_len = clnt_sett->current.secr->size
	};

	cc_alg = kr_cc_alg_get(clnt_sett->current.alg_id);
	if (!cc_alg) {
		return -2;
	}
	int comp_ret = -1; /* Cookie does not match. */
	int ret = knot_cc_check(cc, cc_len, &input, cc_alg);
	if (ret == KNOT_EOK) {
		comp_ret = 1;
	} else {
		cc_alg = kr_cc_alg_get(clnt_sett->recent.alg_id);
		if (clnt_sett->recent.secr && cc_alg) {
			input.secret_data = clnt_sett->recent.secr->data;
			input.secret_len = clnt_sett->recent.secr->size;
			ret = knot_cc_check(cc, cc_len, &input, cc_alg);
			if (ret == KNOT_EOK) {
				comp_ret = 0;
			}
		}
	}

	return comp_ret;
}

/**
 * Obtain cookie from cache.
 * @note Cookies with invalid length are ignored.
 * @param cache cache context
 * @param sa key value
 * @param cookie_opt entire EDNS cookie option (including header)
 * @return true if a cookie exists in cache
 */
static const uint8_t *get_cookie_opt(kr_cookie_lru_t *cache,
                                     const struct sockaddr *sa)
{
	assert(cache && sa);

	const uint8_t *cached_cookie_opt = kr_cookie_lru_get(cache, sa);
	if (!cached_cookie_opt) {
		return NULL;
	}

	size_t cookie_opt_size = KNOT_EDNS_OPTION_HDRLEN +
	                         knot_edns_opt_get_length(cached_cookie_opt);
	if (cookie_opt_size > KR_COOKIE_OPT_MAX_LEN) {
		return NULL;
	}

	return cached_cookie_opt;
}

/**
 * Check whether the supplied cookie is cached under the given key.
 * @param cache cache context
 * @param sa key value
 * @param cookie_opt cookie option to search for
 */
static bool is_cookie_cached(kr_cookie_lru_t *cache, const struct sockaddr *sa,
                             const uint8_t *cookie_opt)
{
	assert(cache && sa && cookie_opt);

	const uint8_t *cached_opt = get_cookie_opt(cache, sa);
	if (!cached_opt) {
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
static bool check_cookie_content_and_cache(const struct kr_cookie_settings *clnt_sett,
                                           struct kr_request *req,
                                           uint8_t *pkt_cookie_opt,
                                           kr_cookie_lru_t *cache)
{
	assert(clnt_sett && req && pkt_cookie_opt && cache);

	const uint8_t *pkt_cookie_data = knot_edns_opt_get_data(pkt_cookie_opt);
	uint16_t pkt_cookie_len = knot_edns_opt_get_length(pkt_cookie_opt);
	/* knot_edns_opt_cookie_parse() returns error on invalid data. */

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
	const struct sockaddr *srvr_sockaddr = passed_server_sockaddr(req);
	ret = srvr_sockaddr_cc_check(srvr_sockaddr, pkt_cc, pkt_cc_len,
	                             clnt_sett);
	if (ret < 0) {
		DEBUG_MSG(NULL, "%s\n", "could not match received cookie");
		return false;
	}
	assert(srvr_sockaddr);

	/* Don't cache received cookies that don't match the current secret. */
	if ((ret == 1) &&
	    !is_cookie_cached(cache, srvr_sockaddr, pkt_cookie_opt)) {
		ret = kr_cookie_lru_set(cache, srvr_sockaddr, pkt_cookie_opt);
		if (ret != kr_ok()) {
			DEBUG_MSG(NULL, "%s\n", "failed caching cookie");
		} else {
			DEBUG_MSG(NULL, "%s\n", "cookie cached");
		}
	}

	return true;
}

/** Process incoming response. */
int check_response(knot_layer_t *ctx, knot_pkt_t *pkt)
{
	struct kr_request *req = ctx->data;
	struct kr_query *qry = req->current_query;
	struct kr_cookie_ctx *cookie_ctx = &req->ctx->cookie_ctx;

	if (!cookie_ctx->clnt.enabled || (qry->flags & QUERY_TCP)) {
		return ctx->state;
	}

	/* Obtain cookie if present in response. Don't check actual content. */
	uint8_t *pkt_cookie_opt = NULL;
	if (knot_pkt_has_edns(pkt)) {
		pkt_cookie_opt = knot_edns_get_option(pkt->opt_rr,
		                                      KNOT_EDNS_OPTION_COOKIE);
	}

	kr_cookie_lru_t *cookie_cache = req->ctx->cache_cookie;

	const struct sockaddr *srvr_sockaddr = passed_server_sockaddr(req);

	if (!pkt_cookie_opt && srvr_sockaddr &&
	    get_cookie_opt(cookie_cache, srvr_sockaddr)) {
		/* We haven't received any cookies although we should. */
		DEBUG_MSG(NULL, "%s\n",
		          "expected to receive a cookie but none received");
		return KNOT_STATE_FAIL;
	}

	if (!pkt_cookie_opt) {
		/* Don't do anything if no cookies expected and received. */
		return ctx->state;
	}

	if (!check_cookie_content_and_cache(&cookie_ctx->clnt, req,
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

static inline uint8_t *req_cookie_option(struct kr_request *req)
{
	if (!req || !req->qsource.opt) {
		return NULL;
	}

	return knot_edns_get_option(req->qsource.opt, KNOT_EDNS_OPTION_COOKIE);
}

int check_request(knot_layer_t *ctx, void *module_param)
{
	struct kr_request *req = ctx->data;
	struct kr_cookie_settings *srvr_sett = &req->ctx->cookie_ctx.srvr;

	knot_pkt_t *answer = req->answer;

	if (!srvr_sett->enabled) {
		/* TODO -- IS there a way how to determine whether the original
		 * request came via TCP? */
		if (knot_pkt_has_edns(answer)) {
			/* Delete any cookies. */
			knot_edns_remove_options(answer->opt_rr,
			                         KNOT_EDNS_OPTION_COOKIE);
		}
		return ctx->state;
	}

	uint8_t *req_cookie_opt = req_cookie_option(req);
	if (!req_cookie_opt) {
		return ctx->state; /* Don't do anything without cookies. */
	}

	struct knot_dns_cookies cookies = { 0, };
	int ret = kr_parse_cookie_opt(req_cookie_opt, &cookies);
	if (ret != kr_ok()) {
		/* FORMERR -- malformed cookies. */
		DEBUG_MSG(NULL, "%s\n", "request with malformed cookie");
		knot_wire_set_rcode(answer->wire, KNOT_RCODE_FORMERR);
		return KNOT_STATE_FAIL | KNOT_STATE_DONE;
	}

	bool ignore_badcookie = true; /* TODO -- Occasionally ignore? */

	const struct knot_sc_alg *current_sc_alg = kr_sc_alg_get(srvr_sett->current.alg_id);

	if (!req->qsource.addr || !srvr_sett->current.secr || !current_sc_alg) {
		DEBUG_MSG(NULL, "%s\n", "missing valid server cookie context");
		return KNOT_STATE_FAIL;
	}

	int return_state = ctx->state;

	struct knot_sc_private srvr_data = {
		.clnt_sockaddr = req->qsource.addr,
		.secret_data = srvr_sett->current.secr->data,
		.secret_len = srvr_sett->current.secr->size
	};

	struct kr_nonce_input nonce = {
		.rand = kr_rand_uint(UINT32_MAX),
		.time = req->current_query->timestamp.tv_sec
	};

	const struct kr_query *qry = req->current_query;

	if (!cookies.sc) {
		/* Request has no server cookie. */
		if (qry->qdcount == 0) {
			/* RFC7873 5.4 */
			return_state = KNOT_STATE_DONE;
		} else if (!ignore_badcookie) { /* TODO -- Silently discard? */
			/* Generate BADCOOKIE response. */
			DEBUG_MSG(NULL, "%s\n",
			          "request is missing server cookie");
			if (!knot_pkt_has_edns(answer)) {
				DEBUG_MSG(NULL, "%s\n",
				          "missing EDNS section in prepared answer");
				return KNOT_STATE_FAIL;
			}
			kr_pkt_set_ext_rcode(answer, KNOT_RCODE_BADCOOKIE);
			return_state = KNOT_STATE_FAIL | KNOT_STATE_DONE;
		}
		goto answer_add_cookies;
	}

	/* Check server cookie obtained in request. */

	ret = knot_sc_check(KR_NONCE_LEN, &cookies, &srvr_data, current_sc_alg);
	if (ret == KNOT_EINVAL && srvr_sett->recent.secr) {
		const struct knot_sc_alg *recent_sc_alg = kr_sc_alg_get(srvr_sett->recent.alg_id);
		if (recent_sc_alg) {
			/* Try recent algorithm. */
			struct knot_sc_private recent_srvr_data = {
				.clnt_sockaddr = req->qsource.addr,
				.secret_data = srvr_sett->recent.secr->data,
				.secret_len = srvr_sett->recent.secr->size
			};
			ret = knot_sc_check(KR_NONCE_LEN, &cookies,
			                    &recent_srvr_data, recent_sc_alg);
		}
	}
	if (ret != KNOT_EOK) {
		/* Invalid server cookie. */
		if (qry->qdcount == 0) {
			/* RFC7873 5.4 */
			kr_pkt_set_ext_rcode(answer, KNOT_RCODE_BADCOOKIE);
			return_state = KNOT_STATE_DONE | KNOT_STATE_FAIL;
		} else if (!ignore_badcookie) { /* TODO -- Silently discard? */
			/* Generate BADCOOKIE response. */
			DEBUG_MSG(NULL, "%s\n",
			          "request has invalid server cookie");
			if (!knot_pkt_has_edns(req->answer)) {
				DEBUG_MSG(NULL, "%s\n",
				          "missing EDNS section in prepared answer");
				return KNOT_STATE_FAIL;
			}
			kr_pkt_set_ext_rcode(answer, KNOT_RCODE_BADCOOKIE);
			return_state = KNOT_STATE_FAIL | KNOT_STATE_DONE;
		}
		goto answer_add_cookies;
	}

	/* Server cookie is OK. */

answer_add_cookies:
	/* Add server cookie into response. */
	ret = kr_answer_write_cookie(&srvr_data, cookies.cc, cookies.cc_len,
	                             &nonce, current_sc_alg, answer);
	if (ret != kr_ok()) {
		return_state = KNOT_STATE_FAIL;
	}
	return return_state;
}
