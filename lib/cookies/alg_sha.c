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
#include <nettle/hmac.h>
#include <stdint.h>
#include <stdlib.h>

#include <libknot/errcode.h>
#include <libknot/rrtype/opt-cookie.h>

#include "lib/cookies/alg_sha.h"
#include "lib/utils.h"

/**
 * @brief Update hash value.
 *
 * @param ctx HMAC-SHA256 context to be updated.
 * @param sa  Socket address.
 */
static inline void update_hash(struct hmac_sha256_ctx *ctx,
                               const struct sockaddr *sa)
{
	assert(ctx && sa);

	int addr_len = kr_inaddr_len(sa);
	const uint8_t *addr = (uint8_t *)kr_inaddr(sa);

	if (addr && addr_len > 0) {
		hmac_sha256_update(ctx, addr_len, addr);
	}
}

/**
 * @brief Compute client cookie using HMAC_SHA256-64.
 * @note At least one of the arguments must be non-null.
 * @param input  input parameters
 * @param cc_out buffer for computed client cookie
 * @param cc_len buffer size
 * @return Non-zero size of written data on success, 0 in case of a failure.
 */
static uint16_t cc_gen_hmac_sha256_64(const struct knot_cc_input *input,
                                      uint8_t *cc_out, uint16_t cc_len)
{
	if (!input || !cc_out || cc_len < KNOT_OPT_COOKIE_CLNT) {
		return 0;
	}

	if ((!input->clnt_sockaddr && !input->srvr_sockaddr) ||
	    !(input->secret_data && input->secret_len)) {
		return 0;
	}

	struct hmac_sha256_ctx ctx;
	hmac_sha256_set_key(&ctx, input->secret_len, input->secret_data);

	if (input->clnt_sockaddr) {
		update_hash(&ctx, input->clnt_sockaddr);
	}

	if (input->srvr_sockaddr) {
		update_hash(&ctx, input->srvr_sockaddr);
	}

	assert(KNOT_OPT_COOKIE_CLNT <= SHA256_DIGEST_SIZE);

	cc_len = KNOT_OPT_COOKIE_CLNT;
	hmac_sha256_digest(&ctx, cc_len, cc_out);

	return cc_len;
}

#define SRVR_HMAC_SHA256_64_HASH_SIZE 8

/**
 * @brief Compute server cookie hash using HMAC-SHA256-64).
 * @note Server cookie = nonce | time | HMAC-SHA256-64( server secret, client cookie | nonce| time | client IP )
 * @param input    data to compute cookie from
 * @param hash_out hash output buffer
 * @param hash_len buffer size
 * @return Non-zero size of written data on success, 0 in case of a failure.
 */
static uint16_t sc_gen_hmac_sha256_64(const struct knot_sc_input *input,
                                      uint8_t *hash_out, uint16_t hash_len)
{
	if (!input || !hash_out || hash_len < SRVR_HMAC_SHA256_64_HASH_SIZE) {
		return 0;
	}

	if (!input->cc || !input->cc_len || !input->srvr_data ||
	    !input->srvr_data->secret_data || !input->srvr_data->secret_len) {
		return 0;
	}

	struct hmac_sha256_ctx ctx;
	hmac_sha256_set_key(&ctx, input->srvr_data->secret_len,
	                    input->srvr_data->secret_data);

	hmac_sha256_update(&ctx, input->cc_len, input->cc);

	if (input->nonce && input->nonce_len) {
		hmac_sha256_update(&ctx, input->nonce_len, input->nonce);
	}

	if (input->srvr_data->clnt_sockaddr) {
		update_hash(&ctx, input->srvr_data->clnt_sockaddr);
	}

	assert(SRVR_HMAC_SHA256_64_HASH_SIZE < SHA256_DIGEST_SIZE);

	hash_len = SRVR_HMAC_SHA256_64_HASH_SIZE;
	hmac_sha256_digest(&ctx, hash_len, hash_out);

	return hash_len;
}

const struct knot_cc_alg knot_cc_alg_hmac_sha256_64 = { KNOT_OPT_COOKIE_CLNT, cc_gen_hmac_sha256_64 };

const struct knot_sc_alg knot_sc_alg_hmac_sha256_64 = { SRVR_HMAC_SHA256_64_HASH_SIZE, sc_gen_hmac_sha256_64 };
