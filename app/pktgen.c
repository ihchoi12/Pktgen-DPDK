/*-
 * Copyright(c) <2010-2025>, Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
TXlib */

/* Created 2010 by Keith Wiles @ intel.com */

#include <stdint.h>
#include <time.h>
#include <inttypes.h>
#include <math.h>

#include <pg_delay.h>
#include <rte_lcore.h>
#include <lua_config.h>
#include <rte_net.h>
#include <rte_arp.h>
#include <rte_cycles.h>
#include <rte_hexdump.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "pktgen.h"

#ifdef AK_ENABLE_QUEUE_DEPTH_TRACKING
/* AK: Target burst interval (exported to DPDK tracking) */
extern uint64_t ak_target_burst_interval;
#endif
#include "pktgen-gre.h"
#include "pktgen-tcp.h"
#include "pktgen-ipv4.h"
#include "pktgen-ipv6.h"
#include "pktgen-udp.h"
#include "pktgen-arp.h"
#include "pktgen-vlan.h"
#include "pktgen-cpu.h"
#include "pktgen-display.h"
#include "pktgen-random.h"
#include "pktgen-log.h"
#include "pktgen-gtpu.h"
#include "pktgen-sys.h"
#include "pktgen-workq.h"
#include "pktgen_pcm.h"

#include <pthread.h>
#include <sched.h>
#define FAST_TX_MODE 0

/* Core statistics structure for summary reporting */
typedef struct {
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t filtered_rx_packets;   /* RX packets excluding unwanted traffic like DHCP */
    uint64_t filtered_tx_packets;   /* TX packets excluding unwanted traffic like DHCP */
    uint64_t start_time;
    uint16_t port_id;
} lcore_stats_t;

/* Global array to store per-lcore statistics */
static lcore_stats_t lcore_stats[RTE_MAX_LCORE];

/* RX Drop Analysis Structure */
typedef struct {
    uint64_t total_rx_attempts;     /* Total rte_eth_rx_burst calls */
    uint64_t zero_rx_count;         /* Times rx_burst returned 0 */
    uint64_t small_rx_count;        /* Times rx_burst returned < expected */
    uint64_t full_rx_count;         /* Times rx_burst returned max burst */
    uint64_t hw_drops_last;         /* Last HW drop count */
    uint64_t hw_drops_delta;        /* HW drop increase since last check */
    uint64_t mbuf_fail_count;       /* mbuf allocation failures */
    uint64_t ring_full_count;       /* Ring full incidents */
    uint64_t last_check_time;       /* Last analysis timestamp */
    uint64_t analysis_interval;     /* Analysis interval in cycles */
} rx_drop_analysis_t;

/* Per-core RX drop analysis */
static rx_drop_analysis_t rx_analysis[RTE_MAX_LCORE];

/* Function to print packet statistics summary */
void print_pktgen_stats_summary(void);

/* Function to print NIC hardware statistics */
void print_nic_hw_stats(uint16_t port_id);
void print_all_nic_hw_stats(void);

/* RX Drop Analysis Functions */
void init_rx_drop_analysis(unsigned int lcore_id);
void analyze_rx_performance(unsigned int lcore_id, uint16_t port_id, uint16_t nb_rx, uint16_t expected_rx);
void print_rx_drop_analysis(void);

/* Allocated the pktgen structure for global use */
pktgen_t pktgen;

/* Workqueue setup synchronization */
static volatile uint16_t workq_setup_done[RTE_MAX_ETHPORTS][2]; /* [port][WORKQ_RX/WORKQ_TX] */

/* Check if a packet is a DHCP packet (UDP ports 67/68) */
static inline int
is_dhcp_packet(struct rte_mbuf *pkt)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ipv4_hdr;
    struct rte_udp_hdr *udp_hdr;
    uint16_t src_port, dst_port;

    /* Check if packet is large enough and is IPv4 */
    if (rte_pktmbuf_data_len(pkt) < sizeof(*eth_hdr) + sizeof(*ipv4_hdr) + sizeof(*udp_hdr))
        return 0;

    eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    if (rte_be_to_cpu_16(eth_hdr->ether_type) != RTE_ETHER_TYPE_IPV4)
        return 0;

    ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    if (ipv4_hdr->next_proto_id != IPPROTO_UDP)
        return 0;

    udp_hdr = (struct rte_udp_hdr *)((char *)ipv4_hdr + sizeof(*ipv4_hdr));
    src_port = rte_be_to_cpu_16(udp_hdr->src_port);
    dst_port = rte_be_to_cpu_16(udp_hdr->dst_port);

    /* DHCP uses ports 67 (server) and 68 (client) */
    return (src_port == 67 || src_port == 68 || dst_port == 67 || dst_port == 68);
}

#ifdef RTE_LIBRTE_ETHDEV_DEBUG
/* Analyze and log packet details */
static void
analyze_packet(struct rte_mbuf *pkt, unsigned int lcore_id, uint16_t port_id, const char *direction)
{
    struct rte_ether_hdr *eth_hdr;
    struct rte_ipv4_hdr *ipv4_hdr;
    struct rte_tcp_hdr *tcp_hdr;
    struct rte_udp_hdr *udp_hdr;
    uint32_t src_ip, dst_ip;
    uint16_t src_port = 0, dst_port = 0;
    uint8_t proto;
    char src_ip_str[INET_ADDRSTRLEN];
    char dst_ip_str[INET_ADDRSTRLEN];
    char src_mac_str[18];
    char dst_mac_str[18];

    eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    uint16_t ether_type = rte_be_to_cpu_16(eth_hdr->ether_type);

    /* Convert MAC addresses to string format */
    snprintf(src_mac_str, sizeof(src_mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             eth_hdr->src_addr.addr_bytes[0], eth_hdr->src_addr.addr_bytes[1],
             eth_hdr->src_addr.addr_bytes[2], eth_hdr->src_addr.addr_bytes[3],
             eth_hdr->src_addr.addr_bytes[4], eth_hdr->src_addr.addr_bytes[5]);
    snprintf(dst_mac_str, sizeof(dst_mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             eth_hdr->dst_addr.addr_bytes[0], eth_hdr->dst_addr.addr_bytes[1],
             eth_hdr->dst_addr.addr_bytes[2], eth_hdr->dst_addr.addr_bytes[3],
             eth_hdr->dst_addr.addr_bytes[4], eth_hdr->dst_addr.addr_bytes[5]);

    if (ether_type == RTE_ETHER_TYPE_IPV4) {
        ipv4_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
        src_ip = rte_be_to_cpu_32(ipv4_hdr->src_addr);
        dst_ip = rte_be_to_cpu_32(ipv4_hdr->dst_addr);
        proto = ipv4_hdr->next_proto_id;

        /* Convert IPs to string format */
        struct in_addr addr;
        addr.s_addr = rte_cpu_to_be_32(src_ip);
        inet_ntop(AF_INET, &addr, src_ip_str, INET_ADDRSTRLEN);
        addr.s_addr = rte_cpu_to_be_32(dst_ip);
        inet_ntop(AF_INET, &addr, dst_ip_str, INET_ADDRSTRLEN);

        /* Extract port numbers for TCP/UDP */
        if (proto == IPPROTO_TCP && rte_pktmbuf_data_len(pkt) >= sizeof(*eth_hdr) + sizeof(*ipv4_hdr) + sizeof(*tcp_hdr)) {
            tcp_hdr = (struct rte_tcp_hdr *)((char *)ipv4_hdr + sizeof(*ipv4_hdr));
            src_port = rte_be_to_cpu_16(tcp_hdr->src_port);
            dst_port = rte_be_to_cpu_16(tcp_hdr->dst_port);
            uint16_t eth_hdr_size = sizeof(*eth_hdr);
            uint16_t ipv4_hdr_size = sizeof(*ipv4_hdr);
            uint16_t tcp_hdr_size = (tcp_hdr->data_off >> 4) * 4;
            uint16_t payload_size = pkt->pkt_len - eth_hdr_size - ipv4_hdr_size - tcp_hdr_size;

            AK_DEBUG_LOG_PKTGEN("PKTGEN: [%s] lcore=%u port=%u TCP %s:%u -> %s:%u (MAC %s -> %s) (RSS=0x%08x) pkt_len=%u eth=%u ipv4=%u tcp=%u payload=%u",
                direction, lcore_id, port_id, src_ip_str, src_port, dst_ip_str, dst_port, src_mac_str, dst_mac_str, pkt->hash.rss, pkt->pkt_len, eth_hdr_size, ipv4_hdr_size, tcp_hdr_size, payload_size);
        } else if (proto == IPPROTO_UDP && rte_pktmbuf_data_len(pkt) >= sizeof(*eth_hdr) + sizeof(*ipv4_hdr) + sizeof(*udp_hdr)) {
            udp_hdr = (struct rte_udp_hdr *)((char *)ipv4_hdr + sizeof(*ipv4_hdr));
            src_port = rte_be_to_cpu_16(udp_hdr->src_port);
            dst_port = rte_be_to_cpu_16(udp_hdr->dst_port);
            uint16_t eth_hdr_size = sizeof(*eth_hdr);
            uint16_t ipv4_hdr_size = sizeof(*ipv4_hdr);
            uint16_t udp_hdr_size = sizeof(*udp_hdr);
            uint16_t payload_size = pkt->pkt_len - eth_hdr_size - ipv4_hdr_size - udp_hdr_size;
            AK_DEBUG_LOG_PKTGEN("PKTGEN: [%s] lcore=%u port=%u UDP %s:%u -> %s:%u (MAC %s -> %s) (RSS=0x%08x) pkt_len=%u eth=%u ipv4=%u udp=%u payload=%u",
                direction, lcore_id, port_id, src_ip_str, src_port, dst_ip_str, dst_port, src_mac_str, dst_mac_str, pkt->hash.rss, pkt->pkt_len, eth_hdr_size, ipv4_hdr_size, udp_hdr_size, payload_size);
        } else {
            uint16_t payload_size = pkt->pkt_len - sizeof(*eth_hdr) - (ipv4_hdr->version_ihl & 0x0F) * 4;
            AK_DEBUG_LOG_PKTGEN("PKTGEN: [%s] lcore=%u port=%u IP proto=%u %s -> %s (MAC %s -> %s) (RSS=0x%08x) payload=%u",
                direction, lcore_id, port_id, proto, src_ip_str, dst_ip_str, src_mac_str, dst_mac_str, pkt->hash.rss, payload_size);
        }
    } else if (ether_type == RTE_ETHER_TYPE_IPV6) {
        uint16_t payload_size = pkt->pkt_len - sizeof(*eth_hdr) - 40; /* IPv6 header is 40 bytes */
        AK_DEBUG_LOG_PKTGEN("PKTGEN: [%s] lcore=%u port=%u IPv6 packet (MAC %s -> %s) payload=%u",
            direction, lcore_id, port_id, src_mac_str, dst_mac_str, payload_size);
    } else {
        uint16_t payload_size = pkt->pkt_len - sizeof(*eth_hdr);
        AK_DEBUG_LOG_PKTGEN("PKTGEN: [%s] lcore=%u port=%u Non-IP packet (type=0x%04x) (MAC %s -> %s) payload=%u",
            direction, lcore_id, port_id, ether_type, src_mac_str, dst_mac_str, payload_size);
    }
}
#endif
double
next_poisson_time(double rateParameter)
{
    return -logf(1.0f - ((double)random()) / (double)(RAND_MAX)) / rateParameter;
}

/**
 *
 * wire_size - Calculate the wire size of the data in bits to be sent.
 *
 * DESCRIPTION
 * Calculate the number of bytes/bits in a burst of traffic.
 *
 * RETURNS: Number of bytes in a burst of packets.
 *
 * SEE ALSO:
 */
static uint64_t
pktgen_wire_size(port_info_t *pinfo)
{
    uint64_t i, size = 0;

    if (pktgen_tst_port_flags(pinfo, SEND_PCAP_PKTS)) {
        pcap_info_t *pcap = l2p_get_pcap(pinfo->pid);

        size = WIRE_SIZE(pcap->max_pkt_size, uint64_t);
    } else {
        if (unlikely(pinfo->seqCnt > 0)) {
            for (i = 0; i < pinfo->seqCnt; i++)
                size += WIRE_SIZE(pinfo->seq_pkt[i].pkt_size, uint64_t);
            size = size / pinfo->seqCnt; /* Calculate the average sized packet */
        } else
            size = WIRE_SIZE(pinfo->seq_pkt[SINGLE_PKT].pkt_size, uint64_t);
    }
    return (size * 8);
}

/**
 *
 * pktgen_packet_rate - Calculate the transmit rate.
 *
 * DESCRIPTION
 * Calculate the number of cycles to wait between sending bursts of traffic.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
void
pktgen_packet_rate(port_info_t *port)
{
    uint64_t link_speed, wire_size, pps, cpb;

    wire_size = pktgen_wire_size(port);
    if (port->link.link_speed == 0) {
        port->tx_cycles = 0;
        port->tx_pps    = 0;
        return;
    }

    link_speed = (uint64_t)port->link.link_speed * Million;
    pps        = (((link_speed / wire_size) * ((port->tx_rate == 0) ? 1 : port->tx_rate)) / 100);
    pps        = ((pps > 0) ? pps : 1);
    cpb        = (rte_get_timer_hz() / pps) * (uint64_t)port->tx_burst; /* Cycles per Burst */

    // divide up the work between the number of transmit queues, so multiple by queue count.
    port->tx_cycles = cpb * (uint64_t)l2p_get_txcnt(port->pid);
    port->tx_pps    = pps;

#ifdef AK_ENABLE_QUEUE_DEPTH_TRACKING
    /* AK: Set target burst interval for DPDK tracking */
    ak_target_burst_interval = port->tx_cycles;
#endif

    /* AK: Log TX rate configuration (only once per port) */
    static uint16_t logged_ports = 0;
    if (!(logged_ports & (1 << port->pid))) {
        logged_ports |= (1 << port->pid);

        uint16_t num_tx_cores = l2p_get_txcnt(port->pid);
        uint64_t pkt_size_bytes = (wire_size / 8) - PKT_OVERHEAD_SIZE;
        uint64_t per_core_pps = (num_tx_cores > 0) ? (pps / num_tx_cores) : 0;

        printf("\n");
        printf("========================================\n");
        printf("AK: TX Rate Configuration (Port %d)\n", port->pid);
        printf("========================================\n");
        printf("Link Speed       : %.1f Gbps\n", (double)link_speed / 1000000000.0);
        printf("Packet Size      : %lu bytes\n", pkt_size_bytes);
        printf("Wire Size        : %lu bits (with 24-byte overhead)\n", wire_size);
        printf("Target Rate      : %.2f%%\n", port->tx_rate);
        printf("Target PPS       : %lu pps (%.2f Mpps)\n", pps, (double)pps / 1000000.0);
        printf("TX Cores         : %u\n", num_tx_cores);
        printf("Per-Core PPS     : %lu pps (%.2f Mpps)\n", per_core_pps, (double)per_core_pps / 1000000.0);
        printf("TX Burst         : %u packets\n", port->tx_burst);
        printf("TX Cycles        : %lu cycles (wait between bursts)\n", port->tx_cycles);
        printf("========================================\n");
        printf("\n");
    }
}

/**
 *
 * pktgen_fill_pattern - Create the fill pattern in a packet buffer.
 *
 * DESCRIPTION
 * Create a fill pattern based on the arguments for the packet data.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
static inline void
pktgen_fill_pattern(uint8_t *p, uint16_t len, uint32_t type, char *user)
{
    uint32_t i;

    switch (type) {
    case USER_FILL_PATTERN:
        memset(p, 0, len);
        for (i = 0; i < len; i++)
            p[i] = user[i & (USER_PATTERN_SIZE - 1)];
        break;

    case NO_FILL_PATTERN:
        break;

    case ZERO_FILL_PATTERN:
        memset(p, 0, len);
        break;

    default:
    case ABC_FILL_PATTERN: /* Byte wide ASCII pattern */
        for (i = 0; i < len; i++)
            p[i] = "abcdefghijklmnopqrstuvwxyz012345"[i & 0x1f];
        break;
    }
}

/**
 *
 * pktgen_find_matching_ipsrc - Find the matching IP source address
 *
 * DESCRIPTION
 * locate and return the pkt_seq_t pointer to the match IP address.
 *
 * RETURNS: index of sequence packets or -1 if no match found.
 *
 * SEE ALSO:
 */
int
pktgen_find_matching_ipsrc(port_info_t *pinfo, uint32_t addr)
{
    int i, ret = -1;
    uint32_t mask;

    addr = ntohl(addr);

    /* Search the sequence packets for a match */
    for (i = 0; i < pinfo->seqCnt; i++)
        if (addr == pinfo->seq_pkt[i].ip_src_addr.addr.ipv4.s_addr) {
            ret = i;
            break;
        }

    mask = size_to_mask(pinfo->seq_pkt[SINGLE_PKT].ip_src_addr.prefixlen);

    /* Now try to match the single packet address */
    if (ret == -1 ||
        (addr & mask) == (pinfo->seq_pkt[SINGLE_PKT].ip_dst_addr.addr.ipv4.s_addr * mask))
        ret = SINGLE_PKT;

    return ret;
}

/**
 *
 * pktgen_find_matching_ipdst - Find the matching IP destination address
 *
 * DESCRIPTION
 * locate and return the pkt_seq_t pointer to the match IP address.
 *
 * RETURNS: index of sequence packets or -1 if no match found.
 *
 * SEE ALSO:
 */
int
pktgen_find_matching_ipdst(port_info_t *pinfo, uint32_t addr)
{
    int i, ret = -1;

    addr = ntohl(addr);

    /* Search the sequence packets for a match */
    for (i = 0; i < pinfo->seqCnt; i++)
        if (addr == pinfo->seq_pkt[i].ip_dst_addr.addr.ipv4.s_addr) {
            ret = i;
            break;
        }

    /* Now try to match the single packet address */
    if (ret == -1 && addr == pinfo->seq_pkt[SINGLE_PKT].ip_dst_addr.addr.ipv4.s_addr)
        ret = SINGLE_PKT;

    /* Now try to match the range packet address */
    if (ret == -1 && addr == pinfo->seq_pkt[RANGE_PKT].ip_dst_addr.addr.ipv4.s_addr)
        ret = RANGE_PKT;

    return ret;
}

static inline tstamp_t *
pktgen_tstamp_pointer(port_info_t *pinfo __rte_unused, char *p)
{
    int offset = 0;

    offset += sizeof(struct rte_ether_hdr);
    offset += sizeof(struct rte_ipv4_hdr);
    offset += sizeof(struct rte_udp_hdr);
    offset = (offset + sizeof(uint64_t)) & ~(sizeof(uint64_t) - 1);

    return (tstamp_t *)(p + offset);
}

static inline void
pktgen_tstamp_inject(port_info_t *pinfo, uint16_t qid)
{
    pkt_seq_t *pkt = &pinfo->seq_pkt[LATENCY_PKT];
    rte_mbuf_t *mbuf;
    l2p_port_t *port;
    uint16_t sent;

    uint64_t curr_ts = pktgen_get_time();
    latency_t *lat   = &pinfo->latency;

    if (curr_ts >= lat->latency_timo_cycles) {
        lat->latency_timo_cycles = curr_ts + lat->latency_rate_cycles;

        port = l2p_get_port(pinfo->pid);
        if (rte_mempool_get(port->special_mp, (void **)&mbuf) == 0) {
            uint16_t pktsize = pkt->pkt_size;
            uint16_t to_send;

            mbuf->pkt_len  = pktsize;
            mbuf->data_len = pktsize;

            /* IPv4 Header constructor */
            pktgen_packet_ctor(pinfo, LATENCY_PKT, -2);

            rte_memcpy(rte_pktmbuf_mtod(mbuf, uint8_t *), (uint8_t *)pkt->hdr, pktsize);

            to_send = 1;
            do {
                sent = rte_eth_tx_burst(pinfo->pid, qid, &mbuf, to_send);
                to_send -= sent;
            } while (to_send > 0);

            lat->num_latency_tx_pkts++;
        } else
            printf("*** No more latency buffers\n");
    }
}

void
tx_send_packets(port_info_t *pinfo, uint16_t qid, struct rte_mbuf **pkts, uint16_t nb_pkts)
{
    if (nb_pkts) {
        uint16_t sent, to_send = nb_pkts;
        unsigned int lcore_id = rte_lcore_id();
        uint64_t filtered_count = 0;

        /* Single iteration through all TX packets */
        for (int i = 0; i < nb_pkts; i++) {
            /* Calculate packet bytes for queue statistics */
            pinfo->queue_stats.q_obytes[qid] += rte_pktmbuf_pkt_len(pkts[i]);

            /* Filter out unwanted packets (DHCP etc.) for statistics */
            if (stats_enabled && !is_dhcp_packet(pkts[i])) {
                filtered_count++;
            }

#ifdef RTE_LIBRTE_ETHDEV_DEBUG
            /* Analyze packet for debugging */
            analyze_packet(pkts[i], lcore_id, pinfo->pid, "TX");
#endif
        }

        /* Update statistics */
        pinfo->queue_stats.q_opackets[qid] += nb_pkts;
        if (stats_enabled) {
            lcore_stats[lcore_id].tx_packets += nb_pkts;
            lcore_stats[lcore_id].filtered_tx_packets += filtered_count;
            lcore_stats[lcore_id].port_id = pinfo->pid;
            if (lcore_stats[lcore_id].start_time == 0)
                lcore_stats[lcore_id].start_time = rte_rdtsc();
        } else {
            lcore_stats[lcore_id].tx_packets += nb_pkts;
        }
        if (pktgen_tst_port_flags(pinfo, SEND_RANDOM_PKTS))
            pktgen_rnd_bits_apply(pinfo, pkts, to_send, NULL);


#ifdef RTE_LIBRTE_ETHDEV_DEBUG
        /* PCIe byte tracking for hardware-level measurement using Intel PCM */
        uint64_t pcie_read_before = 0, pcie_write_before = 0;
        uint64_t pcie_read_after = 0, pcie_write_after = 0;
        uint32_t socket_id = rte_socket_id();
        AK_DEBUG_LOG_PKTGEN("[PCM TX BURST DEBUG] lcore=%u socket_id=%u pid=%u qid=%u nb_pkts=%u\n",
            lcore_id, socket_id, pinfo->pid, qid, nb_pkts);
        int pcm_available = pcm_monitoring_is_available();
        static __thread int pcm_debug_logged = 0;

        /* Get PCIe counters BEFORE transmission (instant snapshot) */
        if (pcm_available) {
            extern int pcm_wrapper_get_instant_pcie_bytes(uint32_t socket_id, uint64_t *read, uint64_t *write, uint64_t *pci_rdcur);
            int ret = pcm_wrapper_get_instant_pcie_bytes(socket_id, &pcie_read_before, &pcie_write_before, NULL);
            if (ret != 0) {
                if (!pcm_debug_logged) {
                    AK_DEBUG_LOG_PKTGEN("[PCM TX BURST DEBUG] pcm_wrapper_get_instant_pcie_bytes failed with ret=%d on lcore=%u socket=%u\n",
                           ret, lcore_id, socket_id);
                    pcm_debug_logged = 1;
                }
                pcm_available = 0;  /* Disable if failed */
            } else if (!pcm_debug_logged) {
                AK_DEBUG_LOG_PKTGEN("[PCM TX BURST DEBUG] First call succeeded: lcore=%u socket=%u, read_before=%lu, write_before=%lu\n",
                       lcore_id, socket_id, pcie_read_before, pcie_write_before);
                AK_DEBUG_LOG_PKTGEN("[PCM TX BURST DEBUG] Monitoring socket-specific PCIe traffic (socket %u only)\n", socket_id);
                pcm_debug_logged = 1;
            }
        } else if (!pcm_debug_logged) {
            AK_DEBUG_LOG_PKTGEN("[PCM TX BURST DEBUG] PCM not available on lcore=%u\n", lcore_id);
            pcm_debug_logged = 1;
        }
#endif
#ifdef AK_ENABLE_QUEUE_DEPTH_TRACKING
        /* AK: Track producer rate and timestamp (packets generated by application, ONCE per batch) */
        uint64_t burst_start_tsc;
        {
            extern struct ak_txq_depth_stats ak_txq_stats[];
            if (lcore_id < AK_MAX_LCORES) {
                uint64_t now = rte_get_tsc_cycles();
                burst_start_tsc = now;

                /* Record start timestamp on first packet */
                if (ak_txq_stats[lcore_id].start_timestamp == 0) {
                    ak_txq_stats[lcore_id].start_timestamp = now;
                    ak_txq_stats[lcore_id].last_burst_tsc = now;
                }

                /* Track burst interval (cycles between consecutive bursts) */
                if (ak_txq_stats[lcore_id].last_burst_tsc != 0) {
                    uint64_t interval = now - ak_txq_stats[lcore_id].last_burst_tsc;
                    __atomic_add_fetch(&ak_txq_stats[lcore_id].total_burst_interval,
                        interval, __ATOMIC_RELAXED);
                    __atomic_add_fetch(&ak_txq_stats[lcore_id].burst_count,
                        1, __ATOMIC_RELAXED);
                }
                ak_txq_stats[lcore_id].last_burst_tsc = now;

                /* Always update end timestamp */
                ak_txq_stats[lcore_id].end_timestamp = now;

                /* Count packets generated (not retries) */
                __atomic_add_fetch(&ak_txq_stats[lcore_id].producer_count,
                    nb_pkts, __ATOMIC_RELAXED);
            }
        }
#endif
        do {
            AK_DEBUG_LOG_PKTGEN("[1] rte_eth_tx_burst()");
            sent = rte_eth_tx_burst(pinfo->pid, qid, pkts, to_send);
            to_send -= sent;
            pkts += sent;
        } while (to_send > 0);
#ifdef AK_ENABLE_QUEUE_DEPTH_TRACKING
        /* AK: Track burst processing time (including retry loop) */
        {
            extern struct ak_txq_depth_stats ak_txq_stats[];
            if (lcore_id < AK_MAX_LCORES) {
                uint64_t burst_end_tsc = rte_get_tsc_cycles();
                uint64_t processing_time = burst_end_tsc - burst_start_tsc;
                __atomic_add_fetch(&ak_txq_stats[lcore_id].total_burst_processing_time,
                    processing_time, __ATOMIC_RELAXED);
            }
        }
#endif
#ifdef RTE_LIBRTE_ETHDEV_DEBUG
        /* Get PCIe counters AFTER transmission (instant snapshot) */
        if (pcm_available) {
            extern int pcm_wrapper_get_instant_pcie_bytes(uint32_t socket_id, uint64_t *read, uint64_t *write, uint64_t *pci_rdcur);
            if (pcm_wrapper_get_instant_pcie_bytes(socket_id, &pcie_read_after, &pcie_write_after, NULL) == 0) {
                /* Calculate PCIe bytes consumed for this transmission */
                uint64_t pcie_read_delta = pcie_read_after - pcie_read_before;
                uint64_t pcie_write_delta = pcie_write_after - pcie_write_before;

                /* Log first 10 bursts, then every 100 bursts up to 1000, then every 10000 */
                static __thread uint64_t log_counter = 0;
                log_counter++;
                AK_DEBUG_LOG_PKTGEN("log_counter: %lu", log_counter);
                int should_log = (log_counter <= 10) ||
                                (log_counter <= 1000 && log_counter % 100 == 0) ||
                                (log_counter % 10000 == 0);

                if (should_log) {
                    AK_DEBUG_LOG_PKTGEN("[PCM TX BURST] lcore=%u socket=%u port=%u qid=%u nb_pkts=%u burst#%lu | "
                           "PCIe Read: %lu bytes (%lu->%lu), PCIe Write: %lu bytes (%lu->%lu) | "
                           "Avg per pkt: Read=%.1f B, Write=%.1f B\n",
                           lcore_id, socket_id, pinfo->pid, qid, nb_pkts, log_counter,
                           pcie_read_delta, pcie_read_before, pcie_read_after,
                           pcie_write_delta, pcie_write_before, pcie_write_after,
                           (double)pcie_read_delta / nb_pkts,
                           (double)pcie_write_delta / nb_pkts);

                    /* Read PCIe counters again immediately to measure idle drift */
                    uint64_t pcie_read_idle = 0, pcie_write_idle = 0;
                    if (pcm_wrapper_get_instant_pcie_bytes(socket_id, &pcie_read_idle, &pcie_write_idle, NULL) == 0) {
                        uint64_t idle_read_delta = pcie_read_idle - pcie_read_after;
                        uint64_t idle_write_delta = pcie_write_idle - pcie_write_after;
                        AK_DEBUG_LOG_PKTGEN("[PCM IDLE CHECK] lcore=%u socket=%u | No packets sent | "
                               "PCIe Read drift: %lu bytes (%lu->%lu), PCIe Write drift: %lu bytes (%lu->%lu)\n",
                               lcore_id, socket_id,
                               idle_read_delta, pcie_read_after, pcie_read_idle,
                               idle_write_delta, pcie_write_after, pcie_write_idle);
                    }
                }
            }
            else{
                printf("[PCM TX BURST] pcm_wrapper_get_instant_pcie_bytes failed");
            }
        }
#endif

        if (qid == 0 && pktgen_tst_port_flags(pinfo, SEND_LATENCY_PKTS))
            pktgen_tstamp_inject(pinfo, qid);
    }
}

static inline void
pktgen_tstamp_check(port_info_t *pinfo, struct rte_mbuf **pkts, uint16_t nb_pkts)
{
    int lid = rte_lcore_id();
    int qid = l2p_get_rxqid(lid);
    int i;
    uint64_t cycles, jitter;
    latency_t *lat = &pinfo->latency;

    for (i = 0; i < nb_pkts; i++) {

        if (pktgen_tst_port_flags(pinfo, SEND_LATENCY_PKTS)) {
            tstamp_t *tstamp = pktgen_tstamp_pointer(pinfo, rte_pktmbuf_mtod(pkts[i], char *));

            if (tstamp->magic != TSTAMP_MAGIC)
                continue;

            cycles        = (pktgen_get_time() - tstamp->timestamp);
            tstamp->magic = 0UL; /* clear timestamp magic cookie */

            if (tstamp->index != lat->expect_index) {
                lat->expect_index = tstamp->index + 1;
                lat->num_skipped++;
                continue; /* Skip this latency packet */
            }
            lat->expect_index++;

            lat->num_latency_pkts++;

            if (pktgen_tst_port_flags(pinfo, SEND_LATENCY_PKTS)) {
                lat->running_cycles += cycles;

                if (lat->min_cycles == 0 || cycles < lat->min_cycles)
                    lat->min_cycles = cycles;
                if (lat->max_cycles == 0 || cycles > lat->max_cycles)
                    lat->max_cycles = cycles;

                jitter = (cycles > lat->prev_cycles) ? cycles - lat->prev_cycles
                                                     : lat->prev_cycles - cycles;
                if (jitter > lat->jitter_threshold_cycles)
                    lat->jitter_count++;

                lat->prev_cycles = cycles;
            }
            if (pktgen_tst_port_flags(pinfo, SAMPLING_LATENCIES)) {
                /* Record latency if it's time for sampling (seperately per lcore) */
                latsamp_stats_t *stats = &pinfo->latsamp_stats[qid];
                uint64_t now           = pktgen_get_time();

                stats->pkt_counter++;
                if (stats->next == 0 || now >= stats->next) {
                    if (stats->idx < stats->num_samples) {
                        stats->data[stats->idx] = (cycles * Billion) / rte_get_tsc_hz();
                        stats->idx++;
                    }

                    /* Calculate next sampling point */
                    if (pinfo->latsamp_type == LATSAMPLER_POISSON) {
                        double next_possion_time_ns = next_poisson_time(pinfo->latsamp_rate);

                        stats->next = now + next_possion_time_ns * (double)rte_get_tsc_hz();
                    } else        // LATSAMPLER_SIMPLE or LATSAMPLER_UNSPEC
                        stats->next = now + rte_get_tsc_hz() / pinfo->latsamp_rate;
                }
            }
        }
    }
}

/**
 *
 * pktgen_tx_flush - Flush Tx buffers from ring.
 *
 * DESCRIPTION
 * Flush TX buffers from ring.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
static inline void
pktgen_tx_flush(port_info_t *pinfo, uint16_t qid)
{
    rte_eth_tx_done_cleanup(pinfo->pid, qid, 0);
}

/**
 *
 * pktgen_exit_cleanup - Clean up the data and other items
 *
 * DESCRIPTION
 * Clean up the data.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
static inline void
pktgen_exit_cleanup(uint8_t lid __rte_unused)
{
}

/**
 *
 * pktgen_packet_ctor - Construct a complete packet with all headers and data.
 *
 * DESCRIPTION
 * Construct a packet type based on the arguments passed with all headers.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
void
pktgen_packet_ctor(port_info_t *pinfo, int32_t seq_idx, int32_t type)
{
    pkt_seq_t *pkt            = &pinfo->seq_pkt[seq_idx];
    uint16_t sport_entropy    = 0;
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)&pkt->hdr->eth;
    char *l3_hdr              = (char *)&eth[1]; /* Pointer to l3 hdr location for GRE header */
    uint16_t pktsz            = (pktgen.flags & JUMBO_PKTS_FLAG) ? RTE_ETHER_MAX_JUMBO_FRAME_LEN
                                                                 : RTE_ETHER_MAX_LEN;
    struct rte_eth_dev_info dev_info = {0};
    int ret;

    ret = rte_eth_dev_info_get(pinfo->pid, &dev_info);
    if (ret < 0)
        printf("Error during getting device (port %u) info: %s\n", pinfo->pid, strerror(-ret));

    /* Fill in the pattern for data space. */
    pktgen_fill_pattern((uint8_t *)pkt->hdr, pktsz, pinfo->fill_pattern_type, pinfo->user_pattern);

    if (seq_idx == LATENCY_PKT) {
        latency_t *lat = &pinfo->latency;
        tstamp_t *tstamp;

        tstamp = pktgen_tstamp_pointer(pinfo, (char *)pkt->hdr);

        tstamp->magic     = TSTAMP_MAGIC;
        tstamp->timestamp = pktgen_get_time();
        tstamp->index     = lat->next_index++;

        if (lat->latency_entropy)
            sport_entropy = (uint16_t)(pkt->sport + (tstamp->index % lat->latency_entropy));
    }

    /*
     * Randomizes the source IP address and port. Only randomizes if in the "single packet" setting
     * and not processing input packets.
     * For details, see https://github.com/pktgen/Pktgen-DPDK/pull/342
     */
    if (pktgen_tst_port_flags(pinfo, SEND_SINGLE_PKTS) &&
        !pktgen_tst_port_flags(pinfo, PROCESS_INPUT_PKTS)) {

        if (pktgen_tst_port_flags(pinfo, RANDOMIZE_SRC_IP))
            pkt->ip_src_addr.addr.ipv4.s_addr = pktgen_default_rnd_func();

        if (pktgen_tst_port_flags(pinfo, RANDOMIZE_SRC_PT))
            pkt->sport =
                (pktgen_default_rnd_func() % 65535) + 1; /* Avoid port 0, the only invalid port */
    }

    /* Add GRE header and adjust rte_ether_hdr pointer if requested */
    if (pktgen_tst_port_flags(pinfo, SEND_GRE_IPv4_HEADER))
        l3_hdr = pktgen_gre_hdr_ctor(pinfo, pkt, (greIp_t *)l3_hdr);
    else if (pktgen_tst_port_flags(pinfo, SEND_GRE_ETHER_HEADER))
        l3_hdr = pktgen_gre_ether_hdr_ctor(pinfo, pkt, (greEther_t *)l3_hdr);
    else
        l3_hdr = pktgen_ether_hdr_ctor(pinfo, pkt);

    uint64_t offload_capa = dev_info.tx_offload_capa;
    if (likely(pkt->ethType == RTE_ETHER_TYPE_IPV4)) {
        bool ipv4_cksum_offload = offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM;
        if (likely(pkt->ipProto == PG_IPPROTO_TCP)) {
            bool tcp_cksum_offload = offload_capa & RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
            if (pkt->dport != PG_IPPROTO_L4_GTPU_PORT) {
                /* Construct the TCP header */
                pktgen_tcp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV4, tcp_cksum_offload,
                                    pinfo->cksum_requires_phdr);

                /* IPv4 Header constructor */
                pktgen_ipv4_ctor(pkt, l3_hdr, ipv4_cksum_offload);
            } else {
                /* Construct the GTP-U header */
                pktgen_gtpu_hdr_ctor(pkt, l3_hdr, pkt->ipProto, GTPu_VERSION | GTPu_PT_FLAG, 0, 0,
                                     0);

                /* Construct the TCP header */
                pktgen_tcp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV4, tcp_cksum_offload,
                                    pinfo->cksum_requires_phdr);
                if (sport_entropy != 0) {
                    struct rte_ipv4_hdr *ipv4 = (struct rte_ipv4_hdr *)l3_hdr;
                    struct rte_tcp_hdr *tcp   = (struct rte_tcp_hdr *)&ipv4[1];

                    tcp->src_port = htons(sport_entropy & 0xFFFF);
                }

                /* IPv4 Header constructor */
                pktgen_ipv4_ctor(pkt, l3_hdr, ipv4_cksum_offload);
            }
        } else if (pkt->ipProto == PG_IPPROTO_UDP) {
            bool udp_cksum_offload = offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
            if (pktgen_tst_port_flags(pinfo, SEND_VXLAN_PACKETS)) {
                /* Construct the UDP header */
                pkt->dport = VXLAN_PORT_ID;
                pktgen_udp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV4, udp_cksum_offload,
                                    pinfo->cksum_requires_phdr);

                /* IPv4 Header constructor */
                pktgen_ipv4_ctor(pkt, l3_hdr, ipv4_cksum_offload);
            } else if (pkt->dport != PG_IPPROTO_L4_GTPU_PORT) {
                /* Construct the UDP header */
                pktgen_udp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV4, udp_cksum_offload,
                                    pinfo->cksum_requires_phdr);

                /* IPv4 Header constructor */
                pktgen_ipv4_ctor(pkt, l3_hdr, ipv4_cksum_offload);
            } else {
                /* Construct the GTP-U header */
                pktgen_gtpu_hdr_ctor(pkt, l3_hdr, pkt->ipProto, GTPu_VERSION | GTPu_PT_FLAG, 0, 0,
                                     0);

                /* Construct the UDP header */
                pktgen_udp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV4, udp_cksum_offload,
                                    pinfo->cksum_requires_phdr);
                if (sport_entropy != 0) {
                    struct rte_ipv4_hdr *ipv4 = (struct rte_ipv4_hdr *)l3_hdr;
                    struct rte_udp_hdr *udp   = (struct rte_udp_hdr *)&ipv4[1];

                    udp->src_port = htons(sport_entropy & 0xFFFF);
                }

                /* IPv4 Header constructor */
                pktgen_ipv4_ctor(pkt, l3_hdr, ipv4_cksum_offload);
            }
        } else if (pkt->ipProto == PG_IPPROTO_ICMP) {
            struct rte_ipv4_hdr *ipv4;
            struct rte_udp_hdr *udp;
            struct rte_icmp_hdr *icmp;
            uint16_t tlen;

            /* Start from Ethernet header */
            ipv4 = (struct rte_ipv4_hdr *)l3_hdr;
            udp  = (struct rte_udp_hdr *)&ipv4[1];

            /* Create the ICMP header */
            ipv4->src_addr = htonl(pkt->ip_src_addr.addr.ipv4.s_addr);
            ipv4->dst_addr = htonl(pkt->ip_dst_addr.addr.ipv4.s_addr);

            tlen = pkt->pkt_size - (pkt->ether_hdr_size + sizeof(struct rte_ipv4_hdr));
            ipv4->total_length  = htons(tlen);
            ipv4->next_proto_id = pkt->ipProto;

            icmp            = (struct rte_icmp_hdr *)&udp[1];
            icmp->icmp_code = 0;
            if ((type == -1) || (type == ICMP4_TIMESTAMP)) {
                union icmp_data *data = (union icmp_data *)&udp[1];

                icmp->icmp_type           = ICMP4_TIMESTAMP;
                data->timestamp.ident     = 0x1234;
                data->timestamp.seq       = 0x5678;
                data->timestamp.originate = 0x80004321;
                data->timestamp.receive   = 0;
                data->timestamp.transmit  = 0;
            } else if (type == ICMP4_ECHO) {
                union icmp_data *data = (union icmp_data *)&udp[1];

                icmp->icmp_type  = ICMP4_ECHO;
                data->echo.ident = 0x1234;
                data->echo.seq   = 0x5678;
                data->echo.data  = 0;
            }
            icmp->icmp_cksum = 0;
            /* ICMP4_TIMESTAMP_SIZE */
            tlen             = pkt->pkt_size - (pkt->ether_hdr_size + sizeof(struct rte_ipv4_hdr));
            icmp->icmp_cksum = rte_raw_cksum(icmp, tlen);
            if (icmp->icmp_cksum == 0)
                icmp->icmp_cksum = 0xFFFF;

            /* IPv4 Header constructor */
            pktgen_ipv4_ctor(pkt, l3_hdr, ipv4_cksum_offload);
        }
    } else if (pkt->ethType == RTE_ETHER_TYPE_IPV6) {
        if (pkt->ipProto == PG_IPPROTO_TCP) {
            bool tcp_cksum_offload = offload_capa & RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
            /* Construct the TCP header */
            pktgen_tcp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV6, tcp_cksum_offload,
                                pinfo->cksum_requires_phdr);
            if (sport_entropy != 0) {
                struct rte_ipv6_hdr *ipv6 = (struct rte_ipv6_hdr *)l3_hdr;
                struct rte_tcp_hdr *tcp   = (struct rte_tcp_hdr *)&ipv6[1];

                tcp->src_port = htons(sport_entropy & 0xFFFF);
            }

            /* IPv6 Header constructor */
            pktgen_ipv6_ctor(pkt, l3_hdr);
        } else if (pkt->ipProto == PG_IPPROTO_UDP) {
            bool udp_cksum_offload = offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
            /* Construct the UDP header */
            pktgen_udp_hdr_ctor(pkt, l3_hdr, RTE_ETHER_TYPE_IPV6, udp_cksum_offload,
                                pinfo->cksum_requires_phdr);
            if (sport_entropy != 0) {
                struct rte_ipv6_hdr *ipv6 = (struct rte_ipv6_hdr *)l3_hdr;
                struct rte_udp_hdr *udp   = (struct rte_udp_hdr *)&ipv6[1];

                udp->src_port = htons(sport_entropy & 0xFFFF);
            }

            /* IPv6 Header constructor */
            pktgen_ipv6_ctor(pkt, l3_hdr);
        }
    } else if (pkt->ethType == RTE_ETHER_TYPE_ARP) {
        /* Start from Ethernet header */
        struct rte_arp_hdr *arp = (struct rte_arp_hdr *)l3_hdr;

        arp->arp_hardware = htons(1);
        arp->arp_protocol = htons(RTE_ETHER_TYPE_IPV4);
        arp->arp_hlen     = RTE_ETHER_ADDR_LEN;
        arp->arp_plen     = 4;

        /* make request/reply operation selectable by user */
        arp->arp_opcode = htons(2);

        rte_ether_addr_copy(&pkt->eth_src_addr, &arp->arp_data.arp_sha);
        *((uint32_t *)&arp->arp_data.arp_sha) = htonl(pkt->ip_src_addr.addr.ipv4.s_addr);

        rte_ether_addr_copy(&pkt->eth_dst_addr, &arp->arp_data.arp_tha);
        *((uint32_t *)((void *)&arp->arp_data + offsetof(struct rte_arp_ipv4, arp_tip))) =
            htonl(pkt->ip_dst_addr.addr.ipv4.s_addr);
    } else
        pktgen_log_error("Unknown EtherType 0x%04x", pkt->ethType);
}

/**
 *
 * pktgen_packet_type - Examine a packet and return the type of packet
 *
 * DESCRIPTION
 * Examine a packet and return the type of packet.
 * the packet.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
static inline pktType_e
pktgen_packet_type(struct rte_mbuf *m)
{
    pktType_e ret;
    struct rte_ether_hdr *eth;

    eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    ret = ntohs(eth->ether_type);

    return ret;
}

/**
 *
 * pktgen_packet_classify - Examine a packet and classify it for statistics
 *
 * DESCRIPTION
 * Examine a packet and determine its type along with counting statistics around
 * the packet.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
static void
pktgen_packet_classify(struct rte_mbuf *m, int pid, int qid)
{
    port_info_t *pinfo     = l2p_get_port_pinfo(pid);
    pkt_stats_t *pkt_stats = &pinfo->pkt_stats;
    pkt_sizes_t *pkt_sizes = &pinfo->pkt_sizes;
    uint16_t plen;
    pktType_e pType;

    pType = pktgen_packet_type(m);

    /* Count the type of packets found. */
    switch ((int)pType) {
    case RTE_ETHER_TYPE_ARP:
        pkt_stats->arp_pkts++;
        break;
    case RTE_ETHER_TYPE_IPV4:
        pkt_stats->ip_pkts++;
        break;
    case RTE_ETHER_TYPE_IPV6:
        pkt_stats->ipv6_pkts++;
        break;
    case RTE_ETHER_TYPE_VLAN:
        pkt_stats->vlan_pkts++;
        break;
    default:
        break;
    }

    if (unlikely(pktgen_tst_port_flags(pinfo, PROCESS_INPUT_PKTS))) {
        switch ((int)pType) {
        case RTE_ETHER_TYPE_ARP:
            pktgen_process_arp(m, pid, qid, 0);
            break;
        case RTE_ETHER_TYPE_IPV4:
            pktgen_process_ping4(m, pid, qid, 0);
            break;
        case RTE_ETHER_TYPE_IPV6:
            pktgen_process_ping6(m, pid, qid, 0);
            break;
        case RTE_ETHER_TYPE_VLAN:
            pktgen_process_vlan(m, pid, qid);
            break;
        case UNKNOWN_PACKET: /* FALL THRU */
        default:
            break;
        }
    }

    plen = rte_pktmbuf_pkt_len(m) + RTE_ETHER_CRC_LEN;

    /* Count the size of each packet. */
    if (plen < RTE_ETHER_MIN_LEN)
        pkt_sizes->runt++;
    else if (plen > RTE_ETHER_MAX_LEN)
        pkt_sizes->jumbo++;
    else if (plen == RTE_ETHER_MIN_LEN)
        pkt_sizes->_64++;
    else if ((plen >= (RTE_ETHER_MIN_LEN + 1)) && (plen <= 127))
        pkt_sizes->_65_127++;
    else if ((plen >= 128) && (plen <= 255))
        pkt_sizes->_128_255++;
    else if ((plen >= 256) && (plen <= 511))
        pkt_sizes->_256_511++;
    else if ((plen >= 512) && (plen <= 1023))
        pkt_sizes->_512_1023++;
    else if ((plen >= 1024) && (plen <= RTE_ETHER_MAX_LEN))
        pkt_sizes->_1024_1518++;
    else {
        pktgen_log_info("Unknown packet size: %u", plen);
        pinfo->pkt_sizes.unknown++;
    }

    uint8_t *p = rte_pktmbuf_mtod(m, uint8_t *);

    /* Process multicast and broadcast packets. */
    if (unlikely(p[0] & 1)) {
        if ((p[0] == 0xff) && (p[1] == 0xff))
            pkt_sizes->broadcast++;
        else
            pkt_sizes->multicast++;
    }
}

/**
 *
 * pktgen_packet_classify_buld - Classify a set of packets in one call.
 *
 * DESCRIPTION
 * Classify a list of packets and to improve classify performance.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
#define PREFETCH_OFFSET 3
static inline void
pktgen_packet_classify_bulk(struct rte_mbuf **pkts, int nb_rx, int pid, int qid)
{
    int j, i;

    /* Prefetch first packets */
    for (j = 0; j < PREFETCH_OFFSET && j < nb_rx; j++)
        rte_prefetch0(rte_pktmbuf_mtod(pkts[j], void *));

    /* Prefetch and handle already prefetched packets */
    for (i = 0; i < (nb_rx - PREFETCH_OFFSET); i++) {
        rte_prefetch0(rte_pktmbuf_mtod(pkts[j], void *));
        j++;

        pktgen_packet_classify(pkts[i], pid, qid);
    }

    /* Handle remaining prefetched packets */
    for (; i < nb_rx; i++)
        pktgen_packet_classify(pkts[i], pid, qid);
}

/**
 *
 * pktgen_send_special - Send a special packet to the given port.
 *
 * DESCRIPTION
 * Create a special packet in the buffer provided.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
static void
pktgen_send_special(port_info_t *pinfo)
{
    if (!pktgen_tst_port_flags(pinfo, SEND_ARP_PING_REQUESTS))
        return;

    /* Send packets attached to the sequence packets. */
    for (uint32_t s = 0; s < pinfo->seqCnt; s++) {
        if (unlikely(pktgen_tst_port_flags(pinfo, SEND_GRATUITOUS_ARP)))
            pktgen_send_arp(pinfo->pid, GRATUITOUS_ARP, s);
        else if (unlikely(pktgen_tst_port_flags(pinfo, SEND_ARP_REQUEST)))
            pktgen_send_arp(pinfo->pid, 0, s);

        if (unlikely(pktgen_tst_port_flags(pinfo, SEND_PING4_REQUEST)))
            pktgen_send_ping4(pinfo->pid, s);
#ifdef INCLUDE_PING6
        if (unlikely(pktgen_tst_port_flags(pinfo, SEND_PING6_REQUEST)))
            pktgen_send_ping6(pinfo->pid, s);
#endif
    }

    /* Send the requests from the Single packet setup. */
    if (unlikely(pktgen_tst_port_flags(pinfo, SEND_GRATUITOUS_ARP)))
        pktgen_send_arp(pinfo->pid, GRATUITOUS_ARP, SINGLE_PKT);
    else if (unlikely(pktgen_tst_port_flags(pinfo, SEND_ARP_REQUEST)))
        pktgen_send_arp(pinfo->pid, 0, SINGLE_PKT);

    if (unlikely(pktgen_tst_port_flags(pinfo, SEND_PING4_REQUEST)))
        pktgen_send_ping4(pinfo->pid, SINGLE_PKT);
#ifdef INCLUDE_PING6
    if (unlikely(pktgen_tst_port_flags(pinfo, SEND_PING6_REQUEST)))
        pktgen_send_ping6(pinfo->pid, SINGLE_PKT);
#endif

    pktgen_clr_port_flags(pinfo, SEND_ARP_PING_REQUESTS);
}

struct pkt_setup_s {
    int32_t seq_idx;
    port_info_t *pinfo;
};

static inline void
mempool_setup_cb(struct rte_mempool *mp __rte_unused, void *opaque, void *obj,
                 unsigned obj_idx __rte_unused)
{
    struct rte_mbuf *m               = (struct rte_mbuf *)obj;
    struct pkt_setup_s *s            = (struct pkt_setup_s *)opaque;
    struct rte_eth_dev_info dev_info = {0};
    port_info_t *pinfo               = s->pinfo;
    int32_t idx, seq_idx = s->seq_idx;
    pkt_seq_t *pkt;
    int ret;

    ret = rte_eth_dev_info_get(pinfo->pid, &dev_info);
    if (ret != 0)
        printf("Error during getting device (port %u) info: %s\n", pinfo->pid, strerror(-ret));

    idx = seq_idx;
    if (pktgen_tst_port_flags(pinfo, SEND_SEQ_PKTS)) {
        idx = pinfo->seqIdx;

        /* move to the next packet in the sequence. */
        if (unlikely(++pinfo->seqIdx >= pinfo->seqCnt))
            pinfo->seqIdx = 0;
    }
    pkt = &pinfo->seq_pkt[idx];

    if (idx == RANGE_PKT)
        pktgen_range_ctor(&pinfo->range, pkt);

    pktgen_packet_ctor(pinfo, idx, -1);

    rte_memcpy(rte_pktmbuf_mtod(m, uint8_t *), (uint8_t *)pkt->hdr, pkt->pkt_size);

    m->pkt_len  = pkt->pkt_size;
    m->data_len = pkt->pkt_size;
    m->l2_len   = pkt->ether_hdr_size;
    m->l3_len   = sizeof(struct rte_ipv4_hdr);

    switch (pkt->ethType) {
    case RTE_ETHER_TYPE_IPV4:
        if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM)
            pkt->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;
        break;

    case RTE_ETHER_TYPE_IPV6:
        pkt->ol_flags = RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV6;
        break;

    case RTE_ETHER_TYPE_VLAN:
        if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_VLAN_INSERT) {
            /* TODO */
        }
        break;
    default:
        break;
    }

    switch (pkt->ipProto) {
    case PG_IPPROTO_UDP:
        if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_UDP_CKSUM)
            pkt->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
        break;
    case PG_IPPROTO_TCP:
        if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_TCP_CKSUM)
            pkt->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
        break;
    default:
        break;
    }
    m->ol_flags = pkt->ol_flags;
}

/**
 *
 * pktgen_setup_packets - Setup the default packets to be sent.
 *
 * DESCRIPTION
 * Construct the default set of packets for a given port.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
void
pktgen_setup_packets(uint16_t pid)
{
    struct rte_mempool *tx_mp = l2p_get_tx_mp(pid);
    l2p_port_t *port          = l2p_get_port(pid);
    port_info_t *pinfo        = l2p_get_port_pinfo(pid);

    if (unlikely(tx_mp == NULL))
        rte_exit(EXIT_FAILURE, "Invalid mempool for port %d\n", pid);

    if (port == NULL)
        rte_exit(EXIT_FAILURE, "Invalid l2p port for %d\n", pid);

    if (pktgen_tst_port_flags(pinfo, SETUP_TRANSMIT_PKTS)) {
        pktgen_clr_port_flags(pinfo, SETUP_TRANSMIT_PKTS);

        if (!pktgen_tst_port_flags(pinfo, SEND_PCAP_PKTS)) {
            struct pkt_setup_s s;
            int32_t idx = SINGLE_PKT;

            if (pktgen_tst_port_flags(pinfo, SEND_RANGE_PKTS)) {
                idx = RANGE_PKT;
            } else if (pktgen_tst_port_flags(pinfo, SEND_SEQ_PKTS))
                idx = FIRST_SEQ_PKT;
            else if (pktgen_tst_port_flags(pinfo, (SEND_SINGLE_PKTS | SEND_RANDOM_PKTS)))
                idx = SINGLE_PKT;

            s.pinfo   = pinfo;
            s.seq_idx = idx;
            rte_mempool_obj_iter(tx_mp, mempool_setup_cb, &s);
        }
    }
}

/**
 *
 * pktgen_send_pkts - Send a set of packet buffers to a given port.
 *
 * DESCRIPTION
 * Transmit a set of packets mbufs to a given port.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
void
pktgen_send_pkts(port_info_t *pinfo, uint16_t qid, struct rte_mempool *mp)
{
    uint64_t txCnt;
    struct rte_mbuf **pkts = pinfo->tx_pkts[qid];

    if (!pktgen_tst_port_flags(pinfo, SEND_FOREVER)) {
        txCnt = pkt_atomic64_tx_count(&pinfo->current_tx_count, pinfo->tx_burst);
        if (txCnt == 0) {
            pktgen_clr_port_flags(pinfo, SENDING_PACKETS);
            return;
        }
        if (txCnt > pinfo->tx_burst)
            txCnt = pinfo->tx_burst;
    } else
        txCnt = pinfo->tx_burst;

    if (rte_mempool_get_bulk(mp, (void **)pkts, txCnt) == 0)
        tx_send_packets(pinfo, qid, pkts, txCnt);
}

/**
 *
 * pktgen_main_transmit - Determine the next packet format to transmit.
 *
 * DESCRIPTION
 * Determine the next packet format to transmit for a given port.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
static inline void
pktgen_main_transmit(port_info_t *pinfo, uint16_t qid)
{
    uint16_t pid = pinfo->pid;

    /* Transmit ARP/Ping packets if needed */
    pktgen_send_special(pinfo);

    /* When not transmitting on this port then continue. */
    if (pktgen_tst_port_flags(pinfo, SENDING_PACKETS)) {
        struct rte_mempool *mp = l2p_get_tx_mp(pid);

        pinfo->qcnt[qid]++; /* Count the number of times queue is sending */

        if (pktgen_tst_port_flags(pinfo, SEND_PCAP_PKTS))
            mp = l2p_get_pcap_mp(pid);

        pktgen_send_pkts(pinfo, qid, mp);
    }
}

static __inline__ void
fast_main_transmit(port_info_t *pinfo, uint16_t qid)
{
    if (pktgen_tst_port_flags(pinfo, SENDING_PACKETS)) {
        struct rte_mempool *mp = l2p_get_tx_mp(pinfo->pid);
        struct rte_mbuf **pkts = pinfo->tx_pkts[qid];

        /* Use mempool routines instead of pktmbuf to make sure the mbufs is not altered */
        if (rte_mempool_get_bulk(mp, (void **)pkts, pinfo->tx_burst) == 0) {
            uint16_t sent, send = pinfo->tx_burst;
            do {
                sent = rte_eth_tx_burst(pinfo->pid, qid, pkts, send);
                send -= sent;
                pkts += sent;
            } while (send > 0);
        }
    }
}

/**
 *
 * pktgen_main_receive - Main receive routine for packets of a port.
 *
 * DESCRIPTION
 * Handle the main receive set of packets on a given port plus handle all of the
 * input processing if required.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
static inline void
pktgen_main_receive(port_info_t *pinfo, uint16_t qid)
{
    uint16_t nb_rx, nb_pkts = pinfo->rx_burst, pid;
    struct rte_mbuf **pkts = pinfo->rx_pkts[qid];

    if (unlikely(pktgen_tst_port_flags(pinfo, STOP_RECEIVING_PACKETS)))
        return;

    pid = pinfo->pid;
    unsigned int lcore_id = rte_lcore_id();

    /* Initialize RX analysis if not done */
    if (stats_enabled && rx_analysis[lcore_id].analysis_interval == 0) {
        init_rx_drop_analysis(lcore_id);
    }

    /* Read packets from RX queues and free the mbufs */
    nb_rx = rte_eth_rx_burst(pid, qid, pkts, nb_pkts);

    /* Analyze RX performance */
    if (stats_enabled) {
        analyze_rx_performance(lcore_id, pid, nb_rx, nb_pkts);
    }

    if (likely(nb_rx > 0)) {
        struct rte_eth_stats *qstats = &pinfo->queue_stats;
        unsigned int lcore_id = rte_lcore_id();
        uint64_t filtered_count = 0;

        /* Single iteration through all received packets */
        for (int i = 0; i < nb_rx; i++) {
            /* Calculate packet bytes for queue statistics */
            qstats->q_ibytes[qid] += rte_pktmbuf_pkt_len(pkts[i]);

            /* Filter out unwanted packets (DHCP etc.) for statistics */
            if (stats_enabled && !is_dhcp_packet(pkts[i])) {
                filtered_count++;
            }

#ifdef RTE_LIBRTE_ETHDEV_DEBUG
            /* Analyze packet for debugging */
            analyze_packet(pkts[i], lcore_id, pid, "RX");
#endif
        }

        /* Update statistics */
        qstats->q_ipackets[qid] += nb_rx;
        if (stats_enabled) {
            lcore_stats[lcore_id].rx_packets += nb_rx;
            lcore_stats[lcore_id].filtered_rx_packets += filtered_count;
            lcore_stats[lcore_id].port_id = pid;
            if (lcore_stats[lcore_id].start_time == 0)
                lcore_stats[lcore_id].start_time = rte_rdtsc();
        }
        pktgen_tstamp_check(pinfo, pkts, nb_rx);

        /* Skip packet classification in high-performance mode */
        // pktgen_packet_classify_bulk(pkts, nb_rx, pid, qid);

        if (unlikely(pinfo->dump_count > 0))
            pktgen_packet_dump_bulk(pkts, nb_rx, pid);

        if (unlikely(pktgen_tst_port_flags(pinfo, CAPTURE_PKTS))) {
            capture_t *capture = &pktgen.capture[pg_socket_id()];

            if (unlikely(capture->port == pid))
                pktgen_packet_capture_bulk(pkts, nb_rx, capture);
        }

        rte_pktmbuf_free_bulk(pkts, nb_rx);
    }
}

static int
pktgen_rx_workq_setup(uint16_t pid)
{
    workq_fn funcs[] = {pktgen_main_receive};

    for (uint16_t i = 0; i < RTE_DIM(funcs); i++) {
        if (workq_add(WORKQ_RX, pid, funcs[i]))
            return -1;
    }
    return 0;
}

static int
pktgen_tx_workq_setup(uint16_t pid)
{
#if FAST_TX_MODE
    workq_fn funcs[] = {fast_main_transmit};
    (void)pktgen_main_transmit;
#else
    workq_fn funcs[] = {pktgen_main_transmit};
    (void)fast_main_transmit;
#endif

    for (uint16_t i = 0; i < RTE_DIM(funcs); i++) {
        if (workq_add(WORKQ_TX, pid, funcs[i]))
            return -1;
    }
    return 0;
}

static int
pktgen_workq_setup_once(workq_type_t wqt, uint16_t pid, void *arg)
{
    uint16_t wqt_idx = (wqt == WORKQ_RX) ? 0 : 1;

    /* Check if this workqueue type for this port is already set up */
    if (__atomic_compare_exchange_n(&workq_setup_done[pid][wqt_idx],
                                    &(uint16_t){0}, 1,
                                    false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        /* This core won the race - perform the actual setup */
        AK_DEBUG_LOG_LINE(DEBUG, "Core %d: Setting up %s workqueue for port %d",
                          rte_lcore_id(), (wqt == WORKQ_RX) ? "RX" : "TX", pid);

        if (workq_port_arg_set(pid, arg)) {
            __atomic_store_n(&workq_setup_done[pid][wqt_idx], 0, __ATOMIC_RELEASE);
            return -1;
        }

        int ret = (wqt == WORKQ_RX) ? pktgen_rx_workq_setup(pid) : pktgen_tx_workq_setup(pid);
        if (ret != 0) {
            /* Reset on failure */
            __atomic_store_n(&workq_setup_done[pid][wqt_idx], 0, __ATOMIC_RELEASE);
            return ret;
        }

        AK_DEBUG_LOG_LINE(DEBUG, "Core %d: Successfully set up %s workqueue for port %d",
                          rte_lcore_id(), (wqt == WORKQ_RX) ? "RX" : "TX", pid);
    } else {
        /* Another core is setting it up - wait for completion */
        AK_DEBUG_LOG_LINE(DEBUG, "Core %d: Waiting for %s workqueue setup for port %d",
                          rte_lcore_id(), (wqt == WORKQ_RX) ? "RX" : "TX", pid);

        while (__atomic_load_n(&workq_setup_done[pid][wqt_idx], __ATOMIC_ACQUIRE) != 1) {
            rte_pause();
        }

        AK_DEBUG_LOG_LINE(DEBUG, "Core %d: %s workqueue for port %d is ready",
                          rte_lcore_id(), (wqt == WORKQ_RX) ? "RX" : "TX", pid);
    }

    return 0;
}

static int
pktgen_workq_setup(workq_type_t wqt, uint16_t pid, void *arg)
{
    return pktgen_workq_setup_once(wqt, pid, arg);
}

/**
 *
 * pktgen_main_rxtx_loop - Single thread loop for tx/rx packets
 *
 * DESCRIPTION
 * Handle sending and receiving packets from a given set of ports. This is the
 * main loop or thread started on a single core.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
static void
pktgen_main_rxtx_loop(void)
{
    port_info_t *pinfo;
    uint64_t curr_tsc, tx_next_cycle, tx_bond_cycle;
    uint16_t pid, rx_qid, tx_qid, lid = rte_lcore_id();

    if (lid == rte_get_main_lcore()) {
        printf("Using %d initial lcore for Rx/Tx\n", lid);
        rte_exit(0, "using initial lcore for port");
    }

    pinfo = l2p_get_pinfo_by_lcore(lid);

    curr_tsc      = pktgen_get_time();
    tx_next_cycle = curr_tsc;
    tx_bond_cycle = curr_tsc + (pktgen_get_timer_hz() / 10);

    pid    = pinfo->pid;
    rx_qid = l2p_get_rxqid(lid);
    tx_qid = l2p_get_txqid(lid);

    printf("RX/TX lid %3d, pid %2d, qids %2d/%2d Mempool %-16s @ %p\n", lid, pinfo->pid, rx_qid,
           tx_qid, l2p_get_tx_mp(pinfo->pid)->name, l2p_get_tx_mp(pinfo->pid));

    if (pktgen_workq_setup(WORKQ_RX, pid, pinfo))
        rte_exit(EXIT_FAILURE, "Error setting up Rx work queue for pid %u\n", pid);
    if (pktgen_workq_setup(WORKQ_TX, pid, pinfo))
        rte_exit(EXIT_FAILURE, "Error setting up Tx work queue for pid %u\n", pid);

    while (pktgen.force_quit == 0) {
        /* Process RX workqueue list */
        workq_run(WORKQ_RX, pid, rx_qid);

        curr_tsc = pktgen_get_time();

        /* Determine when is the next time to send packets */
        if (curr_tsc >= tx_next_cycle) {
            tx_next_cycle = curr_tsc + pinfo->tx_cycles;

            // Process TX workqueue list
            workq_run(WORKQ_TX, pid, tx_qid);
        }
        if (curr_tsc >= tx_bond_cycle) {
            tx_bond_cycle = curr_tsc + (pktgen_get_timer_hz() / 10);
            if (pktgen_tst_port_flags(pinfo, BONDING_TX_PACKETS))
                rte_eth_tx_burst(pid, tx_qid, NULL, 0);
        }
    }

    pktgen_log_debug("Exit %d", lid);

    pktgen_exit_cleanup(lid);
}

/**
 *
 * pktgen_main_tx_loop - Main transmit loop for a core, no receive packet handling
 *
 * DESCRIPTION
 * When Tx and Rx are split across two cores this routing handles the tx packets.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
static void
pktgen_main_tx_loop(void)
{
    uint16_t tx_qid, lid = rte_lcore_id();
    port_info_t *pinfo = l2p_get_pinfo_by_lcore(lid);
    uint64_t curr_tsc, tx_next_cycle, tx_bond_cycle;
    uint16_t pid = pinfo->pid;

    if (lid == rte_get_main_lcore()) {
        printf("Using %d initial lcore for Rx/Tx\n", lid);
        rte_exit(0, "Invalid initial lcore assigned to a port");
    }

    curr_tsc      = pktgen_get_time();
    tx_next_cycle = curr_tsc + pinfo->tx_cycles;
    tx_bond_cycle = curr_tsc + pktgen_get_timer_hz() / 10;

    tx_qid = l2p_get_txqid(lid);

    printf("TX lid %3d, pid %2d, qid %2d, Mempool %-16s @ %p\n", lid, pinfo->pid, tx_qid,
           l2p_get_tx_mp(pinfo->pid)->name, l2p_get_tx_mp(pinfo->pid));

    if (pktgen_workq_setup(WORKQ_TX, pid, pinfo))
        rte_exit(EXIT_FAILURE, "Error setting up Tx work queue for pid %u\n", pid);

    while (unlikely(pktgen.force_quit == 0)) {
        curr_tsc = pktgen_get_time();

        /* Determine when is the next time to send packets */
        if (unlikely(curr_tsc >= tx_next_cycle)) {
            tx_next_cycle = curr_tsc + pinfo->tx_cycles;

            // Process TX workqueue list
            workq_run(WORKQ_TX, pid, tx_qid);
        }
        if (unlikely(curr_tsc >= tx_bond_cycle)) {
            tx_bond_cycle = curr_tsc + pktgen_get_timer_hz() / 10;
            if (pktgen_tst_port_flags(pinfo, BONDING_TX_PACKETS))
                rte_eth_tx_burst(pinfo->pid, tx_qid, NULL, 0);
        }
    }

    pktgen_log_debug("Exit %d", lid);

    pktgen_exit_cleanup(lid);
}

/**
 *
 * pktgen_main_rx_loop - Handle only the rx packets for a set of ports.
 *
 * DESCRIPTION
 * When Tx and Rx processing is split between two ports this routine handles
 * only the receive packets.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
static void
pktgen_main_rx_loop(void)
{
    port_info_t *pinfo;
    uint16_t lid = rte_lcore_id(), rx_qid = l2p_get_rxqid(lid), pid;

    if (lid == rte_get_main_lcore()) {
        printf("Using %d initial lcore for Rx/Tx\n", lid);
        rte_exit(0, "using initial lcore for ports");
    }

    pinfo  = l2p_get_pinfo_by_lcore(lid);
    rx_qid = l2p_get_rxqid(lid);
    AK_DEBUG_LOG_LINE(DEBUG, "RX Core Info: lid %3d, rx_qid %2d, cpu_id %2d, pinfo %p", lid, rx_qid, sched_getcpu(), pinfo);
    printf("RX lid %3d, pid %2d, qid %2d, Mempool %-16s @ %p\n", lid, pinfo->pid, rx_qid,
           l2p_get_rx_mp(pinfo->pid)->name, l2p_get_rx_mp(pinfo->pid));

    pid = pinfo->pid;

    if (pktgen_workq_setup(WORKQ_RX, pid, pinfo))
        rte_exit(EXIT_FAILURE, "Error setting up Rx work queue for pid %u\n", pid);

    while (pktgen.force_quit == 0) {
        workq_run(WORKQ_RX, pid, rx_qid);
    }
    workq_port_destroy(pid);

    pktgen_log_debug("Exit %d", lid);

    pktgen_exit_cleanup(lid);
}

/**
 *
 * pktgen_launch_one_lcore - Launch a single logical core thread.
 *
 * DESCRIPTION
 * Help launching a single thread on one logical core.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */
int
pktgen_launch_one_lcore(void *arg __rte_unused)
{
    uint16_t pid, lid = rte_lcore_id();

    if ((pid = l2p_get_pid_by_lcore(lid)) >= RTE_MAX_ETHPORTS) {
        pktgen_log_info("*** Logical core %3d has no work, skipping launch", lid);
        return 0;
    }

    switch (l2p_get_type(lid)) {
    case LCORE_MODE_RX:
        pktgen_main_rx_loop();
        break;
    case LCORE_MODE_TX:
        pktgen_main_tx_loop();
        break;
    case LCORE_MODE_BOTH:
        pktgen_main_rxtx_loop();
        break;
    default:
        rte_exit(EXIT_FAILURE, "Invalid logical core mode %d\n", l2p_get_type(lid));
    }
    return 0;
}

static void
_page_display(void)
{
    static unsigned int counter = 0;

    pktgen_display_set_color("top.spinner");
    scrn_printf(1, 1, "%c", "-\\|/"[(counter++ & 3)]);
    pktgen_display_set_color(NULL);

    if ((pktgen.flags & PAGE_MASK_BITS) == 0)
        pktgen.flags |= MAIN_PAGE_FLAG;

    if (pktgen.flags & MAIN_PAGE_FLAG)
        pktgen_page_stats();
    else if (pktgen.flags & SYSTEM_PAGE_FLAG)
        pktgen_page_system();
    else if (pktgen.flags & RANGE_PAGE_FLAG)
        pktgen_page_range();
    else if (pktgen.flags & CPU_PAGE_FLAG)
        pktgen_page_cpu();
    else if (pktgen.flags & SEQUENCE_PAGE_FLAG)
        pktgen_page_seq(pktgen.curr_port);
    else if (pktgen.flags & RND_BITFIELD_PAGE_FLAG) {
        port_info_t *pinfo = l2p_get_port_pinfo(pktgen.curr_port);
        pktgen_page_random_bitfields(pktgen.flags & PRINT_LABELS_FLAG, pktgen.curr_port,
                                     pinfo->rnd_bitfields);
    } else if (pktgen.flags & LOG_PAGE_FLAG)
        pktgen_page_log(pktgen.flags & PRINT_LABELS_FLAG);
    else if (pktgen.flags & LATENCY_PAGE_FLAG)
        pktgen_page_latency();
    else if (pktgen.flags & STATS_PAGE_FLAG)
        pktgen_page_queue_stats(pktgen.curr_port);
    else if (pktgen.flags & XSTATS_PAGE_FLAG)
        pktgen_page_xstats(pktgen.curr_port);
    else
        pktgen_page_stats();
}

/**
 *
 * pktgen_page_display - Display the correct page based on timer callback.
 *
 * DESCRIPTION
 * When timer is active update or display the correct page of data.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

void
pktgen_page_display(void)
{
    static unsigned int update_display = 1;

    /* Leave if the screen is paused */
    if (scrn_is_paused())
        return;

    scrn_save();

    if (pktgen.flags & UPDATE_DISPLAY_FLAG) {
        pktgen.flags &= ~UPDATE_DISPLAY_FLAG;
        update_display = 1;
    }

    update_display--;
    if (update_display == 0) {
        update_display = UPDATE_DISPLAY_TICK_INTERVAL;
        _page_display();

        if (pktgen.flags & PRINT_LABELS_FLAG)
            pktgen.flags &= ~PRINT_LABELS_FLAG;
    }

    scrn_restore();

    pktgen_print_packet_dump();
}

static void *
_timer_thread(void *arg)
{
    uint64_t process, page, prev;

    this_scrn = arg;

    pktgen.stats_timeout = pktgen.hz;
    pktgen.page_timeout  = UPDATE_DISPLAY_TICK_RATE;

    page = prev = pktgen_get_time();
    process     = page + pktgen.stats_timeout;
    page += pktgen.page_timeout;

    pktgen.timer_running = 1;

    while (pktgen.timer_running) {
        uint64_t curr;

        curr = pktgen_get_time();

        if (curr >= process) {
            process = curr + pktgen.stats_timeout;
            pktgen_process_stats();
            prev = curr;

            /* Sample PCIe metrics (rate-limited to 1 Hz internally) */
            extern void pcie_log_sample(void);
            pcie_log_sample();
        }

        if (curr >= page) {
            page = curr + pktgen.page_timeout;
            pktgen_page_display();
        }

        rte_pause();
    }
    return NULL;
}

/**
 *
 * pktgen_timer_setup - Set up the timer callback routines.
 *
 * DESCRIPTION
 * Setup the two timers to be used for display and calculating statistics.
 *
 * RETURNS: N/A
 *
 * SEE ALSO:
 */

void
pktgen_timer_setup(void)
{
    rte_cpuset_t cpuset_data;
    rte_cpuset_t *cpuset = &cpuset_data;
    pthread_t tid;

    CPU_ZERO(cpuset);

    pthread_create(&tid, NULL, _timer_thread, this_scrn);

    CPU_SET(rte_get_main_lcore(), cpuset);
    pthread_setaffinity_np(tid, sizeof(cpuset), cpuset);
}

/**
 * print_pktgen_stats_summary - Print per-lcore packet statistics summary
 *
 * DESCRIPTION
 * Print a summary table of RX/TX packet statistics for each lcore,
 * similar to L3FWD's statistics output format.
 *
 * RETURNS: N/A
 */
void
print_pktgen_stats_summary(void)
{
    unsigned int lcore_id;
    uint64_t total_rx = 0, total_tx = 0;
    double total_rx_rate = 0.0, total_tx_rate = 0.0;
    uint64_t current_time = rte_rdtsc();
    uint64_t tsc_hz = rte_get_tsc_hz();

    /* Only print statistics if enabled */
    if (!stats_enabled) {
        return;
    }

    printf("\n");
    printf("=====================================\n");
    printf("PKTGEN Packet Statistics Summary\n");
    printf("=====================================\n");
    printf("%-8s %-12s %-12s %-10s %-10s %-8s\n",
        "Lcore", "RX Packets", "TX Packets", "RX Mpps", "TX Mpps", "Diff%");
    printf("%-8s %-12s %-12s %-10s %-10s %-8s\n",
        "-----", "----------", "----------", "--------", "--------", "------");

    RTE_LCORE_FOREACH(lcore_id) {
        if (lcore_stats[lcore_id].start_time > 0) {
            uint64_t duration = current_time - lcore_stats[lcore_id].start_time;
            double elapsed_sec = (double)duration / tsc_hz;
            double rx_rate = elapsed_sec > 0 ? lcore_stats[lcore_id].filtered_rx_packets / elapsed_sec : 0;
            double tx_rate = elapsed_sec > 0 ? lcore_stats[lcore_id].filtered_tx_packets / elapsed_sec : 0;

            /* Calculate proper rate for individual lcore using filtered counts */
            double rate = 0.0;
            uint64_t filtered_rx = lcore_stats[lcore_id].filtered_rx_packets;
            uint64_t filtered_tx = lcore_stats[lcore_id].filtered_tx_packets;

            if (filtered_rx > 0) {
                if (filtered_tx > filtered_rx) {
                    /* Amplification case: more filtered TX than filtered RX */
                    rate = (double)(filtered_tx - filtered_rx) * 100.0 / filtered_rx;
                } else {
                    /* Loss case: less filtered TX than filtered RX */
                    rate = (double)(filtered_rx - filtered_tx) * 100.0 / filtered_rx;
                }
            } else if (filtered_tx > 0) {
                /* Only filtered TX, no filtered RX - show as 0% since no reference */
                rate = 0.0;
            }

            printf("%-8u %-12" PRIu64 " %-12" PRIu64 " %-10.1f %-10.1f %-8.1f\n",
                lcore_id,
                lcore_stats[lcore_id].filtered_rx_packets,
                lcore_stats[lcore_id].filtered_tx_packets,
                rx_rate / 1000000.0,  /* Convert to Mpps */
                tx_rate / 1000000.0,
                rate);

            total_rx += lcore_stats[lcore_id].filtered_rx_packets;
            total_tx += lcore_stats[lcore_id].filtered_tx_packets;
            total_rx_rate += rx_rate / 1000000.0;
            total_tx_rate += tx_rate / 1000000.0;
        }
    }

    /* Calculate total excluded packets (unwanted traffic like DHCP) */
    uint64_t total_raw_rx = 0, total_raw_tx = 0, total_excluded_rx = 0, total_excluded_tx = 0;
    RTE_LCORE_FOREACH(lcore_id) {
        if (lcore_stats[lcore_id].start_time > 0) {
            total_raw_rx += lcore_stats[lcore_id].rx_packets;
            total_raw_tx += lcore_stats[lcore_id].tx_packets;
        }
    }
    total_excluded_rx = total_raw_rx - total_rx;
    total_excluded_tx = total_raw_tx - total_tx;

    /* Calculate loss/amplification rate properly */
    int64_t difference = (int64_t)total_tx - (int64_t)total_rx;  /* TX - RX */
    double rate = 0.0;
    const char *rate_type = "";

    if (total_rx > 0) {
        if (difference > 0) {
            /* More TX than RX - amplification */
            rate = (double)difference * 100.0 / total_rx;
            rate_type = "amplification";
        } else if (difference < 0) {
            /* More RX than TX - loss */
            rate = (double)(-difference) * 100.0 / total_rx;
            rate_type = "loss";
        } else {
            /* Perfect match */
            rate = 0.0;
            rate_type = "perfect";
        }
    }

    printf("%-8s %-12s %-12s %-10s %-10s %-8s\n",
        "-----", "----------", "----------", "--------", "--------", "------");
    printf("%-8s %-12" PRIu64 " %-12" PRIu64 " %-10.1f %-10.1f %-8.1f\n",
        "Total", total_rx, total_tx, total_rx_rate, total_tx_rate, rate);
    printf("=====================================\n");
    printf("ANALYSIS: RX/TX difference = %+" PRId64 " packets (%.1f%% %s)\n",
        difference, rate, rate_type);

    printf("FILTERING: %" PRIu64 " RX + %" PRIu64 " TX unwanted packets excluded from counting\n",
        total_excluded_rx, total_excluded_tx);
    // if (total_raw_rx > 0 && total_raw_tx > 0) {
    //     printf("           (%.1f%% of total RX, %.1f%% of total TX)\n",
    //         (double)total_excluded_rx * 100.0 / total_raw_rx,
    //         (double)total_excluded_tx * 100.0 / total_raw_tx);
    // }
    printf("=====================================\n");

    /* Print NIC hardware statistics */
    print_all_nic_hw_stats();

    /* Print RX drop analysis */
    print_rx_drop_analysis();
}

/**
 * print_nic_hw_stats - Print NIC hardware statistics for a specific port
 *
 * DESCRIPTION
 * Print detailed NIC hardware statistics including drops, errors, and buffer states
 *
 * RETURNS: N/A
 */
void
print_nic_hw_stats(uint16_t port_id)
{
    struct rte_eth_stats eth_stats;
    struct rte_eth_xstat *xstats = NULL;
    struct rte_eth_xstat_name *xstat_names = NULL;
    int cnt_xstats, ret, i;

    printf("\n=== NIC Hardware Statistics (Port %u) ===\n", port_id);

    /* Get basic ethernet statistics */
    ret = rte_eth_stats_get(port_id, &eth_stats);
    if (ret == 0) {
        printf("Hardware RX Packets:     %"PRIu64"\n", eth_stats.ipackets);
        printf("Hardware TX Packets:     %"PRIu64"\n", eth_stats.opackets);
        printf("Hardware RX Bytes:       %"PRIu64"\n", eth_stats.ibytes);
        printf("Hardware TX Bytes:       %"PRIu64"\n", eth_stats.obytes);
        printf("Hardware RX Errors:      %"PRIu64"\n", eth_stats.ierrors);
        printf("Hardware TX Errors:      %"PRIu64"\n", eth_stats.oerrors);
        printf("Hardware RX Missed:      %"PRIu64" (packets dropped by HW)\n", eth_stats.imissed);
        printf("Hardware RX No MBuf:     %"PRIu64" (mbuf allocation failed)\n", eth_stats.rx_nombuf);

        /* Print per-queue statistics if available */
        bool has_queue_stats = false;
        for (i = 0; i < RTE_ETHDEV_QUEUE_STAT_CNTRS && i < 8; i++) {
            if (eth_stats.q_ipackets[i] > 0 || eth_stats.q_opackets[i] > 0 || eth_stats.q_errors[i] > 0) {
                if (!has_queue_stats) {
                    printf("\nPer-Queue Statistics:\n");
                    has_queue_stats = true;
                }
                printf("  Queue %d: RX=%"PRIu64", TX=%"PRIu64", Errors=%"PRIu64"\n",
                    i, eth_stats.q_ipackets[i], eth_stats.q_opackets[i], eth_stats.q_errors[i]);
            }
        }

        /* Calculate packet loss if any */
        if (eth_stats.ipackets > 0) {
            uint64_t total_drops = eth_stats.imissed + eth_stats.rx_nombuf + eth_stats.ierrors;
            if (total_drops > 0) {
                double drop_rate = (double)total_drops * 100.0 / (eth_stats.ipackets + total_drops);
                printf("\nPacket Loss Analysis:\n");
                printf("  Total Drops:           %"PRIu64"\n", total_drops);
                printf("  Drop Rate:             %.2f%%\n", drop_rate);
                printf("  Primary Drop Cause:    ");
                if (eth_stats.imissed > eth_stats.rx_nombuf && eth_stats.imissed > eth_stats.ierrors) {
                    printf("HW Ring Full (imissed)\n");
                } else if (eth_stats.rx_nombuf > eth_stats.ierrors) {
                    printf("No MBuf Available\n");
                } else if (eth_stats.ierrors > 0) {
                    printf("HW Errors\n");
                } else {
                    printf("Unknown\n");
                }
            }
        }
    } else {
        printf("Failed to get basic statistics for port %u\n", port_id);
    }

    /* Get extended statistics for detailed drop analysis */
    cnt_xstats = rte_eth_xstats_get_names(port_id, NULL, 0);
    if (cnt_xstats > 0) {
        xstat_names = malloc(sizeof(struct rte_eth_xstat_name) * cnt_xstats);
        xstats = malloc(sizeof(struct rte_eth_xstat) * cnt_xstats);

        if (xstat_names && xstats) {
            ret = rte_eth_xstats_get_names(port_id, xstat_names, cnt_xstats);
            if (ret == cnt_xstats) {
                ret = rte_eth_xstats_get(port_id, xstats, cnt_xstats);
                if (ret == cnt_xstats) {
                    printf("\nDetailed Drop/Error Statistics:\n");
                    bool found_drops = false;
                    for (i = 0; i < cnt_xstats; i++) {
                        const char *name = xstat_names[i].name;
                        uint64_t value = xstats[i].value;

                        /* Filter for drop/error related statistics */
                        if (value > 0 && (strstr(name, "drop") || strstr(name, "discard") ||
                                         strstr(name, "error") || strstr(name, "miss") ||
                                         strstr(name, "full") || strstr(name, "overflow") ||
                                         strstr(name, "underrun") || strstr(name, "crc") ||
                                         strstr(name, "fragment") || strstr(name, "jabber"))) {
                            printf("  %-30s: %"PRIu64"\n", name, value);
                            found_drops = true;
                        }
                    }
                    if (!found_drops) {
                        printf("  No drop/error statistics found\n");
                    }
                }
            }
        }

        free(xstat_names);
        free(xstats);
    }
    printf("==========================================\n");
}

/**
 * print_all_nic_hw_stats - Print NIC hardware statistics for all active ports
 *
 * DESCRIPTION
 * Print NIC hardware statistics for all ports currently in use by pktgen
 *
 * RETURNS: N/A
 */
void
print_all_nic_hw_stats(void)
{
    uint16_t port_id;
    port_info_t *pinfo;

    printf("\n");
    printf("########################################\n");
    printf("# NIC HARDWARE STATISTICS ANALYSIS\n");
    printf("########################################\n");

    RTE_ETH_FOREACH_DEV(port_id) {
        pinfo = l2p_get_port_pinfo(port_id);
        if (pinfo && pinfo->seq_pkt) {
            print_nic_hw_stats(port_id);
        }
    }

    printf("########################################\n");
}

/**
 * init_rx_drop_analysis - Initialize RX drop analysis for a core
 */
void
init_rx_drop_analysis(unsigned int lcore_id)
{
    rx_drop_analysis_t *analysis = &rx_analysis[lcore_id];

    memset(analysis, 0, sizeof(rx_drop_analysis_t));
    analysis->analysis_interval = rte_get_tsc_hz(); /* 1 second interval */
    analysis->last_check_time = rte_rdtsc();

    AK_DEBUG_LOG_LINE(DEBUG, "RX Drop Analysis initialized for lcore %u", lcore_id);
}

/**
 * analyze_rx_performance - Analyze RX performance and detect issues
 */
void
analyze_rx_performance(unsigned int lcore_id, uint16_t port_id, uint16_t nb_rx, uint16_t expected_rx)
{
    rx_drop_analysis_t *analysis = &rx_analysis[lcore_id];
    uint64_t current_time = rte_rdtsc();

    /* Update counters */
    analysis->total_rx_attempts++;

    if (nb_rx == 0) {
        analysis->zero_rx_count++;
        /* Log frequent zero RX events */
#ifdef RTE_LIBRTE_ETHDEV_DEBUG
        // if (analysis->zero_rx_count % 10000 == 0) {
        //     printf("DEBUG: Lcore %u - %lu consecutive zero RX events\n",
        //            lcore_id, analysis->zero_rx_count);
        // }
#endif
    } else if (nb_rx < expected_rx / 2) {
        analysis->small_rx_count++;
    } else if (nb_rx == expected_rx) {
        analysis->full_rx_count++;
    }

    /* Periodic detailed analysis */
    if (current_time - analysis->last_check_time > analysis->analysis_interval) {
        struct rte_eth_stats eth_stats;

        if (rte_eth_stats_get(port_id, &eth_stats) == 0) {
            /* Use port-level HW drops but only show on first RX core to avoid duplication */
            static uint64_t initial_port_hw_drops = 0;
            static unsigned int first_rx_core = RTE_MAX_LCORE;
            static bool first_measurement = true;

            /* Determine the first RX core for this port */
            if (first_rx_core == RTE_MAX_LCORE) {
                first_rx_core = lcore_id;
            }

            if (lcore_id == first_rx_core) {
                /* Only the first RX core tracks port-level HW drops */
                uint64_t hw_drops_current = eth_stats.imissed + eth_stats.rx_nombuf + eth_stats.ierrors;

                if (first_measurement) {
                    initial_port_hw_drops = hw_drops_current;
                    analysis->hw_drops_delta = 0;
                    first_measurement = false;
                } else {
                    /* Show total drops since test started */
                    analysis->hw_drops_delta = hw_drops_current - initial_port_hw_drops;
                }
            } else {
                /* Other cores show 0 to avoid duplication */
                analysis->hw_drops_delta = 0;
            }

#ifdef RTE_LIBRTE_ETHDEV_DEBUG
            /* Log significant drop events */
            if (analysis->hw_drops_delta > 1000) {
                AK_DEBUG_LOG_LINE(DEBUG, "Lcore %u Port %u - %lu total HW drops since start!",
                                 lcore_id, port_id, analysis->hw_drops_delta);
                AK_DEBUG_LOG_LINE(DEBUG, "  Zero RX: %lu, Small RX: %lu, Full RX: %lu",
                                 analysis->zero_rx_count, analysis->small_rx_count, analysis->full_rx_count);
                AK_DEBUG_LOG_LINE(DEBUG, "  RX Missed: %lu, No MBuf: %lu, Errors: %lu",
                                 eth_stats.imissed, eth_stats.rx_nombuf, eth_stats.ierrors);
            }
#endif
        }

        analysis->last_check_time = current_time;
        /* Reset counters for next interval */
        analysis->zero_rx_count = 0;
        analysis->small_rx_count = 0;
        analysis->full_rx_count = 0;
    }
}

/**
 * print_rx_drop_analysis - Print comprehensive RX drop analysis
 */
void
print_rx_drop_analysis(void)
{
    unsigned int lcore_id;

    printf("\n");
    printf("########################################\n");
    printf("# RX DROP ANALYSIS REPORT\n");
    printf("########################################\n");

    printf("%-8s %-12s %-12s %-12s %-12s\n",
           "Lcore", "RX Attempts", "Zero RX", "Small RX", "Full RX");
    printf("%-8s %-12s %-12s %-12s %-12s\n",
           "-----", "-----------", "--------", "---------", "--------");

    /* Get current HW drops for accurate final measurement */
    struct rte_eth_stats eth_stats;

    if (rte_eth_stats_get(0, &eth_stats) == 0) {
        /* HW stats available but not displayed in this table */
    }

    RTE_LCORE_FOREACH(lcore_id) {
        rx_drop_analysis_t *analysis = &rx_analysis[lcore_id];

        if (analysis->total_rx_attempts > 0) {
            printf("%-8u %-12lu %-12lu %-12lu %-12lu\n",
                   lcore_id,
                   analysis->total_rx_attempts,
                   analysis->zero_rx_count,
                   analysis->small_rx_count,
                   analysis->full_rx_count);
        }
    }

    printf("########################################\n");
}
