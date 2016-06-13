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

#include <arpa/inet.h> /* ntohl(), ... */
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "contrib/fnv/fnv.h"
#include "lib/cookies/alg_clnt.h" /* kr_address_bytes() */
#include "lib/cookies/alg_srvr.h"

/**
 * @brief Server cookie contains only hash value.
 * @note DNS Cookies -- Appendix B.1
 */
static int srvr_cookie_parse_simple(const uint8_t *cookie_data, uint16_t data_len,
                                    struct kr_srvr_cookie_inbound *inbound)
{
	if (!cookie_data || !inbound) {
		return kr_error(EINVAL);
	}

	const uint8_t *cc = NULL, *sc = NULL;
        uint16_t cc_len = 0, sc_len = 0;
	int ret = knot_edns_opt_cookie_parse(cookie_data, data_len,
                                             &cc, &cc_len, &sc, &sc_len);
	if (ret != KNOT_EOK || !sc) {
		kr_error(EINVAL); /* Server cookie missing. */
	}
	assert(cc_len == KNOT_OPT_COOKIE_CLNT);

	//memset(inbound, 0, sizeof(*inbound));
	inbound->clnt_cookie = cc;
	inbound->hash_data = sc; /* Entire server cookie contains data. */
	inbound->hash_len = sc_len;

	return kr_ok();
}

/**
 * @brief Server cookie contains also additional values.
 * @note DNS Cookies -- Appendix B.2
 */
static int srvr_cookie_parse(const uint8_t *cookie_data, uint16_t data_len,
                             struct kr_srvr_cookie_inbound *inbound)
{
	if (!cookie_data || !inbound) {
		return kr_error(EINVAL);
	}

	const uint8_t *cc = NULL, *sc = NULL;
        uint16_t cc_len = 0, sc_len = 0;
	int ret = knot_edns_opt_cookie_parse(cookie_data, data_len,
                                             &cc, &cc_len, &sc, &sc_len);
	if (ret != KNOT_EOK || !sc) {
		kr_error(EINVAL); /* Server cookie missing. */
	}
	assert(cc_len == KNOT_OPT_COOKIE_CLNT);

	if (sc_len <= (2 * sizeof(uint32_t))) { /* nonce + time */
		return kr_error(EINVAL);
	}

	uint32_t aux;

	inbound->clnt_cookie = cc;
	memcpy(&aux, sc, sizeof(aux));
	inbound->nonce = ntohl(aux);
	memcpy(&aux, sc + sizeof(aux), sizeof(aux));
	inbound->time = ntohl(aux);
	inbound->hash_data = sc + (2 * sizeof(aux));
	inbound->hash_len = sc_len - (2 * sizeof(aux));

	return kr_ok();
}

#define SRVR_FNV64_SIMPLE_HASH_SIZE 8

/**
 * @brief Compute server cookie using FNV-64 (hash only).
 * @note Server cookie = FNV-64( client IP | client cookie | server secret )
 */
static int kr_srvr_cookie_alg_fnv64_simple(const struct kr_srvr_cookie_input *input,
                                           uint8_t sc_out[KNOT_OPT_COOKIE_SRVR_MAX],
                                           size_t *sc_size)
{
	if (!input || !sc_out ||
	    !sc_size || (*sc_size < SRVR_FNV64_SIMPLE_HASH_SIZE)) {
		return kr_error(EINVAL);
	}

	if (!input->clnt_cookie ||
	    !input->srvr_data.secret_data || !input->srvr_data.secret_len) {
		return kr_error(EINVAL);
	}

	const uint8_t *addr = NULL;
	size_t alen = 0; /* Address length. */

	Fnv64_t hash_val = FNV1A_64_INIT;

	if (kr_ok() == kr_address_bytes(input->srvr_data.clnt_sockaddr, &addr,
	                                &alen)) {
		assert(addr && alen);
		hash_val = fnv_64a_buf((void *) addr, alen, hash_val);
	}

	hash_val = fnv_64a_buf((void *) input->clnt_cookie,
	                       KNOT_OPT_COOKIE_CLNT, hash_val);

	hash_val = fnv_64a_buf((void *) input->srvr_data.secret_data,
	                       input->srvr_data.secret_len, hash_val);

	memcpy(sc_out, &hash_val, sizeof(hash_val));
	*sc_size = sizeof(hash_val);
	assert(SRVR_FNV64_SIMPLE_HASH_SIZE == *sc_size);

	return kr_ok();
}

#define SRVR_FNV64_SIZE 16

/**
 * @brief Compute server cookie using FNV-64.
 * @note Server cookie = nonce | time | FNV-64( client IP | nonce| time | client cookie | server secret )
 */
static int kr_srvr_cookie_alg_fnv64(const struct kr_srvr_cookie_input *input,
                                    uint8_t sc_out[KNOT_OPT_COOKIE_SRVR_MAX],
                                    size_t *sc_size)
{
	if (!input || !sc_out ||
	    !sc_size || (*sc_size < SRVR_FNV64_SIMPLE_HASH_SIZE)) {
		return kr_error(EINVAL);
	}

	if (!input->clnt_cookie ||
	    !input->srvr_data.secret_data || !input->srvr_data.secret_len) {
		return kr_error(EINVAL);
	}

	const uint8_t *addr = NULL;
	size_t alen = 0; /* Address length. */

	Fnv64_t hash_val = FNV1A_64_INIT;

	if (input->srvr_data.clnt_sockaddr) {
		if (kr_ok() == kr_address_bytes(input->srvr_data.clnt_sockaddr,
		                                &addr, &alen)) {
			assert(addr && alen);
			hash_val = fnv_64a_buf((void *) addr, alen, hash_val);
		}
	}

	hash_val = fnv_64a_buf((void *) &input->nonce, sizeof(input->nonce),
	                       hash_val);

	hash_val = fnv_64a_buf((void *) &input->time, sizeof(input->time),
	                       hash_val);

	hash_val = fnv_64a_buf((void *) input->clnt_cookie,
	                       KNOT_OPT_COOKIE_CLNT, hash_val);

	hash_val = fnv_64a_buf((void *) input->srvr_data.secret_data,
	                       input->srvr_data.secret_len, hash_val);

	uint32_t aux = htonl(input->nonce);
	memcpy(sc_out, &aux, sizeof(aux));
	aux = htonl(input->time);
	memcpy(sc_out + sizeof(aux), &aux, sizeof(aux));

	memcpy(sc_out + (2 * sizeof(aux)), &hash_val, sizeof(hash_val));
	*sc_size = (2 * sizeof(aux)) + sizeof(hash_val);
	assert(SRVR_FNV64_SIZE == *sc_size);

	return kr_ok();
}

#define SRVR_HMAC_SHA256_64_SIMPLE_HASH_SIZE 8

/**
 * @brief Compute server cookie using HMAC-SHA256-64 (hash only).
 * @note Server cookie = HMAC-SHA256-64( server secret, client cookie | client IP )
 */
static int kr_srvr_cookie_alg_hmac_sha256_64_simple(const struct kr_srvr_cookie_input *input,
                                                    uint8_t sc_out[KNOT_OPT_COOKIE_SRVR_MAX],
                                                    size_t *sc_size)
{
	if (!input || !sc_out ||
	    !sc_size || (*sc_size < SRVR_FNV64_SIMPLE_HASH_SIZE)) {
		return kr_error(EINVAL);
	}

	if (!input->clnt_cookie ||
	    !input->srvr_data.secret_data || !input->srvr_data.secret_len) {
		return kr_error(EINVAL);
	}

	const uint8_t *addr = NULL;
	size_t alen = 0; /* Address length. */

	uint8_t digest[SHA256_DIGEST_LENGTH];
	unsigned int digest_len = SHA256_DIGEST_LENGTH;

	HMAC_CTX ctx;
	HMAC_CTX_init(&ctx);

	int ret = HMAC_Init_ex(&ctx, input->srvr_data.secret_data,
	                       input->srvr_data.secret_len,
	                       EVP_sha256(), NULL);
	if (ret != 1) {
		ret = kr_error(EINVAL);
		goto fail;
	}

	ret = HMAC_Update(&ctx, input->clnt_cookie, KNOT_OPT_COOKIE_CLNT);
	if (ret != 1) {
		ret = kr_error(EINVAL);
		goto fail;
	}

	if (input->srvr_data.clnt_sockaddr) {
		if (kr_ok() == kr_address_bytes(input->srvr_data.clnt_sockaddr,
		                                &addr, &alen)) {
			assert(addr && alen);
			ret = HMAC_Update(&ctx, addr, alen);
			if (ret != 1) {
				ret = kr_error(EINVAL);
				goto fail;
			}
		}
	}

	if (1 != HMAC_Final(&ctx, digest, &digest_len)) {
		ret = kr_error(EINVAL);
		goto fail;
	}

	assert(SRVR_HMAC_SHA256_64_SIMPLE_HASH_SIZE <= SHA256_DIGEST_LENGTH);

	memcpy(sc_out, digest, SRVR_HMAC_SHA256_64_SIMPLE_HASH_SIZE);
	*sc_size = SRVR_HMAC_SHA256_64_SIMPLE_HASH_SIZE;

	ret = kr_ok();

fail:
	HMAC_CTX_cleanup(&ctx);
	return ret;
}

#define SRVR_HMAC_SHA256_64_SIZE 16

/**
 * @brief Compute server cookie using HMAC-SHA256-64).
 * @note Server cookie = nonce | time | HMAC-SHA256-64( server secret, client cookie | nonce| time | client IP )
 */
static int kr_srvr_cookie_alg_hmac_sha256_64(const struct kr_srvr_cookie_input *input,
                                             uint8_t sc_out[KNOT_OPT_COOKIE_SRVR_MAX],
                                             size_t *sc_size)
{
	if (!input || !sc_out ||
	    !sc_size || (*sc_size < SRVR_FNV64_SIMPLE_HASH_SIZE)) {
		return kr_error(EINVAL);
	}

	if (!input->clnt_cookie ||
	    !input->srvr_data.secret_data || !input->srvr_data.secret_len) {
		return kr_error(EINVAL);
	}

	const uint8_t *addr = NULL;
	size_t alen = 0; /* Address length. */

	uint8_t digest[SHA256_DIGEST_LENGTH];
	unsigned int digest_len = SHA256_DIGEST_LENGTH;

	HMAC_CTX ctx;
	HMAC_CTX_init(&ctx);

	int ret = HMAC_Init_ex(&ctx, input->srvr_data.secret_data,
	                       input->srvr_data.secret_len,
	                       EVP_sha256(), NULL);
	if (ret != 1) {
		ret = kr_error(EINVAL);
		goto fail;
	}

	ret = HMAC_Update(&ctx, input->clnt_cookie, KNOT_OPT_COOKIE_CLNT);
	if (ret != 1) {
		ret = kr_error(EINVAL);
		goto fail;
	}

	ret = HMAC_Update(&ctx, (void *) &input->nonce, sizeof(input->nonce));
	if (ret != 1) {
		ret = kr_error(EINVAL);
		goto fail;
	}

	ret = HMAC_Update(&ctx, (void *) &input->time, sizeof(input->time));
	if (ret != 1) {
		ret = kr_error(EINVAL);
		goto fail;
	}

	if (input->srvr_data.clnt_sockaddr) {
		if (kr_ok() == kr_address_bytes(input->srvr_data.clnt_sockaddr,
		                                &addr, &alen)) {
			assert(addr && alen);
			ret = HMAC_Update(&ctx, addr, alen);
			if (ret != 1) {
				ret = kr_error(EINVAL);
				goto fail;
			}
		}
	}

	if (1 != HMAC_Final(&ctx, digest, &digest_len)) {
		ret = kr_error(EINVAL);
		goto fail;
	}

	uint32_t aux = htonl(input->nonce);
	memcpy(sc_out, &aux, sizeof(aux));
	aux = htonl(input->time);
	memcpy(sc_out + sizeof(aux), &aux, sizeof(aux));

	assert(SRVR_HMAC_SHA256_64_SIMPLE_HASH_SIZE <= SHA256_DIGEST_LENGTH);

	memcpy(sc_out + (2 * sizeof(aux)), digest,
	       SRVR_HMAC_SHA256_64_SIMPLE_HASH_SIZE);
	*sc_size = (2 * sizeof(aux)) + SRVR_HMAC_SHA256_64_SIMPLE_HASH_SIZE;
	assert(SRVR_HMAC_SHA256_64_SIZE == *sc_size);

	ret = kr_ok();

fail:
	HMAC_CTX_cleanup(&ctx);
	return ret;
}
