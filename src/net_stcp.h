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

/* The stack's net_init() and the firmware's (hal_common.c) are different
 * functions; CMake renames the stack's in its own sources the same way. */
#define net_init stcp_net_init
#include "net.h"
#include "tcp.h"
#undef net_init

net_t *net_stcp(void);

/* Responses completed since boot: what says this image's network works. */
extern volatile uint32_t net_http_served;

#endif
