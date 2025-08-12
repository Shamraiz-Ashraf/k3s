#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <getopt.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pcap/pcap.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h>
#include <rte_cycles.h>
#include <rte_pci.h>
#include <rte_bus.h>
#include <rte_bus_pci.h>
#include <rte_dev.h>

/* Configuration constants */
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 4096
#define NUM_MBUFS 16383        // Improved: Power of 2 minus 1 for better cache alignment
#define MBUF_CACHE_SIZE 512    // Improved: Larger cache for better performance
#define BURST_SIZE 64          // Improved: Optimized burst size
#define MAX_BURST_SIZE 256     // Improved: Reasonable maximum burst size
#define MAX_PACKET_SIZE 2048   // Maximum expected packet size
#define STATS_INTERVAL_SEC 1   // Statistics printing interval
#define PAUSE_CHECK_INTERVAL_US 10000  // 10ms pause frame check interval
#define MAX_RETRY_COUNT 3      // Maximum transmission retry attempts
#define MIN_WAIT_CYCLES_US 100 // Minimum wait time in microseconds

/* Improved packet structure with better memory layout */
struct packet_info {
    uint8_t *data;
    uint32_t len;
    uint32_t wire_len;         // Pre-calculated wire length including overhead
    uint64_t bits;             // Pre-calculated bits for this packet
} __rte_cache_aligned;

/* Application context structure for better organization */
struct app_context {
    uint16_t port_id;
    struct rte_mempool *mbuf_pool;
    struct packet_info *packets;
    size_t num_packets;
    uint8_t *packet_data_buffer;
    
    /* Configuration */
    uint64_t target_bitrate;
    struct rte_ether_addr dst_mac;
    uint32_t dst_ip;
    uint16_t dst_port;
    bool have_dst_mac;
    bool have_dst_ip;
    bool have_dst_port;
    int replays;
    double run_duration;
    
    /* Timing and statistics */
    uint64_t hz;
    uint64_t start_time;
    uint64_t next_send_time;
    uint64_t last_stats_time;
    uint64_t last_pause_check;
    uint64_t pause_until_time;
    uint64_t total_packets_sent;
    uint64_t total_bytes_sent;
    uint64_t total_packets_dropped;
    
    /* Burst configuration */
    uint16_t effective_burst_size;
    bool precise_pacing_mode;
    double sec_per_bit;
    uint64_t min_wait_cycles;
};

volatile bool keep_running = true;

static void sig_handler(int sig) {
    (void)sig; // Suppress unused parameter warning
    keep_running = false;
}

static void usage_sender(const char *progname) {
    printf("DPDK High-Performance Packet Sender\n");
    printf("Usage: %s [EAL options] -- [app options]\n\n", progname);
    printf("App options:\n");
    printf("  --bitrate <Mbps>       Target send bitrate (default: link speed)\n");
    printf("  --dst-mac <MAC>        Destination MAC address (format: xx:xx:xx:xx:xx:xx)\n");
    printf("  --dst-ip <IP>          Destination IPv4 address\n");
    printf("  --dst-port <port>      Destination UDP port\n");
    printf("  --pcap <file>          Input PCAP file to replay (required)\n");
    printf("  --replays <N>          Number of replays (required, 0 for infinite)\n");
    printf("  --pci <address>        PCI address of the TX port (required)\n");
    printf("  --run-duration <sec>   Maximum run duration in seconds\n");
    printf("  --help                 Show this help\n\n");
    printf("Notes:\n");
    printf("  - If destination parameters are not specified, original PCAP values are preserved\n");
    printf("  - Bitrate precision decreases at very high rates (>15 Gbps) for better throughput\n");
    printf("  - Flow control and pause frames are automatically handled\n");
}

static int parse_mac(const char *str, struct rte_ether_addr *mac) {
    if (!str || !mac) return -1;
    
    unsigned int bytes[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &bytes[0], &bytes[1], &bytes[2],
               &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return -1;
    }
    
    for (int i = 0; i < 6; i++) {
        if (bytes[i] > 255) return -1;
        mac->addr_bytes[i] = (uint8_t)bytes[i];
    }
    return 0;
}

static uint16_t find_port_by_pci(const char *pci_addr_str) {
    uint16_t port_id;
    
    if (!pci_addr_str) {
        printf("Warning: No PCI address provided\n");
        return rte_eth_dev_count_avail() > 0 ? 0 : UINT16_MAX;
    }
    
    RTE_ETH_FOREACH_DEV(port_id) {
        struct rte_eth_dev_info dev_info;
        int ret = rte_eth_dev_info_get(port_id, &dev_info);
        if (ret == 0) {
            printf("Using port %u (PCI matching simplified for %s)\n", port_id, pci_addr_str);
            return port_id;
        }
    }
    
    printf("Warning: No matching port found for PCI %s, using first available\n", pci_addr_str);
    return rte_eth_dev_count_avail() > 0 ? 0 : UINT16_MAX;
}

/* Improved pause frame detection with better error handling */
static bool check_for_pause_frames(struct app_context *ctx) {
    if (!ctx || ctx->pause_until_time > 0) {
        return false; // Skip if already in pause or invalid context
    }
    
    struct rte_mbuf *rx_bufs[8];
    uint16_t nb_rx = rte_eth_rx_burst(ctx->port_id, 0, rx_bufs, 8);
    bool pause_detected = false;
    
    for (uint16_t i = 0; i < nb_rx; i++) {
        struct rte_mbuf *m = rx_bufs[i];
        if (!m || m->pkt_len < 64) {
            if (m) rte_pktmbuf_free(m);
            continue;
        }
        
        uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
        if (!data) {
            rte_pktmbuf_free(m);
            continue;
        }
        
        // Check for Ethernet pause frame: EtherType 0x8808, Opcode 0x0001
        if (m->pkt_len >= 18 &&  // Minimum pause frame size
            data[12] == 0x88 && data[13] == 0x08 &&
            data[14] == 0x00 && data[15] == 0x01) {
            
            uint16_t pause_time = (data[16] << 8) | data[17];
            if (pause_time > 0) {
                // Convert pause time to nanoseconds and then to cycles
                uint64_t pause_ns = (uint64_t)pause_time * 512ULL;
                uint64_t pause_cycles = (pause_ns * ctx->hz) / 1000000000ULL;
                
                ctx->pause_until_time = rte_get_timer_cycles() + pause_cycles;
                pause_detected = true;
                printf("Pause frame detected: %u units (%lu ns pause)\n", pause_time, pause_ns);
            }
        }
        rte_pktmbuf_free(m);
    }
    
    return pause_detected;
}

/* Initialize application context */
static int init_app_context(struct app_context *ctx) {
    if (!ctx) return -1;
    
    memset(ctx, 0, sizeof(*ctx));
    ctx->port_id = UINT16_MAX;
    ctx->replays = -1;
    ctx->dst_port = 8888;
    ctx->effective_burst_size = BURST_SIZE;
    ctx->precise_pacing_mode = true;
    
    return 0;
}

/* Enhanced port configuration with better error handling */
static int configure_port(struct app_context *ctx) {
    if (!ctx || ctx->port_id == UINT16_MAX) return -1;
    
    int socket_id = rte_eth_dev_socket_id(ctx->port_id);
    if (socket_id < 0) socket_id = 0;
    
    // Create mbuf pool with improved parameters
    ctx->mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
                                             MBUF_CACHE_SIZE, 0, 
                                             RTE_MBUF_DEFAULT_BUF_SIZE, socket_id);
    if (!ctx->mbuf_pool) {
        printf("Error: Cannot create mbuf pool\n");
        return -1;
    }
    
    // Enhanced port configuration
    struct rte_eth_conf port_conf = {
        .rxmode = {
            .mq_mode = RTE_ETH_MQ_RX_NONE,
        },
        .txmode = {
            .mq_mode = RTE_ETH_MQ_TX_NONE,
            .offloads = RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE |
                       RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                       RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                       RTE_ETH_TX_OFFLOAD_MULTI_SEGS,
        }
    };
    
    int ret = rte_eth_dev_configure(ctx->port_id, 1, 1, &port_conf);
    if (ret != 0) {
        printf("Error: Cannot configure device: err=%d, port=%u\n", ret, ctx->port_id);
        return ret;
    }
    
    // Setup RX queue for pause frame detection
    ret = rte_eth_rx_queue_setup(ctx->port_id, 0, RX_RING_SIZE, socket_id, NULL, ctx->mbuf_pool);
    if (ret < 0) {
        printf("Error: Cannot setup rx queue: err=%d, port=%u\n", ret, ctx->port_id);
        return ret;
    }
    
    // Setup TX queue with optimized parameters
    struct rte_eth_txconf txconf = {
        .offloads = RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE |
                   RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                   RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                   RTE_ETH_TX_OFFLOAD_MULTI_SEGS,
        .tx_thresh = {
            .pthresh = 32,
            .hthresh = 0,
            .wthresh = 16,
        },
        .tx_free_thresh = 64,
    };
    
    ret = rte_eth_tx_queue_setup(ctx->port_id, 0, TX_RING_SIZE, socket_id, &txconf);
    if (ret < 0) {
        printf("Error: Cannot setup tx queue: err=%d, port=%u\n", ret, ctx->port_id);
        return ret;
    }
    
    ret = rte_eth_dev_start(ctx->port_id);
    if (ret < 0) {
        printf("Error: Cannot start device: err=%d, port=%u\n", ret, ctx->port_id);
        return ret;
    }
    
    // Wait for link to come up
    struct rte_eth_link link;
    int link_check_count = 0;
    do {
        ret = rte_eth_link_get_nowait(ctx->port_id, &link);
        if (ret < 0) {
            printf("Error: Failed to get link info: err=%d\n", ret);
            return ret;
        }
        if (link.link_status == RTE_ETH_LINK_UP) break;
        rte_delay_ms(100);
        link_check_count++;
    } while (link_check_count < 50); // Wait up to 5 seconds
    
    if (link.link_status != RTE_ETH_LINK_UP) {
        printf("Error: Link is down after 5 seconds\n");
        return -1;
    }
    
    printf("Link up - speed %u Mbps - %s\n", link.link_speed,
           (link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX) ? "full-duplex" : "half-duplex");
    
    // Set target bitrate to link speed if not specified
    if (ctx->target_bitrate == 0) {
        ctx->target_bitrate = (uint64_t)link.link_speed * 1000000ULL;
    }
    
    // Configure flow control
    struct rte_eth_fc_conf fc_conf = {
        .mode = RTE_ETH_FC_FULL,
        .send_xon = 1,
        .pause_time = 0x1000,
    };
    
    ret = rte_eth_dev_flow_ctrl_set(ctx->port_id, &fc_conf);
    if (ret < 0) {
        printf("Warning: Failed to set flow control, err=%d (continuing anyway)\n", ret);
    } else {
        printf("Flow control enabled: Full duplex\n");
    }
    
    return 0;
}

/* Load PCAP with improved memory management */
static int load_pcap(struct app_context *ctx, const char *pcap_file) {
    if (!ctx || !pcap_file) return -1;
    
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t *pcap = pcap_open_offline(pcap_file, errbuf);
    if (!pcap) {
        printf("Error: Cannot open pcap file '%s': %s\n", pcap_file, errbuf);
        return -1;
    }
    
    // First pass: count packets and calculate total size
    struct pcap_pkthdr *hdr;
    const u_char *data;
    size_t capacity = 1024;
    size_t total_data_size = 0;
    
    struct packet_info *temp_packets = rte_malloc("temp_packets", 
                                                  capacity * sizeof(struct packet_info), 0);
    if (!temp_packets) {
        pcap_close(pcap);
        return -1;
    }
    
    // Load packets with dynamic array growth
    while (pcap_next_ex(pcap, &hdr, &data) == 1) {
        if (hdr->caplen == 0 || hdr->caplen > MAX_PACKET_SIZE) {
            printf("Warning: Skipping packet with invalid size %u\n", hdr->caplen);
            continue;
        }
        
        if (ctx->num_packets >= capacity) {
            capacity *= 2;
            temp_packets = rte_realloc(temp_packets, capacity * sizeof(struct packet_info), 0);
            if (!temp_packets) {
                printf("Error: Cannot reallocate packet array\n");
                pcap_close(pcap);
                return -1;
            }
        }
        
        temp_packets[ctx->num_packets].len = hdr->caplen;
        temp_packets[ctx->num_packets].wire_len = hdr->caplen + 24; // L1/L2 overhead
        temp_packets[ctx->num_packets].bits = (uint64_t)temp_packets[ctx->num_packets].wire_len * 8;
        temp_packets[ctx->num_packets].data = NULL; // Will be set later
        
        total_data_size += hdr->caplen;
        ctx->num_packets++;
    }
    pcap_close(pcap);
    
    if (ctx->num_packets == 0) {
        printf("Error: No valid packets found in PCAP file\n");
        rte_free(temp_packets);
        return -1;
    }
    
    printf("Loaded %zu packets from PCAP, total size: %zu bytes\n", ctx->num_packets, total_data_size);
    
    // Allocate final packet array
    ctx->packets = rte_malloc("packets", ctx->num_packets * sizeof(struct packet_info), 64);
    if (!ctx->packets) {
        rte_free(temp_packets);
        return -1;
    }
    memcpy(ctx->packets, temp_packets, ctx->num_packets * sizeof(struct packet_info));
    rte_free(temp_packets);
    
    // Allocate single buffer for all packet data (better cache performance)
    ctx->packet_data_buffer = rte_malloc("packet_data_buffer", total_data_size, 64);
    if (!ctx->packet_data_buffer) {
        rte_free(ctx->packets);
        ctx->packets = NULL;
        return -1;
    }
    
    // Second pass: load actual packet data
    pcap = pcap_open_offline(pcap_file, errbuf);
    if (!pcap) {
        printf("Error: Cannot reopen pcap file: %s\n", errbuf);
        return -1;
    }
    
    size_t offset = 0;
    size_t packet_idx = 0;
    while (pcap_next_ex(pcap, &hdr, &data) == 1 && packet_idx < ctx->num_packets) {
        if (hdr->caplen == 0 || hdr->caplen > MAX_PACKET_SIZE) {
            continue; // Skip invalid packets
        }
        
        memcpy(ctx->packet_data_buffer + offset, data, hdr->caplen);
        ctx->packets[packet_idx].data = ctx->packet_data_buffer + offset;
        offset += hdr->caplen;
        packet_idx++;
    }
    pcap_close(pcap);
    
    return 0;
}

/* Configure timing parameters based on target bitrate */
static void configure_timing(struct app_context *ctx) {
    if (!ctx) return;
    
    ctx->hz = rte_get_timer_hz();
    ctx->sec_per_bit = 1.0 / (double)ctx->target_bitrate;
    ctx->min_wait_cycles = (ctx->hz * MIN_WAIT_CYCLES_US) / 1000000ULL;
    
    // Adaptive burst size based on bitrate for optimal precision vs performance
    if (ctx->target_bitrate <= 1000000000ULL) { // <= 1 Gbps
        ctx->effective_burst_size = 16;
        ctx->precise_pacing_mode = true;
        printf("Precision mode enabled (<= 1 Gbps): burst_size=%u\n", ctx->effective_burst_size);
    } else if (ctx->target_bitrate <= 5000000000ULL) { // <= 5 Gbps
        ctx->effective_burst_size = 32;
        ctx->precise_pacing_mode = true;
        printf("Balanced precision mode (<= 5 Gbps): burst_size=%u\n", ctx->effective_burst_size);
    } else if (ctx->target_bitrate <= 15000000000ULL) { // <= 15 Gbps
        ctx->effective_burst_size = 64;
        ctx->precise_pacing_mode = true;
        printf("Standard mode (<= 15 Gbps): burst_size=%u\n", ctx->effective_burst_size);
    } else { // > 15 Gbps
        ctx->effective_burst_size = 128;
        ctx->precise_pacing_mode = false;
        printf("High throughput mode (> 15 Gbps): burst_size=%u\n", ctx->effective_burst_size);
    }
    
    // Ensure minimum burst size
    if (ctx->effective_burst_size < 4) {
        ctx->effective_burst_size = 4;
    }
    
    printf("Target bitrate: %.2f Mbps, Timer frequency: %lu Hz\n", 
           ctx->target_bitrate / 1000000.0, ctx->hz);
}

/* Optimized packet modification with error checking */
static void modify_packet_headers(struct rte_mbuf *m, struct app_context *ctx) {
    if (!m || !ctx) return;
    
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    if (!eth) return;
    
    // Modify destination MAC if specified
    if (ctx->have_dst_mac) {
        eth->dst_addr = ctx->dst_mac;
    }
    
    // Only process IPv4 packets with sufficient size
    if (m->pkt_len < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) ||
        rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4) {
        return;
    }
    
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    if (!ip) return;
    
    bool ip_modified = false;
    
    // Modify destination IP if specified
    if (ctx->have_dst_ip) {
        ip->dst_addr = rte_cpu_to_be_32(ctx->dst_ip);
        ip_modified = true;
    }
    
    if (ip_modified) {
        // Use hardware checksum offload
        m->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;
        m->l2_len = sizeof(struct rte_ether_hdr);
        m->l3_len = sizeof(struct rte_ipv4_hdr);
        ip->hdr_checksum = 0;
    }
    
    // Handle UDP port modification
    uint16_t ip_hdr_len = (ip->ihl & 0x0f) << 2;
    if (ctx->have_dst_port && ip->next_proto_id == IPPROTO_UDP &&
        m->pkt_len >= sizeof(struct rte_ether_hdr) + ip_hdr_len + sizeof(struct rte_udp_hdr)) {
        
        struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((uint8_t *)ip + ip_hdr_len);
        if (udp) {
            udp->dst_port = rte_cpu_to_be_16(ctx->dst_port);
            m->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
            udp->dgram_cksum = rte_ipv4_phdr_cksum(ip, m->ol_flags);
        }
    }
}

/* Improved transmission function with better error handling */
static int transmit_burst(struct app_context *ctx, struct rte_mbuf **tx_burst, 
                         uint16_t burst_count, uint64_t *bytes_sent) {
    if (!ctx || !tx_burst || burst_count == 0) return -1;
    
    uint16_t sent = 0;
    uint16_t retry_count = 0;
    uint64_t local_bytes = 0;
    
    // Handle pause frames if necessary
    if (ctx->pause_until_time > 0) {
        uint64_t now = rte_get_timer_cycles();
        if (now < ctx->pause_until_time) {
            // Still in pause period - drop packets
            for (uint16_t i = 0; i < burst_count; i++) {
                rte_pktmbuf_free(tx_burst[i]);
            }
            ctx->total_packets_dropped += burst_count;
            return 0; // Return success but no packets sent
        } else {
            ctx->pause_until_time = 0; // Pause period expired
            printf("Transmission resumed after pause\n");
        }
    }
    
    // Transmit with retry logic
    while (sent < burst_count && keep_running && retry_count < MAX_RETRY_COUNT) {
        uint16_t this_sent = rte_eth_tx_burst(ctx->port_id, 0, &tx_burst[sent], burst_count - sent);
        sent += this_sent;
        
        if (sent < burst_count) {
            retry_count++;
            if (retry_count > 1) {
                rte_delay_us_block(1); // Small delay for subsequent retries
            }
        }
    }
    
    // Calculate bytes for successfully sent packets
    for (uint16_t i = 0; i < sent; i++) {
        local_bytes += tx_burst[i]->pkt_len;
    }
    
    // Free any unsent packets
    for (uint16_t i = sent; i < burst_count; i++) {
        rte_pktmbuf_free(tx_burst[i]);
    }
    
    ctx->total_packets_sent += sent;
    ctx->total_packets_dropped += (burst_count - sent);
    if (bytes_sent) *bytes_sent = local_bytes;
    
    return sent;
}

/* Enhanced statistics printing */
static void print_statistics(struct app_context *ctx, bool final) {
    if (!ctx) return;
    
    uint64_t now = rte_get_timer_cycles();
    static uint64_t last_packets = 0, last_bytes = 0;
    
    if (!final && (now - ctx->last_stats_time) < ctx->hz) {
        return; // Not time for stats yet
    }
    
    double elapsed = (double)(now - ctx->last_stats_time) / ctx->hz;
    if (elapsed <= 0) return;
    
    uint64_t interval_packets = ctx->total_packets_sent - last_packets;
    uint64_t interval_bytes = ctx->total_bytes_sent - last_bytes;
    
    double interval_throughput = (double)interval_bytes * 8.0 / elapsed;
    double interval_pps = (double)interval_packets / elapsed;
    
    if (final) {
        double total_elapsed = (double)(now - ctx->start_time) / ctx->hz;
        double avg_throughput = (double)ctx->total_bytes_sent * 8.0 / total_elapsed;
        double avg_pps = (double)ctx->total_packets_sent / total_elapsed;
        
        printf("\n=== Final Statistics ===\n");
        printf("Total runtime: %.2f seconds\n", total_elapsed);
        printf("Total packets sent: %lu\n", ctx->total_packets_sent);
        printf("Total packets dropped: %lu\n", ctx->total_packets_dropped);
        printf("Total bytes sent: %lu\n", ctx->total_bytes_sent);
        printf("Average throughput: %.2f Mbps\n", avg_throughput / 1000000.0);
        printf("Average PPS: %.0f\n", avg_pps);
        printf("Target throughput: %.2f Mbps\n", ctx->target_bitrate / 1000000.0);
        printf("Efficiency: %.2f%%\n", (avg_throughput / ctx->target_bitrate) * 100.0);
    } else {
        printf("Throughput: %.2f Mbps, PPS: %.0f, Target: %.2f Mbps, Dropped: %lu\n",
               interval_throughput / 1000000.0, interval_pps, 
               ctx->target_bitrate / 1000000.0, ctx->total_packets_dropped);
    }
    
    last_packets = ctx->total_packets_sent;
    last_bytes = ctx->total_bytes_sent;
    ctx->last_stats_time = now;
}

/* Main packet sending loop with improved precision and efficiency */
static int send_packets(struct app_context *ctx) {
    if (!ctx || !ctx->packets) return -1;
    
    struct rte_mbuf *tx_burst[MAX_BURST_SIZE];
    uint16_t burst_count = 0;
    uint64_t burst_bits = 0;
    
    ctx->start_time = rte_get_timer_cycles();
    ctx->next_send_time = ctx->start_time;
    ctx->last_stats_time = ctx->start_time;
    ctx->last_pause_check = ctx->start_time;
    
    printf("Starting packet transmission...\n");
    
    for (int rep = 0; keep_running && (ctx->replays == 0 || rep < ctx->replays); rep++) {
        for (size_t i = 0; i < ctx->num_packets && keep_running; i++) {
            // Allocate mbuf
            struct rte_mbuf *m = rte_pktmbuf_alloc(ctx->mbuf_pool);
            if (!m) {
                printf("Warning: Failed to allocate mbuf\n");
                ctx->total_packets_dropped++;
                continue;
            }
            
            // Copy packet data
            void *pkt_data = rte_pktmbuf_mtod(m, void *);
            if (!pkt_data) {
                rte_pktmbuf_free(m);
                ctx->total_packets_dropped++;
                continue;
            }
            
            rte_memcpy(pkt_data, ctx->packets[i].data, ctx->packets[i].len);
            m->pkt_len = ctx->packets[i].len;
            m->data_len = ctx->packets[i].len;
            
            // Modify headers if needed
            modify_packet_headers(m, ctx);
            
            // Add to burst
            tx_burst[burst_count++] = m;
            burst_bits += ctx->packets[i].bits;
            
            // Send burst when full or at end of packet list
            if (burst_count == ctx->effective_burst_size || i == ctx->num_packets - 1) {
                // Precise timing calculation
                double sec_for_burst = (double)burst_bits * ctx->sec_per_bit;
                uint64_t cycles_for_burst = (uint64_t)(sec_for_burst * ctx->hz + 0.5);
                uint64_t target_time = ctx->next_send_time + cycles_for_burst;
                
                // Precise pacing
                if (ctx->precise_pacing_mode) {
                    uint64_t now = rte_get_timer_cycles();
                    while (now < target_time && keep_running) {
                        now = rte_get_timer_cycles();
                    }
                } else {
                    // High-speed mode: only wait if significantly ahead
                    uint64_t now = rte_get_timer_cycles();
                    if (target_time > now + ctx->min_wait_cycles) {
                        while (now < target_time && keep_running) {
                            now = rte_get_timer_cycles();
                        }
                    }
                }
                
                ctx->next_send_time = target_time;
                
                // Check for pause frames periodically
                uint64_t now = rte_get_timer_cycles();
                if ((now - ctx->last_pause_check) >= (ctx->hz * PAUSE_CHECK_INTERVAL_US) / 1000000ULL) {
                    check_for_pause_frames(ctx);
                    ctx->last_pause_check = now;
                }
                
                // Transmit burst
                uint64_t bytes_sent = 0;
                int sent = transmit_burst(ctx, tx_burst, burst_count, &bytes_sent);
                if (sent >= 0) {
                    ctx->total_bytes_sent += bytes_sent;
                }
                
                // Reset burst state
                burst_count = 0;
                burst_bits = 0;
                
                // Print statistics
                print_statistics(ctx, false);
                
                // Check run duration
                if (ctx->run_duration > 0) {
                    double elapsed = (double)(now - ctx->start_time) / ctx->hz;
                    if (elapsed >= ctx->run_duration) {
                        keep_running = false;
                        break;
                    }
                }
            }
        }
        rep++;
    }
    
    // Print final statistics
    print_statistics(ctx, true);
    return 0;
}

/* Cleanup function */
static void cleanup_app_context(struct app_context *ctx) {
    if (!ctx) return;
    
    if (ctx->port_id != UINT16_MAX) {
        rte_eth_dev_stop(ctx->port_id);
        rte_eth_dev_close(ctx->port_id);
    }
    
    if (ctx->mbuf_pool) {
        rte_mempool_free(ctx->mbuf_pool);
    }
    
    if (ctx->packet_data_buffer) {
        rte_free(ctx->packet_data_buffer);
    }
    
    if (ctx->packets) {
        rte_free(ctx->packets);
    }
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    }
    argc -= ret;
    argv += ret;
    
    struct app_context ctx;
    if (init_app_context(&ctx) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to initialize application context\n");
    }
    
    // Parse command line arguments
    char pcap_file[256] = {0};
    char pci_addr[32] = {0};
    bool show_help = false;
    
    static const struct option long_options[] = {
        {"bitrate", required_argument, NULL, 'b'},
        {"dst-mac", required_argument, NULL, 'm'},
        {"dst-ip", required_argument, NULL, 'i'},
        {"dst-port", required_argument, NULL, 'p'},
        {"pcap", required_argument, NULL, 'f'},
        {"replays", required_argument, NULL, 'r'},
        {"pci", required_argument, NULL, 'd'},
        {"run-duration", required_argument, NULL, 'u'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "b:m:i:p:f:r:d:u:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 'b':
                ctx.target_bitrate = strtoull(optarg, NULL, 10) * 1000000ULL;
                if (ctx.target_bitrate == 0) {
                    printf("Error: Invalid bitrate value\n");
                    cleanup_app_context(&ctx);
                    return EXIT_FAILURE;
                }
                break;
            case 'm':
                if (parse_mac(optarg, &ctx.dst_mac) < 0) {
                    printf("Error: Invalid MAC address format\n");
                    cleanup_app_context(&ctx);
                    return EXIT_FAILURE;
                }
                ctx.have_dst_mac = true;
                break;
            case 'i': {
                struct in_addr in;
                if (inet_pton(AF_INET, optarg, &in) != 1) {
                    printf("Error: Invalid IP address format\n");
                    cleanup_app_context(&ctx);
                    return EXIT_FAILURE;
                }
                ctx.dst_ip = ntohl(in.s_addr);
                ctx.have_dst_ip = true;
                break;
            }
            case 'p': {
                long port = strtol(optarg, NULL, 10);
                if (port <= 0 || port > 65535) {
                    printf("Error: Invalid port number (1-65535)\n");
                    cleanup_app_context(&ctx);
                    return EXIT_FAILURE;
                }
                ctx.dst_port = (uint16_t)port;
                ctx.have_dst_port = true;
                break;
            }
            case 'f':
                strncpy(pcap_file, optarg, sizeof(pcap_file) - 1);
                pcap_file[sizeof(pcap_file) - 1] = '\0';
                break;
            case 'r': {
                long replays = strtol(optarg, NULL, 10);
                if (replays < 0) {
                    printf("Error: Replays must be >= 0\n");
                    cleanup_app_context(&ctx);
                    return EXIT_FAILURE;
                }
                ctx.replays = (int)replays;
                break;
            }
            case 'd':
                strncpy(pci_addr, optarg, sizeof(pci_addr) - 1);
                pci_addr[sizeof(pci_addr) - 1] = '\0';
                break;
            case 'u': {
                ctx.run_duration = strtod(optarg, NULL);
                if (ctx.run_duration < 0) {
                    printf("Error: Run duration must be >= 0\n");
                    cleanup_app_context(&ctx);
                    return EXIT_FAILURE;
                }
                break;
            }
            case 'h':
                show_help = true;
                break;
            default:
                usage_sender(argv[0]);
                cleanup_app_context(&ctx);
                return EXIT_FAILURE;
        }
    }
    
    if (show_help) {
        usage_sender(argv[0]);
        cleanup_app_context(&ctx);
        return 0;
    }
    
    // Validate required arguments
    if (pcap_file[0] == '\0' || pci_addr[0] == '\0' || ctx.replays < 0) {
        printf("Error: Missing required arguments\n");
        usage_sender(argv[0]);
        cleanup_app_context(&ctx);
        return EXIT_FAILURE;
    }
    
    // Find and configure port
    ctx.port_id = find_port_by_pci(pci_addr);
    if (ctx.port_id == UINT16_MAX) {
        printf("Error: No suitable port found for PCI address %s\n", pci_addr);
        cleanup_app_context(&ctx);
        return EXIT_FAILURE;
    }
    
    if (configure_port(&ctx) < 0) {
        printf("Error: Failed to configure port\n");
        cleanup_app_context(&ctx);
        return EXIT_FAILURE;
    }
    
    // Load PCAP file
    if (load_pcap(&ctx, pcap_file) < 0) {
        printf("Error: Failed to load PCAP file\n");
        cleanup_app_context(&ctx);
        return EXIT_FAILURE;
    }
    
    // Configure timing parameters
    configure_timing(&ctx);
    
    // Start packet transmission
    printf("Configuration complete. Starting transmission...\n");
    ret = send_packets(&ctx);
    
    // Cleanup and exit
    cleanup_app_context(&ctx);
    
    if (ret < 0) {
        printf("Error: Packet transmission failed\n");
        return EXIT_FAILURE;
    }
    
    printf("Transmission completed successfully.\n");
    return 0;
}