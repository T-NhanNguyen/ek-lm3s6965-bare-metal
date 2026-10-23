#ifndef CALLER_LWIPOPTS_H
#define CALLER_LWIPOPTS_H
#ifndef CALLER_LWIP_CONTRACT
#error "Caller interface compile definition did not propagate"
#endif
#define NO_SYS 1
#define SYS_LIGHTWEIGHT_PROT 0
#define MEM_ALIGNMENT 4
#define MEM_SIZE 8192
#define MEMP_NUM_PBUF 10
#define PBUF_POOL_SIZE 6
#define PBUF_POOL_BUFSIZE 768
#define MEMP_NUM_TCP_PCB 6
#define MEMP_NUM_TCP_PCB_LISTEN 3
#define MEMP_NUM_TCP_SEG 20
#define MEMP_NUM_SYS_TIMEOUT 4
#define LWIP_TCP 1
#define TCP_MSS 256
#define TCP_WND 1024
#define TCP_SND_BUF 1024
#define TCP_SND_QUEUELEN 16
#define TCP_QUEUE_OOSEQ 0
#define TCP_OVERSIZE 0
#define LWIP_ARP 1
#define ARP_TABLE_SIZE 3
#define ARP_QUEUEING 0
#define LWIP_ETHERNET 1
#define LWIP_IPV4 1
#define LWIP_ICMP 1
#define IP_FRAG 0
#define IP_REASSEMBLY 0
#define LWIP_IPV6 0
#define LWIP_UDP 0
#define LWIP_UDPLITE 0
#define LWIP_RAW 0
#define LWIP_DNS 0
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_ACD 0
#define LWIP_IGMP 0
#define LWIP_ALTCP 0
#define LWIP_SOCKET 0
#define LWIP_NETCONN 0
#define LWIP_STATS 0
#endif
