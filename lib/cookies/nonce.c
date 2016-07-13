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

#include "contrib/wire.h"
#include "lib/cookies/nonce.h"

int kr_nonce_write_wire(uint8_t *buf, uint16_t *buf_len,
                        struct kr_nonce_input *input)
{
	if (!buf || !buf_len || !input) {
		return kr_error(EINVAL);
	}

	if (*buf_len < KR_NONCE_LEN) {
		return kr_error(EINVAL);
	}

	wire_write_u32(buf, input->rand);
	wire_write_u32(buf + sizeof(uint32_t), input->time);
	*buf_len = 2 * sizeof(uint32_t);
	assert(KR_NONCE_LEN == *buf_len);

	return kr_ok();
}
