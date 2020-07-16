//See LICENSE for license details
#ifdef LNICBRIDGEMODULE_struct_guard

#include "lnic.h"

#include <stdio.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/mman.h>

// DO NOT MODIFY PARAMS BELOW THIS LINE
#define TOKENS_PER_BIGTOKEN 7

#define SIMLATENCY_BT (this->LINKLATENCY/TOKENS_PER_BIGTOKEN)

#define BUFWIDTH (512/8)
#define BUFBYTES (SIMLATENCY_BT*BUFWIDTH)
#define EXTRABYTES 1

#define FLIT_BITS 64
#define PACKET_MAX_FLITS 190
#define BITTIME_PER_QUANTA 512
#define CYCLES_PER_QUANTA (BITTIME_PER_QUANTA / FLIT_BITS)

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

#define niclog_printf(...) if (this->niclog) { fprintf(this->niclog, __VA_ARGS__); fflush(this->niclog); }

lnic_t::lnic_t(simif_t *sim, std::vector<std::string> &args,
        LNICBRIDGEMODULE_struct *mmio_addrs, int lnicno,
        long dma_addr): bridge_driver_t(sim)
{
    this->mmio_addrs = mmio_addrs;

    const char *niclogfile = NULL;
    const char *shmemportname = NULL;
    int netbw = MAX_BANDWIDTH, netburst = 8;

    this->loopback = false;
    this->niclog = NULL;

    this->nic_mac_lendian = 0;
    this->switch_mac_lendian = 0;
    this->nic_ip_lendian = 0;
    this->timeout_cycles_lendian = 0;
    this->rtt_pkts_lendian = 0;

    this->LINKLATENCY = 0;
    this->dma_addr = dma_addr;


    // construct arg parsing strings here. We basically append the bridge_driver
    // number to each of these base strings, to get args like +blkdev0 etc.
    std::string num_equals = std::to_string(lnicno) + std::string("=");
    std::string niclog_arg = std::string("+niclog") + num_equals;
    std::string nicloopback_arg = std::string("+nic-loopback") + std::to_string(lnicno);

    std::string nic_mac_addr_arg = std::string("+nic_mac_addr") + num_equals;
    std::string switch_mac_addr_arg = std::string("+switch_mac_addr") + num_equals;
    std::string nic_ip_addr_arg = std::string("+nic_ip_addr") + num_equals;
    std::string timeout_cycles_arg = std::string("+timeout_cycles") + num_equals;
    std::string rtt_pkts_arg = std::string("+rtt_pkts") + num_equals;

    std::string netbw_arg = std::string("+netbw") + num_equals;
    std::string netburst_arg = std::string("+netburst") + num_equals;
    std::string linklatency_arg = std::string("+linklatency") + num_equals;
    std::string shmemportname_arg = std::string("+shmemportname") + num_equals;


    for (auto &arg: args) {
        if (arg.find(niclog_arg) == 0) {
            niclogfile = const_cast<char*>(arg.c_str()) + niclog_arg.length();
        }
        if (arg.find(nicloopback_arg) == 0) {
            this->loopback = true;
        }
        if (arg.find(nic_mac_addr_arg) == 0) {
            uint8_t mac_bytes[6];
            int mac_octets[6];
            char * macstring = NULL;
            macstring = const_cast<char*>(arg.c_str()) + nic_mac_addr_arg.length();
            char * trailingjunk;

            // convert mac address from string to 48 bit int
            if (6 == sscanf(macstring, "%x:%x:%x:%x:%x:%x%c",
                        &mac_octets[0], &mac_octets[1], &mac_octets[2],
                        &mac_octets[3], &mac_octets[4], &mac_octets[5],
                        trailingjunk)) {

                for (int i = 0; i < 6; i++) {
                    nic_mac_lendian |= (((uint64_t)(uint8_t)mac_octets[i]) << (8*i));
                }
            } else {
                fprintf(stderr, "INVALID NIC MAC ADDRESS SUPPLIED WITH +nic_mac_addrN=\n");
            }
        }
        if (arg.find(switch_mac_addr_arg) == 0) {
            uint8_t mac_bytes[6];
            int mac_octets[6];
            char * macstring = NULL;
            macstring = const_cast<char*>(arg.c_str()) + switch_mac_addr_arg.length();
            char * trailingjunk;

            // convert mac address from string to 48 bit int
            if (6 == sscanf(macstring, "%x:%x:%x:%x:%x:%x%c",
                        &mac_octets[0], &mac_octets[1], &mac_octets[2],
                        &mac_octets[3], &mac_octets[4], &mac_octets[5],
                        trailingjunk)) {

                for (int i = 0; i < 6; i++) {
                    switch_mac_lendian |= (((uint64_t)(uint8_t)mac_octets[i]) << (8*i));
                }
            } else {
                fprintf(stderr, "INVALID SWITCH MAC ADDRESS SUPPLIED WITH +switch_mac_addrN=\n");
            }
        }
        if (arg.find(nic_ip_addr_arg) == 0) {
            char * ipstring = NULL;
            int ip_octets[4];
            ipstring = const_cast<char*>(arg.c_str()) + nic_ip_addr_arg.length();
            char * trailingjunk;
            if (4 == sscanf(ipstring, "%d.%d.%d.%d%c",
                        &ip_octets[0], &ip_octets[1], &ip_octets[2], &ip_octets[3],
                        trailingjunk)) {
                for (int i = 0; i < 4; i++) {
                    nic_ip_lendian |= (uint32_t)(ip_octets[i] << (8*i));
                }
            } else {
                fprintf(stderr, "INVALID NIC IP ADDRESS SUPPLIED WITH +nic_ip_addrN=\n");
            }
        }
        if (arg.find(timeout_cycles_arg) == 0) {
            char *str = const_cast<char*>(arg.c_str()) + timeout_cycles_arg.length();
            this->timeout_cycles_lendian = atoll(str);
        }
        if (arg.find(rtt_pkts_arg) == 0) {
            char* str = const_cast<char*>(arg.c_str()) + rtt_pkts_arg.length();
            this->rtt_pkts_lendian = atoi(str);
        }
        if (arg.find(netbw_arg) == 0) {
            char *str = const_cast<char*>(arg.c_str()) + netbw_arg.length();
            netbw = atoi(str);
        }
        if (arg.find(netburst_arg) == 0) {
            char *str = const_cast<char*>(arg.c_str()) + netburst_arg.length();
            netburst = atoi(str);
        }
        if (arg.find(linklatency_arg) == 0) {
            char *str = const_cast<char*>(arg.c_str()) + linklatency_arg.length();
            this->LINKLATENCY = atoi(str);
        }
        if (arg.find(shmemportname_arg) == 0) {
            shmemportname = const_cast<char*>(arg.c_str()) + shmemportname_arg.length();
        }
    }

    assert(this->LINKLATENCY > 0);
    assert(netbw <= MAX_BANDWIDTH);
    assert(netburst < 256);
    simplify_frac(netbw, MAX_BANDWIDTH, &rlimit_inc, &rlimit_period);
    rlimit_size = netburst;
    pause_threshold = PACKET_MAX_FLITS + this->LINKLATENCY;
    pause_quanta = pause_threshold / CYCLES_PER_QUANTA;
    pause_refresh = this->LINKLATENCY;

    printf("using link latency: %d cycles\n", this->LINKLATENCY);
    printf("using netbw: %d\n", netbw);
    printf("using netburst: %d\n", netburst);

    if (niclogfile) {
        this->niclog = fopen(niclogfile, "w");
        if (!this->niclog) {
            fprintf(stderr, "Could not open NIC log file: %s\n", niclogfile);
            abort();
        }
    }

    char name[257];
    int shmemfd;

    if (!loopback) {
        assert(shmemportname != NULL);
        for (int j = 0; j < 2; j++) {
            printf("Using non-slot-id associated shmemportname:\n");
            sprintf(name, "/port_nts%s_%d", shmemportname, j);

            printf("opening/creating shmem region\n%s\n", name);
            shmemfd = shm_open(name, O_RDWR | O_CREAT , S_IRWXU);
            ftruncate(shmemfd, BUFBYTES+EXTRABYTES);
            pcis_read_bufs[j] = (char*)mmap(NULL, BUFBYTES+EXTRABYTES, PROT_READ | PROT_WRITE, MAP_SHARED, shmemfd, 0);

            printf("Using non-slot-id associated shmemportname:\n");
            sprintf(name, "/port_stn%s_%d", shmemportname, j);

            printf("opening/creating shmem region\n%s\n", name);
            shmemfd = shm_open(name, O_RDWR | O_CREAT , S_IRWXU);
            ftruncate(shmemfd, BUFBYTES+EXTRABYTES);
            pcis_write_bufs[j] = (char*)mmap(NULL, BUFBYTES+EXTRABYTES, PROT_READ | PROT_WRITE, MAP_SHARED, shmemfd, 0);
        }
    } else {
        for (int j = 0; j < 2; j++) {
            pcis_read_bufs[j] = (char *) malloc(BUFBYTES + EXTRABYTES);
            pcis_write_bufs[j] = pcis_read_bufs[j];
        }
    }
}

lnic_t::~lnic_t() {
    if (this->niclog)
        fclose(this->niclog);
    if (loopback) {
        for (int j = 0; j < 2; j++)
            free(pcis_read_bufs[j]);
    } else {
        for (int j = 0; j < 2; j++) {
            munmap(pcis_read_bufs[j], BUFBYTES+EXTRABYTES);
            munmap(pcis_write_bufs[j], BUFBYTES+EXTRABYTES);
        }
    }
    free(this->mmio_addrs);
}

#define ceil_div(n, d) (((n) - 1) / (d) + 1)

void lnic_t::init() {
    // Old bridge setup enters addr's as little-endian (convenient for RISC-V)
    // We want them as big-endian (convenient for network)
    uint64_t nic_mac_bigendian = __builtin_bswap64(nic_mac_lendian) >> 16;
    uint64_t switch_mac_bigendian = __builtin_bswap64(switch_mac_lendian) >> 16;
    uint32_t nic_ip_bigendian = __builtin_bswap32(nic_ip_lendian);
    uint64_t timeout_cycles_bigendian = __builtin_bswap64(timeout_cycles_lendian);
    uint16_t rtt_pkts_bigendian = __builtin_bswap16(rtt_pkts_lendian);
    printf("Writing switch mac old %#lx new %#lx\n", switch_mac_lendian, switch_mac_bigendian);

    write(mmio_addrs->nic_mac_addr_upper, (nic_mac_bigendian >> 32) & 0xFFFFFFFF);
    write(mmio_addrs->nic_mac_addr_lower, nic_mac_bigendian & 0xFFFFFFFF);
    write(mmio_addrs->switch_mac_addr_upper, (switch_mac_bigendian >> 32) & 0xFFFFFFFF);
    write(mmio_addrs->switch_mac_addr_lower, switch_mac_bigendian & 0xFFFFFFFF);
    write(mmio_addrs->nic_ip_addr, nic_ip_bigendian);
    write(mmio_addrs->timeout_cycles_upper, (timeout_cycles_bigendian >> 32) & 0xFFFFFFFF);
    write(mmio_addrs->timeout_cycles_lower, timeout_cycles_bigendian & 0xFFFFFFFF);
    write(mmio_addrs->rtt_pkts, rtt_pkts_bigendian);

    uint32_t output_tokens_available = read(mmio_addrs->outgoing_count);
    uint32_t input_token_capacity = SIMLATENCY_BT - read(mmio_addrs->incoming_count);
    if ((input_token_capacity != SIMLATENCY_BT) || (output_tokens_available != 0)) {
        printf("FAIL. INCORRECT TOKENS ON BOOT. produced tokens available %d, input slots available %d\n", output_tokens_available, input_token_capacity);
        exit(1);
    }

    printf("On init, %d token slots available on input.\n", input_token_capacity);
    uint32_t token_bytes_produced = 0;
    token_bytes_produced = push(
            dma_addr,
            pcis_write_bufs[1],
            BUFWIDTH*input_token_capacity);
    if (token_bytes_produced != input_token_capacity*BUFWIDTH) {
        printf("ERR MISMATCH!\n");
        exit(1);
    }
    return;
}

//#define TOKENVERIFY

void lnic_t::tick() {
    struct timespec tstart, tend;

    //#define DEBUG_NIC_PRINT

    while (true) { // break when we don't have 5k tokens
        uint32_t tokens_this_round = 0;

        uint32_t output_tokens_available = read(mmio_addrs->outgoing_count);
        uint32_t input_token_capacity = SIMLATENCY_BT - read(mmio_addrs->incoming_count);

        // we will read/write the min of tokens available / token input capacity
        tokens_this_round = std::min(output_tokens_available, input_token_capacity);
#ifdef DEBUG_NIC_PRINT
        niclog_printf("tokens this round: %d\n", tokens_this_round);
#endif

        if (tokens_this_round != SIMLATENCY_BT) {
#ifdef DEBUG_NIC_PRINT
            niclog_printf("FAIL: output available %d, input capacity: %d\n", output_tokens_available, input_token_capacity);
#endif
            return;
        }

        // read into read_buffer
#ifdef DEBUG_NIC_PRINT
        iter++;
        niclog_printf("read fpga iter %ld\n", iter);
#endif
        uint32_t token_bytes_obtained_from_fpga = 0;
        token_bytes_obtained_from_fpga = pull(
                dma_addr,
                pcis_read_bufs[currentround],
                BUFWIDTH * tokens_this_round);
#ifdef DEBUG_NIC_PRINT
        niclog_printf("send iter %ld\n", iter);
#endif

        pcis_read_bufs[currentround][BUFBYTES] = 1;

#ifdef TOKENVERIFY
        // the widget is designed to tag tokens with a 43 bit number,
        // incrementing for each sent token. verify that we are not losing
        // tokens over PCIS
        for (int i = 0; i < tokens_this_round; i++) {
            uint64_t TOKENLRV_AND_COUNT = *(((uint64_t*)pcis_read_bufs[currentround])+i*8);
            uint8_t LAST;
            for (int token_in_bigtoken = 0; token_in_bigtoken < 7; token_in_bigtoken++) {
                if (TOKENLRV_AND_COUNT & (1L << (43+token_in_bigtoken*3))) {
                    LAST = (TOKENLRV_AND_COUNT >> (45 + token_in_bigtoken*3)) & 0x1;
                    niclog_printf("sending to other node, valid data chunk: "
                                "%016lx, last %x, sendcycle: %016ld\n",
                                *((((uint64_t*)pcis_read_bufs[currentround])+i*8)+1+token_in_bigtoken),
                                LAST, timeelapsed_cycles + i*7 + token_in_bigtoken);
                }
            }

            //            *((uint64_t*)(pcis_read_buf + i*64)) |= 0x4924900000000000;
            uint32_t thistoken = *((uint32_t*)(pcis_read_bufs[currentround] + i*64));
            if (thistoken != next_token_from_fpga) {
                niclog_printf("FAIL! Token lost on FPGA interface.\n");
                exit(1);
            }
            next_token_from_fpga++;
        }
#endif
        if (token_bytes_obtained_from_fpga != tokens_this_round * BUFWIDTH) {
            printf("ERR MISMATCH! on reading tokens out. actually read %d bytes, wanted %d bytes.\n", token_bytes_obtained_from_fpga, BUFWIDTH * tokens_this_round);
            printf("errno: %s\n", strerror(errno));
            exit(1);
        }

#ifdef DEBUG_NIC_PRINT
        niclog_printf("recv iter %ld\n", iter);
#endif

#ifdef TOKENVERIFY
        timeelapsed_cycles += LINKLATENCY;
#endif

        if (!loopback) {
            volatile uint8_t * polladdr = (uint8_t*)(pcis_write_bufs[currentround] + BUFBYTES);
            while (*polladdr == 0) { ; }
        }
#ifdef DEBUG_NIC_PRINT
        niclog_printf("done recv iter %ld\n", iter);
#endif

#ifdef TOKENVERIFY
        // this does not do tokenverify - it's just printing tokens
        // there should not be tokenverify on this interface
        for (int i = 0; i < tokens_this_round; i++) {
            uint64_t TOKENLRV_AND_COUNT = *(((uint64_t*)pcis_write_bufs[currentround])+i*8);
            uint8_t LAST;
            for (int token_in_bigtoken = 0; token_in_bigtoken < 7; token_in_bigtoken++) {
                if (TOKENLRV_AND_COUNT & (1L << (43+token_in_bigtoken*3))) {
                    LAST = (TOKENLRV_AND_COUNT >> (45 + token_in_bigtoken*3)) & 0x1;
                    niclog_printf("from other node, valid data chunk: %016lx, "
                                "last %x, recvcycle: %016ld\n",
                                *((((uint64_t*)pcis_write_bufs[currentround])+i*8)+1+token_in_bigtoken),
                                LAST, timeelapsed_cycles + i*7 + token_in_bigtoken);
                }
            }
        }
#endif
        uint32_t token_bytes_sent_to_fpga = 0;
        token_bytes_sent_to_fpga = push(
                dma_addr,
                pcis_write_bufs[currentround],
                BUFWIDTH * tokens_this_round);
        pcis_write_bufs[currentround][BUFBYTES] = 0;
        if (token_bytes_sent_to_fpga != tokens_this_round * BUFWIDTH) {
            printf("ERR MISMATCH! on writing tokens in. actually wrote in %d bytes, wanted %d bytes.\n", token_bytes_sent_to_fpga, BUFWIDTH * tokens_this_round);
            printf("errno: %s\n", strerror(errno));
            exit(1);
        }

        currentround = (currentround + 1) % 2;
    }
}

#endif // #ifdef LNICBRIDGEMODULE_struct_guard

