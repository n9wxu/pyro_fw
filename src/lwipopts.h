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
#define ETH_PAD_SIZE                  0
#define LWIP_IP_ACCEPT_UDP_PORT(p)    ((p) == PP_NTOHS(67))

#define TCP_MSS                       (1500 - 20 - 20)
#define TCP_SND_BUF                   (4 * TCP_MSS)
#define TCP_WND                       (4 * TCP_MSS)

#define ETHARP_SUPPORT_STATIC_ENTRIES 1
#define LWIP_SINGLE_NETIF            1
#define PBUF_POOL_SIZE               8
#define MEMP_NUM_TCP_PCB             8
#define LWIP_MULTICAST_PING          1
#define LWIP_BROADCAST_PING          1

#endif
