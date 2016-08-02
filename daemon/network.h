/*  Copyright (C) 2015 CZ.NIC, z.s.p.o. <knot-dns@labs.nic.cz>

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

#pragma once

#include <uv.h>

#include "lib/generic/array.h"
#include "lib/generic/map.h"

enum endpoint_flag {
    NET_DOWN = 0 << 0,
    NET_UDP  = 1 << 0,
    NET_TCP  = 1 << 1,
    NET_TLS  = 1 << 2,
};

struct endpoint {
    uv_udp_t *udp;
    uv_tcp_t *tcp;
    uint16_t port;
    uint16_t flags;
};

/** @cond internal Array of endpoints */
typedef array_t(struct endpoint*) endpoint_array_t;
/* @endcond */

struct network {
    uv_loop_t *loop;
    map_t endpoints;
};

void network_init(struct network *net, uv_loop_t *loop);
void network_deinit(struct network *net);
int network_listen_fd(struct network *net, int fd);
int network_listen(struct network *net, const char *addr, uint16_t port, uint32_t flags);
int network_close(struct network *net, const char *addr, uint16_t port);
