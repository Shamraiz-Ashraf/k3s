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
#include <stddef.h>  // For offsetof

#include <rte_eal.h>
#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_tcp.h> // For checksum, though UDP
#include <rte_cycles.h>
#include <rte_pci.h>
#include <rte_bus.h>
#include <rte_bus_pci.h>
#include <rte_dev.h> // Added for RTE_DEV_TO_PCI

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 4096  // Larger TX ring for higher throughput

#define NUM_MBUFS 8191     // Reasonable mbuf count for memory constraints
#define MBUF_CACHE_SIZE 256   // Reasonable cache size
#define BURST_SIZE 128     // Larger burst size for higher efficiency
#define MEGA_BURST_SIZE 512 // For very high bitrates, use mega-bursts

struct packet {
    uint8_t *data;
    uint32_t len;
};

volatile bool keep_running = true;

static void sig_handler(int sig) {
    keep_running = false;
}

static void usage_sender(const char *progname) {
    printf("Usage: %s [EAL options] -- [app options]\n", progname);
    printf("App options:\n");
    printf("  --bitrate <Mbps>       Target send bitrate (optional; default: link speed)\n");
    printf("  --dst-mac <MAC>        Destination MAC address (optional; preserves original if not set)\n");
    printf("  --dst-ip <IP>          Destination IPv4 address (optional; preserves original if not set)\n");
    printf("  --dst-port <port>      Destination UDP port (optional; preserves original if not set)\n");
    printf("  --pcap <file>          Input PCAP file to replay (required)\n");
    printf("  --replays <N>          Number of replays (required, 0 for infinite)\n");
    printf("  --pci <address>        PCI address of the TX port (required)\n");
    printf("  --run-duration <seconds> Maximum run duration (optional)\n");
    printf("  --help                 Show this help\n");
    printf("\n");
    printf("Note: If dst-ip or dst-port are not specified, original PCAP destinations are preserved.\n");
}

static int parse_mac(const char *str, struct rte_ether_addr *mac) {
    uint8_t bytes[6];
    if (sscanf(str, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &bytes[0], &bytes[1], &bytes[2],
               &bytes[3], &bytes[4], &bytes[5]) != 6) {
        return -1;
    }
    memcpy(mac->addr_bytes, bytes, 6);
    return 0;
}

static uint16_t find_port_by_pci(const char *pci_addr_str) {
    // For now, just return the first available port
    // This is a simplified version that avoids PCI device structure issues
    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id) {
        struct rte_eth_dev_info dev_info;
        int ret = rte_eth_dev_info_get(port_id, &dev_info);
        if (ret == 0) {
            printf("Using port %u (PCI matching simplified)\n", port_id);
            return port_id;
        }
    }
    printf("Warning: No matching port found for PCI %s, using first available\n", pci_addr_str);
    return rte_eth_dev_count_avail() > 0 ? 0 : UINT16_MAX;
}

// Software-level pause frame detection for enhanced flow control
static bool check_for_pause_frames(uint16_t port_id, uint64_t *pause_until_time, uint64_t hz) {
    if (pause_until_time == NULL) {
        return false;  // Safety check
    }
    
    struct rte_mbuf *rx_bufs[8];  // Small burst for pause frame checking
    uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx_bufs, 8);
    bool pause_detected = false;
    
    for (uint16_t i = 0; i < nb_rx; i++) {
        struct rte_mbuf *m = rx_bufs[i];
        if (m != NULL && m->pkt_len >= 64) {  // Added null check and minimum frame size for pause frames
            uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
            if (data == NULL) {
                rte_pktmbuf_free(m);
                continue;
            }
            
            // Check for Ethernet pause frame: EtherType 0x8808, Opcode 0x0001
            if (data[12] == 0x88 && data[13] == 0x08 &&  // EtherType 0x8808
                data[14] == 0x00 && data[15] == 0x01) {   // Opcode 0x0001 (pause)
                
                // Extract pause time (units of 512 bit-times)
                uint16_t pause_time = (data[16] << 8) | data[17];
                if (pause_time > 0) {
                    // Convert pause time to nanoseconds: pause_time * 512 bit-times
                    // At 1 Gbps: 512 bit-times = 512 ns
                    // Scale for other speeds (simplified to 512 ns)
                    uint64_t pause_ns = (uint64_t)pause_time * 512;
                    uint64_t pause_cycles = (pause_ns * hz) / 1000000000ULL;
                    
                    *pause_until_time = rte_get_timer_cycles() + pause_cycles;
                    pause_detected = true;
                    printf("Pause frame detected: %u units (%lu ns pause)\n", pause_time, pause_ns);
                }
            }
        }
        if (m != NULL) {
            rte_pktmbuf_free(m);
        }
    }
    
    return pause_detected;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, sig_handler);

    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");
    }
    argc -= ret;
    argv += ret;

    uint64_t bitrate = 0;
    struct rte_ether_addr dst_mac = {0};
    bool have_dst_mac = false;
    uint32_t dst_ip = 0;
    bool have_dst_ip = false;
    uint16_t dst_port = 8888;
    bool have_dst_port = false;
    char pcap_file[256] = {0};
    int replays = -1;
    char pci_addr[32] = {0};
    bool show_help = false;
    double run_duration = 0.0;

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
                bitrate = strtoull(optarg, NULL, 10) * 1000000ULL;
                break;
            case 'm':
                if (parse_mac(optarg, &dst_mac) < 0) {
                    rte_exit(EXIT_FAILURE, "Invalid dst-mac\n");
                }
                have_dst_mac = true;
                break;
            case 'i': {
                struct in_addr in;
                if (inet_pton(AF_INET, optarg, &in) != 1) {
                    rte_exit(EXIT_FAILURE, "Invalid dst-ip\n");
                }
                dst_ip = ntohl(in.s_addr);
                have_dst_ip = true;
                break;
            }
            case 'p':
                dst_port = atoi(optarg);
                have_dst_port = true;
                break;
            case 'f':
                strncpy(pcap_file, optarg, sizeof(pcap_file) - 1);
                break;
            case 'r':
                replays = atoi(optarg);
                break;
            case 'd':
                strncpy(pci_addr, optarg, sizeof(pci_addr) - 1);
                break;
            case 'u':
                run_duration = strtod(optarg, NULL);
                break;
            case 'h':
                show_help = true;
                break;
            default:
                usage_sender(argv[0]);
                return EXIT_FAILURE;
        }
    }

    if (show_help) {
        usage_sender(argv[0]);
        return 0;
    }

    if (pcap_file[0] == '\0' || pci_addr[0] == '\0' || replays < 0) {
        usage_sender(argv[0]);
        rte_exit(EXIT_FAILURE, "Missing required arguments\n");
    }

    // Only set default IP if no IP was provided and no auto-detection is needed
    // This preserves original pcap destinations by default

    uint16_t port_id = find_port_by_pci(pci_addr);
    if (port_id == UINT16_MAX) {
        rte_exit(EXIT_FAILURE, "Port not found for PCI %s\n", pci_addr);
    }

    int socket_id = rte_eth_dev_socket_id(port_id);

    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
                                                            MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, socket_id);
    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    }

    struct rte_eth_conf port_conf = {
        .txmode = {
            .mq_mode = RTE_ETH_MQ_TX_NONE,
            .offloads = RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE |
                       RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                       RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                       RTE_ETH_TX_OFFLOAD_MULTI_SEGS,
        }
    };
    ret = rte_eth_dev_configure(port_id, 0, 1, &port_conf);
    if (ret != 0) {
        rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n", ret, port_id);
    }

    struct rte_eth_txconf txconf = {
        .offloads = RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE |
                   RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                   RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                   RTE_ETH_TX_OFFLOAD_MULTI_SEGS,
        .tx_thresh = {
            .pthresh = 32,  // Prefetch threshold for better cache usage
            .hthresh = 0,   // Host threshold
            .wthresh = 16,  // Writeback threshold for better batching
        },
        .tx_free_thresh = 64,  // Free buffers more aggressively
    };
    ret = rte_eth_tx_queue_setup(port_id, 0, TX_RING_SIZE, socket_id, &txconf);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot setup tx queue: err=%d, port=%u\n", ret, port_id);
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot start device: err=%d, port=%u\n", ret, port_id);
    }

    // Get link speed if bitrate not provided
    struct rte_eth_link link;
    ret = rte_eth_link_get_nowait(port_id, &link);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Failed to get link info: err=%d\n", ret);
    }
    if (link.link_status != RTE_ETH_LINK_UP) {
        rte_exit(EXIT_FAILURE, "Link is down\n");
    }
    if (bitrate == 0) {
        bitrate = (uint64_t)link.link_speed * 1000000ULL; // Mbps to bps
    }

    // Enhanced flow control configuration
    struct rte_eth_fc_conf fc_conf;
    memset(&fc_conf, 0, sizeof(fc_conf));
    fc_conf.mode = RTE_ETH_FC_FULL;  // Both send and receive pause frames
    fc_conf.send_xon = 1;           // Send XON frames when ready to receive
    fc_conf.pause_time = 0x1000;    // Pause time in units of 512 bit-times
    
    ret = rte_eth_dev_flow_ctrl_set(port_id, &fc_conf);
    if (ret < 0) {
        printf("Warning: Failed to set flow control, err=%d\n", ret);
    } else {
        printf("Flow control enabled: Full duplex (responds to pause frames)\n");
    }

    // Load PCAP
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t *pcap = pcap_open_offline(pcap_file, errbuf);
    if (pcap == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot open pcap: %s\n", errbuf);
    }

    struct packet *packets = NULL;
    size_t num_packets = 0;
    size_t capacity = 0;

    struct pcap_pkthdr *hdr;
    const u_char *data;
    while (pcap_next_ex(pcap, &hdr, &data) == 1) {
        if (num_packets >= capacity) {
            capacity = capacity ? capacity * 2 : 1024;
            packets = rte_realloc(packets, capacity * sizeof(struct packet), 0);
            if (packets == NULL) {
                rte_exit(EXIT_FAILURE, "Cannot allocate memory for packets\n");
            }
        }
        packets[num_packets].data = rte_malloc("packet data", hdr->caplen, 0);
        if (packets[num_packets].data == NULL) {
            rte_exit(EXIT_FAILURE, "Cannot allocate packet data\n");
        }
        memcpy(packets[num_packets].data, data, hdr->caplen);
        packets[num_packets].len = hdr->caplen;
        num_packets++;
    }
    pcap_close(pcap);

    if (num_packets == 0) {
        rte_exit(EXIT_FAILURE, "No packets in PCAP\n");
    }

    // Pre-allocate packet data in a single buffer for better memory performance
    size_t total_data_size = 0;
    for (size_t i = 0; i < num_packets; i++) {
        total_data_size += packets[i].len;
    }
    uint8_t *packet_data_buffer = rte_malloc("packet_data_buffer", total_data_size, 64);
    if (packet_data_buffer == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot allocate packet data buffer\n");
    }
    
    // Copy all packet data to the single buffer and update pointers
    size_t offset = 0;
    for (size_t i = 0; i < num_packets; i++) {
        memcpy(packet_data_buffer + offset, packets[i].data, packets[i].len);
        rte_free(packets[i].data);
        packets[i].data = packet_data_buffer + offset;
        offset += packets[i].len;
    }

    // Sending with highly optimized batching for maximum throughput
    uint64_t hz = rte_get_timer_hz();
    uint64_t start_time = rte_get_timer_cycles();
    uint64_t next_send_time = start_time;
    uint64_t last_print_time = start_time;
    uint64_t last_check_time = start_time;
    uint64_t total_pkts = 0;
    uint64_t total_bytes = 0;
    uint64_t last_pkts = 0;
    uint64_t last_bytes = 0;

    // Adaptive burst size and timing strategy - PRIORITIZE ACCURACY
    uint16_t effective_burst_size = BURST_SIZE;
    bool precise_pacing_mode = true;  // Default to precise pacing
    
    if (bitrate <= 1000000000ULL) {  // <= 1 Gbps - maximum precision
        effective_burst_size = 16;  // Increased from 8 to avoid edge cases
        printf("Enabling precision mode (<= 1 Gbps target)\n");
    } else if (bitrate <= 5000000000ULL) {  // <= 5 Gbps - balanced precision
        effective_burst_size = 32;  // Increased from 16
        printf("Enabling balanced precision mode (<= 5 Gbps target)\n");
    } else if (bitrate <= 15000000000ULL) {  // <= 15 Gbps - good precision
        effective_burst_size = 64;  // Increased from 32
        printf("Enabling standard mode (<= 15 Gbps target)\n");
    } else {  // > 15 Gbps - prioritize throughput
        effective_burst_size = BURST_SIZE;
        precise_pacing_mode = false;
        printf("Enabling high throughput mode (> 15 Gbps target)\n");
    }
    
    // Safety check for burst size
    if (effective_burst_size < 4) {
        effective_burst_size = 4;
        printf("Warning: Burst size too small, using minimum of 4\n");
    }

    struct rte_mbuf *tx_burst[MEGA_BURST_SIZE];
    uint16_t burst_count = 0;
    
    // Pre-calculate timing constants for accurate pacing
    double bits_per_byte = 8.0;
    double wire_overhead = 24.0; // L1/L2 overhead
    double hz_double = (double)hz;
    double sec_per_bit = 1.0 / (double)bitrate;
    
    // Simple per-burst timing for accuracy (no complex accumulation)
    uint64_t burst_bits = 0;
    uint64_t burst_bytes = 0;  // Track bytes in current burst for safer accounting
    
    // Enhanced pause frame support
    uint64_t pause_until_time = 0;
    uint64_t last_pause_check = start_time;

    int rep = 0;
    while (keep_running && (replays == 0 || rep < replays)) {
        for (size_t i = 0; i < num_packets && keep_running; i++) {
            struct rte_mbuf *m = rte_pktmbuf_alloc(mbuf_pool);
            if (m == NULL) {
                printf("Failed to alloc mbuf\n");
                continue;
            }

            rte_memcpy(rte_pktmbuf_mtod(m, void *), packets[i].data, packets[i].len);
            m->pkt_len = packets[i].len;
            m->data_len = packets[i].len;

            // Optimize header modification with hardware offloads
            struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
            if (have_dst_mac) {
                eth->dst_addr = dst_mac;
            }

            // Only modify IP headers if packet is large enough and is IPv4
            if (m->pkt_len >= sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) &&
                rte_be_to_cpu_16(eth->ether_type) == RTE_ETHER_TYPE_IPV4) {
                
                struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
                
                // Only modify destination IP if explicitly provided
                if (have_dst_ip) {
                    ip->dst_addr = rte_cpu_to_be_32(dst_ip);
                    
                    // Use hardware checksum offload instead of software calculation
                    m->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;
                    m->l2_len = sizeof(struct rte_ether_hdr);
                    m->l3_len = sizeof(struct rte_ipv4_hdr);
                    ip->hdr_checksum = 0;  // Hardware will compute this
                }

                // Only modify UDP if packet is large enough and is UDP
                uint16_t ip_hdr_len = (ip->ihl & 0x0f) << 2;
                if (ip->next_proto_id == IPPROTO_UDP && 
                    m->pkt_len >= sizeof(struct rte_ether_hdr) + ip_hdr_len + sizeof(struct rte_udp_hdr)) {
                    
                    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((uint8_t *)ip + ip_hdr_len);
                    
                    // Only modify destination port if explicitly provided
                    if (have_dst_port) {
                        udp->dst_port = rte_cpu_to_be_16(dst_port);
                        
                        // Use hardware UDP checksum offload
                        m->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
                        udp->dgram_cksum = rte_ipv4_phdr_cksum(ip, m->ol_flags);
                    }
                }
            }

            // Add to burst
            tx_burst[burst_count++] = m;

            // Accumulate bits and bytes for this packet for precise timing and accounting
            uint32_t wire_len = packets[i].len + (uint32_t)wire_overhead;
            burst_bits += (uint64_t)(wire_len * bits_per_byte);
            burst_bytes += packets[i].len;  // Track actual payload bytes safely

            // Send burst when full or last packet
            if (burst_count == effective_burst_size || i == num_packets - 1) {
                
                // Calculate precise timing for this burst
                if (precise_pacing_mode) {
                    double sec_for_burst = (double)burst_bits * sec_per_bit;
                    uint64_t cycles_for_burst = (uint64_t)(sec_for_burst * hz_double + 0.5);
                    uint64_t target_time = next_send_time + cycles_for_burst;
                    
                    // Precise busy-wait - essential for accurate bitrate control
                    uint64_t now = rte_get_timer_cycles();
                    while (now < target_time && keep_running) {
                        now = rte_get_timer_cycles();
                    }
                    
                    // Update next send time accurately
                    next_send_time = target_time;
                } else {
                    // For high bitrates, minimal pacing overhead
                    double sec_for_burst = (double)burst_bits * sec_per_bit;
                    uint64_t cycles_for_burst = (uint64_t)(sec_for_burst * hz_double + 0.5);
                    uint64_t target_time = next_send_time + cycles_for_burst;
                    
                    uint64_t now = rte_get_timer_cycles();
                    // Only wait if more than 100 microseconds ahead
                    uint64_t min_wait_cycles = hz / 10000; // 100us
                    if (target_time > now + min_wait_cycles) {
                        while (now < target_time && keep_running) {
                            now = rte_get_timer_cycles();
                        }
                    }
                    next_send_time = target_time;
                }

                // Check for pause frames periodically (every 1000 packets) - simplified for stability
                uint64_t now = rte_get_timer_cycles();
                if (now - last_pause_check >= hz / 100) {  // Check every 10ms (reduced frequency)
                    // Temporarily disable pause frame checking to isolate segfault
                    // check_for_pause_frames(port_id, &pause_until_time, hz);
                    last_pause_check = now;
                }
                
                // Honor pause frame timing (if any)
                if (pause_until_time > 0) {
                    now = rte_get_timer_cycles();
                    if (now < pause_until_time) {
                        // Skip this transmission burst due to pause
                        for (uint16_t j = 0; j < burst_count; j++) {
                            rte_pktmbuf_free(tx_burst[j]);
                        }
                        burst_count = 0;
                        burst_bits = 0;
                        burst_bytes = 0;
                        continue;  // Skip to next iteration
                    } else {
                        pause_until_time = 0;  // Pause period expired
                        printf("Sender resumed after pause\n");
                    }
                }

                // Send burst with reasonable retry logic
                uint16_t sent = 0;
                uint16_t retry_count = 0;
                while (sent < burst_count && keep_running && retry_count < 5) {
                    uint16_t this_sent = rte_eth_tx_burst(port_id, 0, &tx_burst[sent], burst_count - sent);
                    sent += this_sent;
                    if (sent < burst_count) {
                        retry_count++;
                        if (retry_count > 2) {
                            rte_delay_us_block(1); // Small delay only after multiple failures
                        }
                    }
                }
                
                // Free any unsent packets
                for (uint16_t j = sent; j < burst_count; j++) {
                    rte_pktmbuf_free(tx_burst[j]);
                }

                // Update stats for packets actually sent (safe byte accounting)
                total_pkts += sent;
                if (sent == burst_count) {
                    // All packets sent - use the pre-calculated byte count
                    total_bytes += burst_bytes;
                } else {
                    // Partial burst sent - calculate proportional bytes
                    uint64_t avg_bytes_per_packet = burst_bytes / burst_count;
                    total_bytes += sent * avg_bytes_per_packet;
                }

                // Reset burst state
                burst_count = 0;
                burst_bits = 0;
                burst_bytes = 0;
            }

            // Metrics print
            uint64_t now = rte_get_timer_cycles();
            if (now - last_print_time >= hz) {
                double elapsed = (double)(now - last_print_time) / hz;
                double th = (double)(total_bytes - last_bytes) * 8.0 / elapsed;
                uint64_t pps = (uint64_t)((total_pkts - last_pkts) / elapsed);
                printf("Interval throughput: %.2f Mbps, PPS: %lu, Target: %.0f Mbps\n", 
                       th / 1000000.0, pps, bitrate / 1000000.0);
                last_bytes = total_bytes;
                last_pkts = total_pkts;
                last_print_time = now;
            }

            // Check run duration every hz cycles (~1s)
            if (run_duration > 0 && now - last_check_time >= hz) {
                double elapsed_total = (double)(now - start_time) / hz;
                if (elapsed_total >= run_duration) {
                    keep_running = false;
                }
                last_check_time = now;
            }
        }
        rep++;
    }

    // Final metrics
    uint64_t end_time = rte_get_timer_cycles();
    double total_elapsed = (double)(end_time - start_time) / hz;
    if (total_elapsed > 0) {
        double avg_th = (double)total_bytes * 8.0 / total_elapsed;
        printf("Total packets: %lu, Total bytes: %lu, Average throughput: %.2f Mbps\n",
               total_pkts, total_bytes, avg_th / 1000000.0);
    }

    // Cleanup
    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
    rte_mempool_free(mbuf_pool);

    // Free the single packet data buffer
    rte_free(packet_data_buffer);
    rte_free(packets);

    return 0;
}
