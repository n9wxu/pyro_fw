/*
 * The smallest_tcp stack's one interface, shared by the USB glue
 * (net_glue_stcp.c) and the HTTP transport (http_transport_stcp.inc). Built
 * only with -DPYRO_NET_STACK=smallest_tcp: a spike, see
 * docs/smallest_tcp_evaluation.md.
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

net_t *net_stcp(void);

/* Responses completed since boot: what says this image's network works. */
extern volatile uint32_t net_http_served;

#endif
