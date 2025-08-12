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
#include <time.h>

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
#include <rte_atomic.h>
#include <rte_lcore.h>

// Optimized configuration constants
#define RX_RING_SIZE 1024
#define TX_RING_SIZE 8192  // Increased for better throughput
#define NUM_MBUFS 16383    // Increased for higher packet rates
#define MBUF_CACHE_SIZE 512
#define BURST_SIZE 256     // Optimized burst size
#define MAX_BURST_SIZE 1024
#define PAUSE_CHECK_INTERVAL 1000  // Check pause frames every 1000 packets
#define STATS_INTERVAL 1000000     // Print stats every 1M packets

// Packet structure with better memory layout
struct packet_info {
    uint32_t len;
    uint32_t offset;  // Offset into the single data buffer
    uint16_t eth_type;
    uint8_t ip_proto;
    uint8_t has_udp;
};

// Thread-safe statistics
struct sender_stats {
    rte_atomic64_t total_packets;
    rte_atomic64_t total_bytes;
    rte_atomic64_t dropped_packets;
    rte_atomic64_t pause_frames_received;
    uint64_t start_time;
    uint64_t last_stats_time;
};

volatile bool keep_running = true;
static struct sender_stats stats = {0};

static void sig_handler(int sig) {
    (void)sig;
    keep_running = false;
}

static void usage_sender(const char *progname) {
    printf("Usage: %s [EAL options] -- [app options]\n", progname);
    printf("App options:\n");
    printf("  --bitrate <Mbps>       Target send bitrate (optional; default: link speed)\n");
    printf("  --dst-mac <MAC>        Destination MAC address (optional)\n");
    printf("  --dst-ip <IP>          Destination IPv4 address (optional)\n");
    printf("  --dst-port <port>      Destination UDP port (optional)\n");
    printf("  --pcap <file>          Input PCAP file to replay (required)\n");
    printf("  --replays <N>          Number of replays (required, 0 for infinite)\n");
    printf("  --pci <address>        PCI address of the TX port (required)\n");
    printf("  --run-duration <seconds> Maximum run duration (optional)\n");
    printf("  --burst-size <size>    Custom burst size (optional)\n");
    printf("  --help                 Show this help\n");
    printf("\n");
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
    uint16_t port_id;
    RTE_ETH_FOREACH_DEV(port_id) {
        struct rte_eth_dev_info dev_info;
        int ret = rte_eth_dev_info_get(port_id, &dev_info);
        if (ret == 0) {
            // Try to match PCI address
            if (strstr(dev_info.device->name, pci_addr_str) != NULL) {
                printf("Found matching port %u for PCI %s\n", port_id, pci_addr_str);
                return port_id;
            }
        }
    }
    
    // Fallback to first available port
    if (rte_eth_dev_count_avail() > 0) {
        printf("Warning: No exact PCI match found, using first available port\n");
        return 0;
    }
    return UINT16_MAX;
}

// Improved pause frame detection with better error handling
static bool check_for_pause_frames(uint16_t port_id, uint64_t *pause_until_time, uint64_t hz) {
    if (pause_until_time == NULL) {
        return false;
    }
    
    struct rte_mbuf *rx_bufs[16];
    uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, rx_bufs, 16);
    bool pause_detected = false;
    
    for (uint16_t i = 0; i < nb_rx; i++) {
        struct rte_mbuf *m = rx_bufs[i];
        if (m == NULL) {
            continue;
        }
        
        if (m->pkt_len >= 64) {
            uint8_t *data = rte_pktmbuf_mtod(m, uint8_t *);
            if (data != NULL) {
                // Check for Ethernet pause frame: EtherType 0x8808, Opcode 0x0001
                if (data[12] == 0x88 && data[13] == 0x08 &&
                    data[14] == 0x00 && data[15] == 0x01) {
                    
                    uint16_t pause_time = (data[16] << 8) | data[17];
                    if (pause_time > 0) {
                        uint64_t pause_ns = (uint64_t)pause_time * 512;
                        uint64_t pause_cycles = (pause_ns * hz) / 1000000000ULL;
                        
                        *pause_until_time = rte_get_timer_cycles() + pause_cycles;
                        pause_detected = true;
                        rte_atomic64_inc(&stats.pause_frames_received);
                        printf("Pause frame detected: %u units (%lu ns pause)\n", 
                               pause_time, pause_ns);
                    }
                }
            }
        }
        rte_pktmbuf_free(m);
    }
    
    return pause_detected;
}

// Optimized packet header modification with hardware offloads
static inline void modify_packet_headers(struct rte_mbuf *m, 
                                        const struct rte_ether_addr *dst_mac,
                                        uint32_t dst_ip, uint16_t dst_port,
                                        bool have_dst_mac, bool have_dst_ip, bool have_dst_port) {
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    
    if (have_dst_mac) {
        eth->dst_addr = *dst_mac;
    }
    
    // Only process if packet is large enough for IP header
    if (m->pkt_len < sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)) {
        return;
    }
    
    if (rte_be_to_cpu_16(eth->ether_type) != RTE_ETHER_TYPE_IPV4) {
        return;
    }
    
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    
    if (have_dst_ip) {
        ip->dst_addr = rte_cpu_to_be_32(dst_ip);
        
        // Enable hardware checksum offload
        m->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM | RTE_MBUF_F_TX_IPV4;
        m->l2_len = sizeof(struct rte_ether_hdr);
        m->l3_len = sizeof(struct rte_ipv4_hdr);
        ip->hdr_checksum = 0;
    }
    
    // Process UDP header if present
    if (have_dst_port && ip->next_proto_id == IPPROTO_UDP) {
        uint16_t ip_hdr_len = (ip->ihl & 0x0f) << 2;
        if (m->pkt_len >= sizeof(struct rte_ether_hdr) + ip_hdr_len + sizeof(struct rte_udp_hdr)) {
            struct rte_udp_hdr *udp = (struct rte_udp_hdr *)((uint8_t *)ip + ip_hdr_len);
            udp->dst_port = rte_cpu_to_be_16(dst_port);
            
            // Enable hardware UDP checksum offload
            m->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
            udp->dgram_cksum = rte_ipv4_phdr_cksum(ip, m->ol_flags);
        }
    }
}

// Optimized timing calculation
static inline uint64_t calculate_burst_delay(uint64_t burst_bits, uint64_t bitrate, uint64_t hz) {
    if (bitrate == 0) return 0;
    
    // Use 64-bit arithmetic to avoid overflow
    uint64_t bits_per_sec = bitrate;
    uint64_t ns_per_burst = (burst_bits * 1000000000ULL) / bits_per_sec;
    return (ns_per_burst * hz) / 1000000000ULL;
}

// Print statistics with better formatting
static void print_stats(uint64_t hz) {
    uint64_t now = rte_get_timer_cycles();
    double elapsed = (double)(now - stats.last_stats_time) / hz;
    
    if (elapsed < 1.0) return; // Print at most once per second
    
    uint64_t current_packets = rte_atomic64_read(&stats.total_packets);
    uint64_t current_bytes = rte_atomic64_read(&stats.total_bytes);
    uint64_t current_dropped = rte_atomic64_read(&stats.dropped_packets);
    
    static uint64_t last_packets = 0;
    static uint64_t last_bytes = 0;
    
    uint64_t interval_packets = current_packets - last_packets;
    uint64_t interval_bytes = current_bytes - last_bytes;
    
    if (elapsed > 0) {
        double throughput = (double)interval_bytes * 8.0 / elapsed;
        double pps = (double)interval_packets / elapsed;
        
        printf("Throughput: %.2f Mbps, PPS: %.0f, Dropped: %lu, Pause frames: %ld\n",
               throughput / 1000000.0, pps, current_dropped, 
               rte_atomic64_read(&stats.pause_frames_received));
    }
    
    last_packets = current_packets;
    last_bytes = current_bytes;
    stats.last_stats_time = now;
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

    // Command line arguments
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
    uint16_t custom_burst_size = 0;

    static const struct option long_options[] = {
        {"bitrate", required_argument, NULL, 'b'},
        {"dst-mac", required_argument, NULL, 'm'},
        {"dst-ip", required_argument, NULL, 'i'},
        {"dst-port", required_argument, NULL, 'p'},
        {"pcap", required_argument, NULL, 'f'},
        {"replays", required_argument, NULL, 'r'},
        {"pci", required_argument, NULL, 'd'},
        {"run-duration", required_argument, NULL, 'u'},
        {"burst-size", required_argument, NULL, 's'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "b:m:i:p:f:r:d:u:s:h", long_options, NULL)) != -1) {
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
            case 's':
                custom_burst_size = atoi(optarg);
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

    // Find and configure port
    uint16_t port_id = find_port_by_pci(pci_addr);
    if (port_id == UINT16_MAX) {
        rte_exit(EXIT_FAILURE, "Port not found for PCI %s\n", pci_addr);
    }

    int socket_id = rte_eth_dev_socket_id(port_id);

    // Create memory pool with optimized settings
    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
                                                            MBUF_CACHE_SIZE, 0, 
                                                            RTE_MBUF_DEFAULT_BUF_SIZE, socket_id);
    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");
    }

    // Configure port with optimized settings
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

    // Setup TX queue with optimized configuration
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
    
    ret = rte_eth_tx_queue_setup(port_id, 0, TX_RING_SIZE, socket_id, &txconf);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot setup tx queue: err=%d, port=%u\n", ret, port_id);
    }

    ret = rte_eth_dev_start(port_id);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Cannot start device: err=%d, port=%u\n", ret, port_id);
    }

    // Get link information
    struct rte_eth_link link;
    ret = rte_eth_link_get_nowait(port_id, &link);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "Failed to get link info: err=%d\n", ret);
    }
    if (link.link_status != RTE_ETH_LINK_UP) {
        rte_exit(EXIT_FAILURE, "Link is down\n");
    }
    if (bitrate == 0) {
        bitrate = (uint64_t)link.link_speed * 1000000ULL;
    }

    printf("Link speed: %u Mbps, Target bitrate: %lu Mbps\n", 
           link.link_speed, bitrate / 1000000ULL);

    // Configure flow control
    struct rte_eth_fc_conf fc_conf = {
        .mode = RTE_ETH_FC_FULL,
        .send_xon = 1,
        .pause_time = 0x1000,
    };
    
    ret = rte_eth_dev_flow_ctrl_set(port_id, &fc_conf);
    if (ret < 0) {
        printf("Warning: Failed to set flow control, err=%d\n", ret);
    } else {
        printf("Flow control enabled\n");
    }

    // Load PCAP file with improved error handling
    char errbuf[PCAP_ERRBUF_SIZE];
    pcap_t *pcap = pcap_open_offline(pcap_file, errbuf);
    if (pcap == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot open pcap: %s\n", errbuf);
    }

    // First pass: count packets and calculate total size
    size_t num_packets = 0;
    size_t total_data_size = 0;
    struct pcap_pkthdr *hdr;
    const u_char *data;
    
    while (pcap_next_ex(pcap, &hdr, &data) == 1) {
        num_packets++;
        total_data_size += hdr->caplen;
    }
    
    if (num_packets == 0) {
        rte_exit(EXIT_FAILURE, "No packets in PCAP\n");
    }

    printf("Loaded PCAP: %zu packets, %zu bytes total\n", num_packets, total_data_size);

    // Allocate single buffer for all packet data
    uint8_t *packet_data_buffer = rte_malloc("packet_data_buffer", total_data_size, 64);
    if (packet_data_buffer == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot allocate packet data buffer\n");
    }

    // Allocate packet info array
    struct packet_info *packets = rte_malloc("packet_info", num_packets * sizeof(struct packet_info), 64);
    if (packets == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot allocate packet info array\n");
    }

    // Second pass: copy packet data and build info array
    pcap_close(pcap);
    pcap = pcap_open_offline(pcap_file, errbuf);
    if (pcap == NULL) {
        rte_exit(EXIT_FAILURE, "Cannot reopen pcap: %s\n", errbuf);
    }

    size_t offset = 0;
    size_t packet_idx = 0;
    
    while (pcap_next_ex(pcap, &hdr, &data) == 1 && packet_idx < num_packets) {
        if (hdr->caplen > 0 && data != NULL) {
            memcpy(packet_data_buffer + offset, data, hdr->caplen);
            
            packets[packet_idx].len = hdr->caplen;
            packets[packet_idx].offset = offset;
            
            // Parse packet headers for optimization
            if (hdr->caplen >= sizeof(struct rte_ether_hdr)) {
                struct rte_ether_hdr *eth = (struct rte_ether_hdr *)(packet_data_buffer + offset);
                packets[packet_idx].eth_type = rte_be_to_cpu_16(eth->ether_type);
                
                if (packets[packet_idx].eth_type == RTE_ETHER_TYPE_IPV4 && 
                    hdr->caplen >= sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr)) {
                    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
                    packets[packet_idx].ip_proto = ip->next_proto_id;
                    packets[packet_idx].has_udp = (ip->next_proto_id == IPPROTO_UDP);
                }
            }
            
            offset += hdr->caplen;
            packet_idx++;
        }
    }
    pcap_close(pcap);

    // Determine optimal burst size
    uint16_t effective_burst_size = custom_burst_size;
    if (effective_burst_size == 0) {
        if (bitrate <= 1000000000ULL) {
            effective_burst_size = 32;
        } else if (bitrate <= 5000000000ULL) {
            effective_burst_size = 64;
        } else if (bitrate <= 15000000000ULL) {
            effective_burst_size = 128;
        } else {
            effective_burst_size = BURST_SIZE;
        }
    }
    
    if (effective_burst_size > MAX_BURST_SIZE) {
        effective_burst_size = MAX_BURST_SIZE;
    }
    
    printf("Using burst size: %u\n", effective_burst_size);

    // Initialize statistics
    stats.start_time = rte_get_timer_cycles();
    stats.last_stats_time = stats.start_time;
    rte_atomic64_init(&stats.total_packets);
    rte_atomic64_init(&stats.total_bytes);
    rte_atomic64_init(&stats.dropped_packets);
    rte_atomic64_init(&stats.pause_frames_received);

    // Main sending loop
    uint64_t hz = rte_get_timer_hz();
    uint64_t next_send_time = stats.start_time;
    uint64_t pause_until_time = 0;
    uint64_t pause_check_counter = 0;
    uint64_t stats_counter = 0;
    
    struct rte_mbuf *tx_burst[MAX_BURST_SIZE];
    uint16_t burst_count = 0;
    uint64_t burst_bits = 0;
    uint64_t burst_bytes = 0;

    int rep = 0;
    while (keep_running && (replays == 0 || rep < replays)) {
        for (size_t i = 0; i < num_packets && keep_running; i++) {
            // Check for pause frames periodically
            if (++pause_check_counter >= PAUSE_CHECK_INTERVAL) {
                check_for_pause_frames(port_id, &pause_until_time, hz);
                pause_check_counter = 0;
            }

            // Honor pause frame timing
            uint64_t now = rte_get_timer_cycles();
            if (pause_until_time > 0 && now < pause_until_time) {
                // Skip transmission during pause
                rte_atomic64_inc(&stats.dropped_packets);
                continue;
            } else if (pause_until_time > 0 && now >= pause_until_time) {
                pause_until_time = 0;
                printf("Sender resumed after pause\n");
            }

            // Allocate mbuf
            struct rte_mbuf *m = rte_pktmbuf_alloc(mbuf_pool);
            if (m == NULL) {
                rte_atomic64_inc(&stats.dropped_packets);
                continue;
            }

            // Copy packet data
            const struct packet_info *pkt_info = &packets[i];
            if (rte_pktmbuf_append(m, pkt_info->len) == NULL) {
                rte_pktmbuf_free(m);
                rte_atomic64_inc(&stats.dropped_packets);
                continue;
            }
            
            memcpy(rte_pktmbuf_mtod(m, void *), 
                   packet_data_buffer + pkt_info->offset, 
                   pkt_info->len);

            // Modify headers if needed
            modify_packet_headers(m, &dst_mac, dst_ip, dst_port,
                                have_dst_mac, have_dst_ip, have_dst_port);

            // Add to burst
            tx_burst[burst_count++] = m;
            burst_bits += (uint64_t)(pkt_info->len + 24) * 8; // Include wire overhead
            burst_bytes += pkt_info->len;

            // Send burst when full or last packet
            if (burst_count == effective_burst_size || i == num_packets - 1) {
                // Calculate timing
                uint64_t delay_cycles = calculate_burst_delay(burst_bits, bitrate, hz);
                uint64_t target_time = next_send_time + delay_cycles;
                
                // Wait if needed
                while (rte_get_timer_cycles() < target_time && keep_running) {
                    rte_pause();
                }
                
                next_send_time = target_time;

                // Send burst with retry logic
                uint16_t sent = 0;
                uint16_t retry_count = 0;
                const uint16_t max_retries = 3;
                
                while (sent < burst_count && keep_running && retry_count < max_retries) {
                    uint16_t this_sent = rte_eth_tx_burst(port_id, 0, 
                                                        &tx_burst[sent], 
                                                        burst_count - sent);
                    sent += this_sent;
                    
                    if (sent < burst_count) {
                        retry_count++;
                        if (retry_count < max_retries) {
                            rte_delay_us_block(1);
                        }
                    }
                }

                // Free unsent packets
                for (uint16_t j = sent; j < burst_count; j++) {
                    rte_pktmbuf_free(tx_burst[j]);
                    rte_atomic64_inc(&stats.dropped_packets);
                }

                // Update statistics
                rte_atomic64_add(&stats.total_packets, sent);
                rte_atomic64_add(&stats.total_bytes, burst_bytes);

                // Reset burst state
                burst_count = 0;
                burst_bits = 0;
                burst_bytes = 0;

                // Print statistics periodically
                if (++stats_counter >= STATS_INTERVAL) {
                    print_stats(hz);
                    stats_counter = 0;
                }

                // Check run duration
                if (run_duration > 0) {
                    double elapsed_total = (double)(rte_get_timer_cycles() - stats.start_time) / hz;
                    if (elapsed_total >= run_duration) {
                        keep_running = false;
                        break;
                    }
                }
            }
        }
        rep++;
    }

    // Final statistics
    uint64_t end_time = rte_get_timer_cycles();
    double total_elapsed = (double)(end_time - stats.start_time) / hz;
    
    if (total_elapsed > 0) {
        uint64_t total_pkts = rte_atomic64_read(&stats.total_packets);
        uint64_t total_bytes = rte_atomic64_read(&stats.total_bytes);
        uint64_t dropped_pkts = rte_atomic64_read(&stats.dropped_packets);
        uint64_t pause_frames = rte_atomic64_read(&stats.pause_frames_received);
        
        double avg_th = (double)total_bytes * 8.0 / total_elapsed;
        double avg_pps = (double)total_pkts / total_elapsed;
        
        printf("\n=== Final Statistics ===\n");
        printf("Total packets sent: %lu\n", total_pkts);
        printf("Total bytes sent: %lu\n", total_bytes);
        printf("Dropped packets: %lu\n", dropped_pkts);
        printf("Pause frames received: %lu\n", pause_frames);
        printf("Average throughput: %.2f Mbps\n", avg_th / 1000000.0);
        printf("Average packet rate: %.0f pps\n", avg_pps);
        printf("Total runtime: %.2f seconds\n", total_elapsed);
    }

    // Cleanup
    rte_eth_dev_stop(port_id);
    rte_eth_dev_close(port_id);
    rte_mempool_free(mbuf_pool);
    rte_free(packet_data_buffer);
    rte_free(packets);

    return 0;
}