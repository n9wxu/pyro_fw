#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                        1
#define MEM_ALIGNMENT                 4
#define LWIP_RAW                      0
#define LWIP_NETCONN                  0
#define LWIP_SOCKET                   0
#define LWIP_DHCP                     0
#define LWIP_ICMP                     1
#define LWIP_UDP                      1
#define LWIP_TCP                      1
#define LWIP_IPV4                     1
#define LWIP_IPV6                     0
#define LWIP_IGMP                     1
#define ETH_PAD_SIZE                  0
#define LWIP_IP_ACCEPT_UDP_PORT(p)    ((p) == PP_NTOHS(67))

#define LWIP_MDNS_RESPONDER           1
#define MDNS_MAX_SERVICES             1
#define LWIP_NUM_NETIF_CLIENT_DATA    1
#define MEMP_NUM_SYS_TIMEOUT          16
unsigned int lwip_port_rand(void);
#define LWIP_RAND()                   ((u32_t)lwip_port_rand())

/* A connection's bytes wait in its own 2 kB rings (http_conn.h). What lwIP
 * holds beyond them is two segments each way: a pair, so a host that
 * acknowledges every second segment does so at once. */
#define TCP_MSS                       (1500 - 20 - 20)
#define TCP_SND_BUF                   (2 * TCP_MSS)
#define TCP_WND                       (2 * TCP_MSS)

#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define LWIP_SINGLE_NETIF            1
/* Received frames only, each held until its bytes are in an rx ring: at
 * most a window's worth for each connection sending to the board. */
#define PBUF_POOL_SIZE               12
#define MEMP_NUM_TCP_PCB             16
#define MEMP_NUM_UDP_PCB             4
/* TCP_SND_BUF for each of the four exchanges, with room left for the
 * SYN,ACK of a fifth connection. */
#define MEM_SIZE                      20000
#define MEMP_NUM_TCP_SEG              32
/* One link, one MTU: nothing to reassemble and nothing to fragment. */
#define IP_REASSEMBLY                 0
#define IP_FRAG                       0
#define LWIP_NOASSERT                 1
#define LWIP_MULTICAST_PING          1
#define LWIP_BROADCAST_PING          1

#endif
