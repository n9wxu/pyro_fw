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

#define TCP_MSS                       (1500 - 20 - 20)
#define TCP_SND_BUF                   (4 * TCP_MSS)
#define TCP_WND                       (4 * TCP_MSS)

#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define LWIP_SINGLE_NETIF            1
#define PBUF_POOL_SIZE               24
#define MEMP_NUM_TCP_PCB             8
#define MEMP_NUM_UDP_PCB             4
#define LWIP_MULTICAST_PING          1
#define LWIP_BROADCAST_PING          1

#endif
