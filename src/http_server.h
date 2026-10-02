/*
 * The HTTP server's entry points, for the net task (net_task.c). See
 * http_work.h for how the work between them is divided.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdbool.h>
#include <stdint.h>

void http_server_init(void);

/* Bytes between lwIP and the connections; no handler runs. */
void http_server_transport(void);

/* A period's slack begins. */
void http_server_period(void);

/* One unit of HTTP work, if one fits remaining_us. Returns whether one ran. */
bool http_server_work(int32_t remaining_us);

#endif
