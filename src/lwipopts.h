/*
 * lwIP's options for the USB network. Each is defined in lwIP's
 * src/include/lwip/opt.h; what is not set here keeps opt.h's default.
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* The raw API, driven by the net task alone: no lwIP threads. */
#define NO_SYS 1
#define LWIP_RAW 0
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0

/* Cortex-M0+ faults on an unaligned word access. */
#define MEM_ALIGNMENT 4

#define LWIP_IPV4 1
#define LWIP_IPV6 0
#define LWIP_ICMP 1 /* ping, which the support scripts use to find a board */
#define LWIP_UDP 1  /* DHCP, DNS and mDNS */
#define LWIP_TCP 1  /* HTTP */
#define LWIP_IGMP 1 /* mDNS joins its multicast group */
#define LWIP_DHCP 0 /* the board is the DHCP server (dhserver.c), not a client */

/* TinyUSB delivers frames with no leading pad. */
#define ETH_PAD_SIZE 0

/* A DHCP request reaches the server before the host has an address. */
#define LWIP_IP_ACCEPT_UDP_PORT(p) ((p) == PP_NTOHS(67))

/* [WEB-NET-03, WEB-NET-04] One host name and one service, _pyro._tcp. mDNS
 * keeps its per-interface state in netif client data, and its timers come on
 * top of lwIP's own. */
#define LWIP_MDNS_RESPONDER 1
#define MDNS_MAX_SERVICES 1
#define LWIP_NUM_NETIF_CLIENT_DATA 1
#define MEMP_NUM_SYS_TIMEOUT 16

/* TCP's initial sequence numbers and mDNS's probe delays. */
unsigned int lwip_port_rand(void);
#define LWIP_RAND() ((u32_t)lwip_port_rand())

/* One interface, the USB link, whose IP MTU is 1500 (net_glue.c): an MSS of
 * 1500 less the IP and TCP headers, and four segments in flight each way. */
#define LWIP_SINGLE_NETIF 1
#define TCP_MSS (1500 - 20 - 20)
#define TCP_SND_BUF (4 * TCP_MSS)
#define TCP_WND (4 * TCP_MSS)

/* Received frames, each a pool pbuf (net_glue.c), wait in a link's queue
 * until its connection takes them (WEB-HTTP-03): four connections' windows
 * of four segments, and the rest for DHCP, DNS and mDNS. */
#define PBUF_POOL_SIZE 24

/* A browser opens sockets speculatively; each costs a pcb and a link
 * (http_server.c), not a connection. */
#define MEMP_NUM_TCP_PCB 16
#define MEMP_NUM_UDP_PCB 4 /* DHCP, DNS, mDNS, and one spare */

/* The heap holds what tcp_write() copies: one connection's whole send buffer,
 * TCP_SND_BUF, with room for the stack's own allocations. */
#define MEM_SIZE 8000

#define LWIP_MULTICAST_PING 1
#define LWIP_BROADCAST_PING 1

#endif
