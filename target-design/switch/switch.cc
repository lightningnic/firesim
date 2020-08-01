#include <functional>
#include <queue>
#include <algorithm>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>
#include <omp.h>
#include <cstdlib>
#include <arpa/inet.h>
#include <string>
#include <sstream>
#include <random>

#include <time.h>
#include <netinet/ether.h>
#include <net/ethernet.h>
#include "Packet.h"
#include "EthLayer.h"
#include "IPv4Layer.h"

#define IGNORE_PRINTF

#ifdef IGNORE_PRINTF
#define printf(fmt, ...) (0)
#endif

// param: link latency in cycles
// assuming 3.2 GHz, this number / 3.2 = link latency in ns
// e.g. setting this to 35000 gives you 35000/3.2 = 10937.5 ns latency
// IMPORTANT: this must be a multiple of 7
//
// THIS IS SET BY A COMMAND LINE ARGUMENT. DO NOT CHANGE IT HERE.
//#define LINKLATENCY 6405
int LINKLATENCY = 0;

// param: switching latency in cycles
// assuming 3.2 GHz, this number / 3.2 = switching latency in ns
//
// THIS IS SET BY A COMMAND LINE ARGUMENT. DO NOT CHANGE IT HERE.
int switchlat = 0;

#define SWITCHLATENCY (switchlat)

// param: numerator and denominator of bandwidth throttle
// Used to throttle outbound bandwidth from port
//
// THESE ARE SET BY A COMMAND LINE ARGUMENT. DO NOT CHANGE IT HERE.
int throttle_numer = 1;
int throttle_denom = 1;

// uncomment to use a limited output buffer size, OUTPUT_BUF_SIZE
//#define LIMITED_BUFSIZE

// size of output buffers, in # of flits
// only if LIMITED BUFSIZE is set
// TODO: expose in manager
#define OUTPUT_BUF_SIZE (131072L)

// pull in # clients config
#define NUMCLIENTSCONFIG
#include "switchconfig.h"
#undef NUMCLIENTSCONFIG

// DO NOT TOUCH
#define NUM_TOKENS (LINKLATENCY)
#define TOKENS_PER_BIGTOKEN (7)
#define BIGTOKEN_BYTES (64)
#define NUM_BIGTOKENS (NUM_TOKENS/TOKENS_PER_BIGTOKEN)
#define BUFSIZE_BYTES (NUM_BIGTOKENS*BIGTOKEN_BYTES)

// DO NOT TOUCH
#define SWITCHLAT_NUM_TOKENS (SWITCHLATENCY)
#define SWITCHLAT_NUM_BIGTOKENS (SWITCHLAT_NUM_TOKENS/TOKENS_PER_BIGTOKEN)
#define SWITCHLAT_BUFSIZE_BYTES (SWITCHLAT_NUM_BIGTOKENS*BIGTOKEN_BYTES)

uint64_t this_iter_cycles_start = 0;

// pull in mac2port array
#define MACPORTSCONFIG
#include "switchconfig.h"
#undef MACPORTSCONFIG

// Pull in load generator parameters, if any
#define LOADGENSTATS
#include "switchconfig.h"
#undef LOADGENSTATS
#ifdef USE_LOAD_GEN
#include "LnicLayer.h"
#include "AppLayer.h"
#define LOAD_GEN_MAC "08:55:66:77:88:08"
#define LOAD_GEN_IP "10.0.0.1"
#define NIC_MAC "00:26:E1:00:00:00"
#define NIC_IP "10.0.0.2"
#define MAX_TX_MSG_ID 127
uint64_t next_threshold = 0;
uint16_t global_tx_msg_id = 0;
bool start_message_received = false;
uint64_t global_start_message_count = 0;
std::exponential_distribution<double>* gen_dist;
std::default_random_engine* gen_rand;
std::exponential_distribution<double>* service_exp_dist;
std::default_random_engine* dist_rand;
std::normal_distribution<double>* service_normal_high;
std::normal_distribution<double>* service_normal_low;
std::binomial_distribution<int>* service_select_dist;
#endif

#include "flit.h"
#include "baseport.h"
#include "shmemport.h"
#include "socketport.h"
#include "sshport.h"

#define ETHER_HEADER_SIZE          14
#define IP_DST_FIELD_OFFSET        16 // Dest field immediately after, in same 64-bit flit
#define IP_SUBNET_OFFSET           2
#define IP_HEADER_SIZE             20 // TODO: Not always, just currently the case with L-NIC.

#define LNIC_DATA_FLAG_MASK        0b1
#define LNIC_ACK_FLAG_MASK         0b10
#define LNIC_NACK_FLAG_MASK        0b100
#define LNIC_PULL_FLAG_MASK        0b1000
#define LNIC_CHOP_FLAG_MASK        0b10000
#define LNIC_HEADER_MSG_LEN_OFFSET 5
#define LNIC_PACKET_CHOPPED_SIZE   128 // Bytes, the minimum L-NIC packet size
#define LNIC_HEADER_SIZE           30

#define QUEUE_SIZE_LOG_INTERVAL 100 // 100 cycles between log interval points
#define LOG_QUEUE_SIZE
#define LOG_EVENTS
#define LOG_ALL_PACKETS

// These are both set by command-line arguments. Don't change them here.
int HIGH_PRIORITY_OBUF_SIZE = 0;
int LOW_PRIORITY_OBUF_SIZE = 0;

// TODO: replace these port mapping hacks with a mac -> port mapping,
// could be hardcoded

BasePort * ports[NUMPORTS];
void send_with_priority(uint16_t port, switchpacket* tsp);

/* switch from input ports to output ports */
void do_fast_switching() {
#pragma omp parallel for
    for (int port = 0; port < NUMPORTS; port++) {
        ports[port]->setup_send_buf();
    }


// preprocess from raw input port to packets
#pragma omp parallel for
for (int port = 0; port < NUMPORTS; port++) {
    BasePort * current_port = ports[port];
    uint8_t * input_port_buf = current_port->current_input_buf;

    for (int tokenno = 0; tokenno < NUM_TOKENS; tokenno++) {
        if (is_valid_flit(input_port_buf, tokenno)) {
            uint64_t flit = get_flit(input_port_buf, tokenno);

            switchpacket * sp;
            if (!(current_port->input_in_progress)) {
                sp = (switchpacket*)calloc(sizeof(switchpacket), 1);
                current_port->input_in_progress = sp;

                // here is where we inject switching latency. this is min port-to-port latency
                sp->timestamp = this_iter_cycles_start + tokenno + SWITCHLATENCY;
                sp->sender = port;
            }
            sp = current_port->input_in_progress;

            sp->dat[sp->amtwritten++] = flit;
            if (is_last_flit(input_port_buf, tokenno)) {
                current_port->input_in_progress = NULL;
                if (current_port->push_input(sp)) {
                    printf("packet timestamp: %ld, len: %ld, sender: %d\n",
                            this_iter_cycles_start + tokenno,
                            sp->amtwritten, port);
                }
            }
        }
    }
}

// next do the switching. but this switching is just shuffling pointers,
// so it should be fast. it has to be serial though...

// NO PARALLEL!
// shift pointers to output queues, but in order. basically.
// until the input queues have no more complete packets
// 1) find the next switchpacket with the lowest timestamp across all the inputports
// 2) look at its mac, copy it into the right ports
//          i) if it's a broadcast: sorry, you have to make N-1 copies of it...
//          to put into the other queues

struct tspacket {
    uint64_t timestamp;
    switchpacket * switchpack;

    bool operator<(const tspacket &o) const
    {
        return timestamp > o.timestamp;
    }
};

typedef struct tspacket tspacket;


// TODO thread safe priority queue? could do in parallel?
std::priority_queue<tspacket> pqueue;

for (int i = 0; i < NUMPORTS; i++) {
    while (!(ports[i]->inputqueue.empty())) {
        switchpacket * sp = ports[i]->inputqueue.front();
        ports[i]->inputqueue.pop();
        pqueue.push( tspacket { sp->timestamp, sp });
    }
}

// next, put back into individual output queues
while (!pqueue.empty()) {
    switchpacket * tsp = pqueue.top().switchpack;
    pqueue.pop();

    struct timeval format_time;
    format_time.tv_sec = tsp->timestamp / 1000000000;
    format_time.tv_usec = (tsp->timestamp % 1000000000) / 1000;
    pcpp::RawPacket raw_packet((const uint8_t*)tsp->dat, 200*sizeof(uint64_t), format_time, false, pcpp::LINKTYPE_ETHERNET);
    pcpp::Packet parsed_packet(&raw_packet);
    pcpp::EthLayer* ethernet_layer = parsed_packet.getLayerOfType<pcpp::EthLayer>();
    pcpp::IPv4Layer* ip_layer = parsed_packet.getLayerOfType<pcpp::IPv4Layer>();
    if (ethernet_layer == NULL) {
        fprintf(stdout, "NULL ethernet layer\n");
        free(tsp);
        continue;
    }
    if (ip_layer == NULL) {
        fprintf(stdout, "NULL ip layer from %d with amtread %d and amtwritten %d\n", tsp->sender, tsp->amtread, tsp->amtwritten);
        if (ethernet_layer != NULL) {
            fprintf(stdout, "Source MAC %s, dest MAC %s\n", ethernet_layer->getSourceMac().toString().c_str(), ethernet_layer->getDestMac().toString().c_str());
        }
        for (int i = 0; i < tsp->amtwritten; i++) {
            fprintf(stdout, "%d: %#lx\n", i, __builtin_bswap64(tsp->dat[i]));
        }
        free(tsp);
        continue;
    }

// If this is a load generator, we need to do something completely different with all incoming packets.
#ifdef USE_LOAD_GEN
    load_gen_hook(tsp);
    free(tsp);
    continue;
#endif

    int flit_offset_doublebytes = (ETHER_HEADER_SIZE + IP_DST_FIELD_OFFSET + IP_SUBNET_OFFSET) / sizeof(uint16_t);
    uint16_t switching_flit = ((uint16_t*)tsp->dat)[flit_offset_doublebytes];

    uint16_t send_to_port = get_port_from_flit(switching_flit, 0);
    if (send_to_port == UNKNOWN_ADDRESS) {
        fprintf(stdout, "Packet with unknown destination address, dropping\n");
        free(tsp);
        // Do nothing for a packet with an unknown destination address
    } else if (send_to_port == BROADCAST_ADJUSTED) {
#define ADDUPLINK (NUMUPLINKS > 0 ? 1 : 0)
        // this will only send broadcasts to the first (zeroeth) uplink.
        // on a switch receiving broadcast packet from an uplink, this should
        // automatically prevent switch from sending the broadcast to any uplink
        for (int i = 0; i < NUMDOWNLINKS + ADDUPLINK; i++) {
            if (i != tsp->sender ) {
                switchpacket * tsp2 = (switchpacket*)malloc(sizeof(switchpacket));
                memcpy(tsp2, tsp, sizeof(switchpacket));
                send_with_priority(i, tsp2);
            }
        }
        free(tsp);
    } else {
        send_with_priority(send_to_port, tsp);
    }
}

#ifdef USE_LOAD_GEN
generate_load_packets();
#endif

// Log queue sizes if logging is enabled
#ifdef LOG_QUEUE_SIZE
if (this_iter_cycles_start % QUEUE_SIZE_LOG_INTERVAL == 0) {
    bool non_zero_buffer = false;
    for (int i = 0; i < NUMPORTS; i++) {
        if (ports[i]->outputqueue_high_size != 0 || ports[i]->outputqueue_low_size != 0) {
            non_zero_buffer = true;
            break;
        }
    }
    if (non_zero_buffer) {
        fprintf(stdout, "&&CSV&&QueueSize,%ld", this_iter_cycles_start);
        for (int i = 0; i < NUMPORTS; i++) {
            fprintf(stdout, ",%d,%ld,%ld", i, ports[i]->outputqueue_high_size, ports[i]->outputqueue_low_size);
        }
        fprintf(stdout, "\n");
    }
}
#endif

// finally in parallel, flush whatever we can to the output queues based on timestamp

#pragma omp parallel for
for (int port = 0; port < NUMPORTS; port++) {
    BasePort * thisport = ports[port];
    thisport->write_flits_to_output();
}

}

// Load generator specific code begin
#ifdef USE_LOAD_GEN
void print_packet(char* direction, parsed_packet_t* packet) {
    fprintf(stdout, "%s IP(src=%s, dst=%s), %s, %s, packet_len=%d\n", direction,
            packet->ip->getSrcIpAddress().toString().c_str(), packet->ip->getDstIpAddress().toString().c_str(),
            packet->lnic->toString().c_str(), packet->app->toString().c_str(), packet->tsp->amtwritten * sizeof(uint64_t));
}

bool count_start_message() {
    global_start_message_count++;
    if (strcmp(test_type, "ONE_CONTEXT_FOUR_CORES") == 0) {
        return global_start_message_count >= 4;
    } else if (strcmp(test_type, "FOUR_CONTEXTS_FOUR_CORES") == 0) {
        return global_start_message_count >= 4;
    } else if (strcmp(test_type, "TWO_CONTEXTS_FOUR_SHARED_CORES") == 0) {
        return global_start_message_count >= 8;
    } else if ((strcmp(test_type, "DIF_PRIORITY_LNIC_DRIVEN") == 0) ||
              (strcmp(test_type, "DIF_PRIORITY_TIMER_DRIVEN") == 0) ||
              (strcmp(test_type, "HIGH_PRIORITY_C1_STALL") == 0) ||
              (strcmp(test_type, "LOW_PRIORITY_C1_STALL") == 0)) {
        return global_start_message_count >= 2;
    } else {
        fprintf(stdout, "Unknown test type: %s\n", test_type);
        exit(-1);
    }
}

void log_packet_response_time(parsed_packet_t* packet) {
    // TODO: We need to print a header as well to record what the parameters for this run were.
    uint64_t service_time = be64toh(packet->app->getAppHeader()->service_time);
    uint64_t sent_time = be64toh(packet->app->getAppHeader()->sent_time);
    uint16_t src_context = be16toh(packet->lnic->getLnicHeader()->src_context);
    uint64_t recv_time = packet->tsp->timestamp; // TODO: This accounts for tokens, even though sends don't. Is that a problem?
    uint64_t iter_time = this_iter_cycles_start;
    uint64_t delta_time = (recv_time > sent_time) ? (recv_time - sent_time) : 0;
    fprintf(stdout, "&&CSV&&ResponseTimes,%ld,%ld,%ld,%ld,%ld,%d\n", service_time, delta_time, sent_time, recv_time, iter_time, src_context);
}

bool should_generate_packet_this_cycle() {
    if (!start_message_received) {
        return false;
    }
    if (this_iter_cycles_start >= next_threshold) {
        next_threshold = this_iter_cycles_start + (uint64_t)(*gen_dist)(*gen_rand);
        return true;
    }
    return false;
}

uint64_t get_service_time() {
    if (strcmp(dist_type, "FIXED") == 0) {
        return fixed_dist_cycles;
    } else if (strcmp(dist_type, "EXP") == 0) {
        double exp_value = exp_dist_scale_factor * (*service_exp_dist)(*dist_rand);
        return std::min(std::max((uint64_t)exp_value, min_service_time), max_service_time);
    } else if (strcmp(dist_type, "BIMODAL") == 0) {
        double service_low = (*service_normal_low)(*dist_rand);
        double service_high = (*service_normal_high)(*dist_rand);
        int select_high = (*service_select_dist)(*dist_rand);
        if (select_high) {
            return std::min(std::max((uint64_t)service_high, min_service_time), max_service_time);
        } else {
            return std::min(std::max((uint64_t)service_low, min_service_time), max_service_time);
        }
    } else {
        fprintf(stdout, "Unknown distribution type: %s\n", dist_type);
        exit(-1);
    }

}

uint16_t get_next_tx_msg_id() {
    uint16_t to_return = global_tx_msg_id;
    global_tx_msg_id++;
    if (global_tx_msg_id == MAX_TX_MSG_ID) {
        global_tx_msg_id = 0;
    }
    return to_return;
}

void send_load_packet(uint16_t dst_context, uint64_t service_time, uint64_t sent_time) {
    // Build the new ethernet/ip packet layers
    pcpp::EthLayer new_eth_layer(pcpp::MacAddress(LOAD_GEN_MAC), pcpp::MacAddress(NIC_MAC));
    pcpp::IPv4Layer new_ip_layer(pcpp::IPv4Address(std::string(LOAD_GEN_IP)), pcpp::IPv4Address(std::string(NIC_IP)));
    new_ip_layer.getIPv4Header()->ipId = htons(1);
    new_ip_layer.getIPv4Header()->timeToLive = 64;
    new_ip_layer.getIPv4Header()->protocol = 153; // Protocol code for LNIC
    uint64_t data_packet_size_bytes = ETHER_HEADER_SIZE + IP_HEADER_SIZE + LNIC_HEADER_SIZE + APP_HEADER_SIZE;

    // Build the new lnic and application packet layers
    pcpp::LnicLayer new_lnic_layer(0, 0, 0, 0, 0, 0, 0, 0, 0);
    new_lnic_layer.getLnicHeader()->flags = (uint8_t)LNIC_DATA_FLAG_MASK;
    new_lnic_layer.getLnicHeader()->msg_len = htons(16);
    new_lnic_layer.getLnicHeader()->src_context = htons(0);
    new_lnic_layer.getLnicHeader()->dst_context = htons(dst_context);
    new_lnic_layer.getLnicHeader()->tx_msg_id = htons(get_next_tx_msg_id());
    pcpp::AppLayer new_app_layer(service_time, sent_time);

    // Join the layers into a new packet
    pcpp::Packet new_packet(data_packet_size_bytes);
    new_packet.addLayer(&new_eth_layer);
    new_packet.addLayer(&new_ip_layer);
    new_packet.addLayer(&new_lnic_layer);
    new_packet.addLayer(&new_app_layer);
    new_packet.computeCalculateFields();

    // Convert the packet to a switchpacket
    switchpacket* new_tsp = (switchpacket*)calloc(sizeof(switchpacket), 1);
    new_tsp->timestamp = this_iter_cycles_start;
    new_tsp->amtwritten = data_packet_size_bytes / sizeof(uint64_t);
    new_tsp->amtread = 0;
    new_tsp->sender = 0;
    memcpy(new_tsp->dat, new_packet.getRawPacket()->getRawData(), data_packet_size_bytes);

    // Verify and log the switchpacket
    // TODO: For now we only work with port 0.
    parsed_packet_t sent_packet;
    if (!sent_packet.parse(new_tsp)) {
        fprintf(stdout, "Invalid generated packet.\n");
        free(new_tsp);
        return;
    }
    print_packet("LOAD", &sent_packet);
    send_with_priority(0, new_tsp);
}

void load_gen_hook(switchpacket* tsp) {
    // Parse and log the incoming packet
    parsed_packet_t packet;
    bool is_valid = packet.parse(tsp);
    if (!is_valid) {
        fprintf(stdout, "Invalid received packet.\n");
        return;
    }
    print_packet("RECV", &packet);

    // Send ACK+PULL responses to DATA packets
    // TODO: This only works for one-packet messages for now
    if (packet.lnic->getLnicHeader()->flags & LNIC_DATA_FLAG_MASK) {
        // Calculate the ACK+PULL values
        pcpp::lnichdr* lnic_hdr = packet.lnic->getLnicHeader();
        uint16_t pull_offset = lnic_hdr->pkt_offset + rtt_pkts;
        uint8_t flags = LNIC_ACK_FLAG_MASK | LNIC_PULL_FLAG_MASK;
        uint64_t ack_packet_size_bytes = ETHER_HEADER_SIZE + IP_HEADER_SIZE + LNIC_HEADER_SIZE + APP_HEADER_SIZE;

        // Build the new packet layers
        pcpp::EthLayer new_eth_layer(packet.eth->getDestMac(), packet.eth->getSourceMac());
        pcpp::IPv4Layer new_ip_layer(packet.ip->getDstIpAddress(), packet.ip->getSrcIpAddress());
        new_ip_layer.getIPv4Header()->ipId = htons(1);
        new_ip_layer.getIPv4Header()->timeToLive = 64;
        new_ip_layer.getIPv4Header()->protocol = 153; // Protocol code for LNIC
        pcpp::LnicLayer new_lnic_layer(flags, ntohs(lnic_hdr->dst_context), ntohs(lnic_hdr->src_context),
                                       ntohs(lnic_hdr->msg_len), lnic_hdr->pkt_offset, pull_offset,
                                       ntohs(lnic_hdr->tx_msg_id), ntohs(lnic_hdr->buf_ptr), lnic_hdr->buf_size_class);
        pcpp::AppLayer new_app_layer(0, 0);

        // Join the layers into a new packet
        pcpp::Packet new_packet(ack_packet_size_bytes);
        new_packet.addLayer(&new_eth_layer);
        new_packet.addLayer(&new_ip_layer);
        new_packet.addLayer(&new_lnic_layer);
        new_packet.addLayer(&new_app_layer);
        new_packet.computeCalculateFields();

        // Convert the packet to a switchpacket
        switchpacket* new_tsp = (switchpacket*)calloc(sizeof(switchpacket), 1);
        new_tsp->timestamp = tsp->timestamp;
        new_tsp->amtwritten = ack_packet_size_bytes / sizeof(uint64_t);
        new_tsp->amtread = 0;
        new_tsp->sender = 0;
        memcpy(new_tsp->dat, new_packet.getRawPacket()->getRawData(), ack_packet_size_bytes);

        // Verify and log the switchpacket
        // TODO: For now we only work with port 0.
        parsed_packet_t sent_packet;
        if (!sent_packet.parse(new_tsp)) {
            fprintf(stdout, "Invalid sent packet.\n");
            free(new_tsp);
            return;
        }
        print_packet("SEND", &sent_packet);
        send_with_priority(0, new_tsp);

        // Check for nanoPU startup messages
        if (!start_message_received) {
            if(count_start_message()) {
                start_message_received = true;
            }
        } else {
            log_packet_response_time(&packet);
        }
    }
}

// Figure out which load packets to generate.
// TODO: This should really have an enum instead of a strcmp.
void generate_load_packets() {
    if (!should_generate_packet_this_cycle()) {
        return;
    }
    uint64_t service_time = get_service_time();
    uint64_t sent_time = this_iter_cycles_start; // TODO: Check this

    if (strcmp(test_type, "ONE_CONTEXT_FOUR_CORES") == 0) {
        send_load_packet(0, service_time, sent_time);
    } else if (strcmp(test_type, "FOUR_CONTEXTS_FOUR_CORES") == 0) {
        send_load_packet(rand() % 4, service_time, sent_time);
    } else if ((strcmp(test_type, "TWO_CONTEXTS_FOUR_SHARED_CORES") == 0) ||
               (strcmp(test_type, "DIF_PRIORITY_LNIC_DRIVEN") == 0) ||
               (strcmp(test_type, "DIF_PRIORITY_TIMER_DRIVEN") == 0) ||
               (strcmp(test_type, "HIGH_PRIORITY_C1_STALL") == 0) ||
               (strcmp(test_type, "LOW_PRIORITY_C1_STALL") == 0)) {
        send_load_packet(rand() % 2, service_time, sent_time);
    } else {
        fprintf(stdout, "Unknown test type: %s\n", test_type);
        exit(-1);
    }
}

// Load generator specific code end.
#endif

void send_with_priority(uint16_t port, switchpacket* tsp) {
    uint8_t lnic_header_flags = *((uint8_t*)tsp->dat + ETHER_HEADER_SIZE + IP_HEADER_SIZE);
    bool is_data = lnic_header_flags & LNIC_DATA_FLAG_MASK;
    bool is_ack = lnic_header_flags & LNIC_ACK_FLAG_MASK;
    bool is_nack = lnic_header_flags & LNIC_NACK_FLAG_MASK;
    bool is_pull = lnic_header_flags & LNIC_PULL_FLAG_MASK;
    bool is_chop = lnic_header_flags & LNIC_CHOP_FLAG_MASK;
    uint64_t packet_size_bytes = tsp->amtwritten * sizeof(uint64_t);
    
    uint64_t lnic_msg_len_bytes_offset = (uint64_t)tsp->dat + ETHER_HEADER_SIZE + IP_HEADER_SIZE + LNIC_HEADER_MSG_LEN_OFFSET;
    uint16_t lnic_msg_len_bytes = *(uint16_t*)lnic_msg_len_bytes_offset;
    lnic_msg_len_bytes = __builtin_bswap16(lnic_msg_len_bytes);

    uint64_t lnic_src_context_offset = (uint64_t)tsp->dat + ETHER_HEADER_SIZE + IP_HEADER_SIZE + 1;
    uint64_t lnic_dst_context_offset = (uint64_t)tsp->dat + ETHER_HEADER_SIZE + IP_HEADER_SIZE + 3;
    uint16_t lnic_src_context = __builtin_bswap16(*(uint16_t*)lnic_src_context_offset);
    uint16_t lnic_dst_context = __builtin_bswap16(*(uint16_t*)lnic_dst_context_offset);

    uint64_t packet_data_size = packet_size_bytes - ETHER_HEADER_SIZE - IP_HEADER_SIZE - LNIC_HEADER_SIZE;
    uint64_t packet_msg_words_offset = (uint64_t)tsp->dat + ETHER_HEADER_SIZE + IP_HEADER_SIZE + LNIC_HEADER_SIZE;
    uint64_t* packet_msg_words = (uint64_t*)packet_msg_words_offset;

#ifdef LOG_ALL_PACKETS
    struct timeval format_time;
    format_time.tv_sec = tsp->timestamp / 1000000000;
    format_time.tv_usec = (tsp->timestamp % 1000000000) / 1000;
    pcpp::RawPacket raw_packet((const uint8_t*)tsp->dat, 200*sizeof(uint64_t), format_time, false, pcpp::LINKTYPE_ETHERNET);
    pcpp::Packet parsed_packet(&raw_packet);
    pcpp::IPv4Layer* ip_layer = parsed_packet.getLayerOfType<pcpp::IPv4Layer>();
    std::string ip_src_addr = ip_layer->getSrcIpAddress().toString();
    std::string ip_dst_addr = ip_layer->getDstIpAddress().toString();
    std::string flags_str;
    flags_str += is_data ? "DATA" : "";
    flags_str += is_ack ? " ACK" : "";
    flags_str += is_nack ? " NACK" : "";
    flags_str += is_pull ? " PULL" : "";
    flags_str += is_chop ? " CHOP" : "";
    fprintf(stdout, "IP(src=%s, dst=%s), LNIC(flags=%s, msg_len=%d, src_context=%d, dst_context=%d), packet_len=%d, port=%d\n", ip_src_addr.c_str(), ip_dst_addr.c_str(),
                     flags_str.c_str(), lnic_msg_len_bytes, lnic_src_context, lnic_dst_context, packet_size_bytes, port);
#endif LOG_ALL_PACKETS

    if (is_data && !is_chop) {
        // Regular data, send to low priority queue or chop and send to high priority
        // queue if low priority queue is full.
        if (packet_size_bytes + ports[port]->outputqueue_low_size < LOW_PRIORITY_OBUF_SIZE) {
            ports[port]->outputqueue_low.push(tsp);
            ports[port]->outputqueue_low_size += packet_size_bytes;
        } else {
            // Try to chop the packet
            if (LNIC_PACKET_CHOPPED_SIZE + ports[port]->outputqueue_high_size < HIGH_PRIORITY_OBUF_SIZE) {
#ifdef LOG_EVENTS
                fprintf(stdout, "&&CSV&&Events,Chopped,%ld,%d\n", this_iter_cycles_start, port);
#endif
                switchpacket * tsp2 = (switchpacket*)calloc(sizeof(switchpacket), 1);
                tsp2->timestamp = tsp->timestamp;
                tsp2->amtwritten = LNIC_PACKET_CHOPPED_SIZE / sizeof(uint64_t);
                tsp2->amtread = tsp->amtread;
                tsp2->sender = tsp->sender;
                memcpy(tsp2->dat, tsp->dat, ETHER_HEADER_SIZE + IP_HEADER_SIZE + LNIC_HEADER_SIZE);
                uint64_t lnic_flag_offset = (uint64_t)tsp2->dat + ETHER_HEADER_SIZE + IP_HEADER_SIZE;
                *(uint8_t*)(lnic_flag_offset) |= LNIC_CHOP_FLAG_MASK;
                free(tsp);
                ports[port]->outputqueue_high.push(tsp2);
                ports[port]->outputqueue_high_size += LNIC_PACKET_CHOPPED_SIZE;

            } else {
                // TODO: We should really drop the lowest priority packet sometimes, not always the newly arrived packet
#ifdef LOG_EVENTS
                fprintf(stdout, "&&CSV&&Events,DroppedBothFull,%ld,%d\n", this_iter_cycles_start, port);
#endif
                free(tsp);
            }
        }
    } else if ((is_data && is_chop) || (!is_data && !is_chop)) {
        // Chopped data or control, send to high priority output queue
        if (packet_size_bytes + ports[port]->outputqueue_high_size < HIGH_PRIORITY_OBUF_SIZE) {
            ports[port]->outputqueue_high.push(tsp);
            ports[port]->outputqueue_high_size += packet_size_bytes;
        } else {
#ifdef LOG_EVENTS
            fprintf(stdout, "&&CSV&&Events,DroppedControlFull,%ld,%d\n", this_iter_cycles_start, port);
#endif
            free(tsp);
        }
    } else {
        fprintf(stdout, "Invalid combination: Chopped control packet. Dropping.\n");
        free(tsp);
        // Chopped control packet. This shouldn't be possible.
    }
}

static void simplify_frac(int n, int d, int *nn, int *dd)
{
    int a = n, b = d;

    // compute GCD
    while (b > 0) {
        int t = b;
        b = a % b;
        a = t;
    }

    *nn = n / a;
    *dd = d / a;
}

int main (int argc, char *argv[]) {
    int bandwidth;

    if (argc < 6) {
        // if insufficient args, error out
        fprintf(stdout, "usage: ./switch LINKLATENCY SWITCHLATENCY BANDWIDTH HIGH_PRIORITY_OBUF_SIZE LOW_PRIORITY_OBUF_SIZE\n");
        fprintf(stdout, "insufficient args provided\n.");
        fprintf(stdout, "LINKLATENCY and SWITCHLATENCY should be provided in cycles.\n");
        fprintf(stdout, "BANDWIDTH should be provided in Gbps\n");
        fprintf(stdout, "OBUF SIZES should be provided in bytes.\n");
        exit(1);
    }

    LINKLATENCY = atoi(argv[1]);
    switchlat = atoi(argv[2]);
    bandwidth = atoi(argv[3]);
    HIGH_PRIORITY_OBUF_SIZE = atoi(argv[4]);
    LOW_PRIORITY_OBUF_SIZE = atoi(argv[5]);

    POISSON_LAMBDA = 1.0 / (double)atoi(argv[8]);

#ifdef USE_LOAD_GEN
    double request_rate_lambda = 1.0 / (double)request_rate_lambda_inverse;
    gen_rand = new std::default_random_engine;
    gen_dist = new std::exponential_distribution<double>(request_rate_lambda);
    dist_rand = new std::default_random_engine;
    service_exp_dist = new std::exponential_distribution<double>(exp_dist_decay_const);
    service_normal_high = new std::normal_distribution<double>(bimodal_dist_high_mean, bimodal_dist_high_stdev);
    service_normal_low = new std::normal_distribution<double>(bimodal_dist_low_mean, bimodal_dist_low_stdev);
    service_select_dist = new std::binomial_distribution<int>(1, bimodal_dist_fraction_high);
#endif

    simplify_frac(bandwidth, 200, &throttle_numer, &throttle_denom);

    fprintf(stdout, "Using link latency: %d\n", LINKLATENCY);
    fprintf(stdout, "Using switching latency: %d\n", SWITCHLATENCY);
    fprintf(stdout, "BW throttle set to %d/%d\n", throttle_numer, throttle_denom);
    fprintf(stdout, "High priority obuf size: %d\n", HIGH_PRIORITY_OBUF_SIZE);
    fprintf(stdout, "Low priority obuf size: %d\n", LOW_PRIORITY_OBUF_SIZE);

    if ((LINKLATENCY % 7) != 0) {
        // if invalid link latency, error out.
        fprintf(stdout, "INVALID LINKLATENCY. Currently must be multiple of 7 cycles.\n");
        exit(1);
    }

    omp_set_num_threads(NUMPORTS); // we parallelize over ports, so max threads = # ports

#define PORTSETUPCONFIG
#include "switchconfig.h"
#undef PORTSETUPCONFIG

    while (true) {

        // handle sends
#pragma omp parallel for
        for (int port = 0; port < NUMPORTS; port++) {
            ports[port]->send();
        }

        // handle receives. these are blocking per port
#pragma omp parallel for
        for (int port = 0; port < NUMPORTS; port++) {
            ports[port]->recv();
        }
 
#pragma omp parallel for
        for (int port = 0; port < NUMPORTS; port++) {
            ports[port]->tick_pre();
        }

        do_fast_switching();

        this_iter_cycles_start += LINKLATENCY; // keep track of time

        // some ports need to handle extra stuff after each iteration
        // e.g. shmem ports swapping shared buffers
#pragma omp parallel for
        for (int port = 0; port < NUMPORTS; port++) {
            ports[port]->tick();
        }

    }
}
