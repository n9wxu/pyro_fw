/*
 * The smallest_tcp stack (github.com/n9wxu/smallest_tcp), as the USB glue
 * (net_glue.c) and the HTTP transport (http_transport.c) include it
 * [DD-102].
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef NET_STCP_H
#define NET_STCP_H

#include "net.h"
#include "tcp.h"
/* NET_API_PREFIX (stcp_, from CMake) makes a macro of every external name of
 * the stack, its HTTP module's among them. These four are also the
 * firmware's own functions, and keep their names. */
#undef net_init
#undef http_conn_init
#undef http_reason
#undef http_server_init

#endif
