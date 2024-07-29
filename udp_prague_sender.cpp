// udp_p                                                                        rague_sender.cpp:
// An example of a (dummy data) UDP sender that needs to receive ACKs from a UDP receiver for congestion control
//

#include "prague_cc.h"
#include "udpsocket.h"

#define BUFFER_SIZE 8192       // in bytes (depending on MTU)
#define ECN_MASK ecn_ce

#pragma pack(push, 1)
struct datamessage_t {
    time_tp timestamp;         // timestamp from peer, freeze and keep this time
    time_tp echoed_timestamp;  // echoed_timestamp can be used to calculate the RTT
    count_tp seq_nr;           // packet sequence number, should start with 1 and increase monotonic with packets sent

    void hton() {              // swap the bytes if needed
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        seq_nr = htonl(seq_nr);
    }
};

struct ackmessage_t {
    time_tp timestamp;         // timestamp from peer, freeze and keep this time
    time_tp echoed_timestamp;  // echoed_timestamp can be used to calculate the RTT
    count_tp packets_received; // echoed_packet counter
    count_tp packets_CE;       // echoed CE counter
    count_tp packets_lost;     // echoed lost counter
    bool error_L4S;            // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!

    void hton() {              // swap the bytes if needed
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        packets_received = htonl(packets_received);
        packets_CE = htonl(packets_CE);
        packets_lost = htonl(packets_lost);
    }
        };
#pragma pack(pop)

int main(int argc, char **argv)
{
    // Argument parser
    bool verbose = false;
    bool quiet = false;
    const char *rcv_addr = "127.0.0.1";
    uint32_t rcv_port = 8080;
    size_tp max_pkt = 1400;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-a" && i + 1 < argc) {
            rcv_addr = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            rcv_port = atoi(argv[++i]);
        } else if (arg == "-m" && i + 1 < argc) {
            max_pkt = atoi(argv[++i]);
        } else if (arg == "-v") {
            verbose = true;
            quiet = true;
        } else if (arg == "-q") {
            quiet = true;
        } else {
            perror("Usage: udp_prague_receiver -a <receiver address, def: 127.0.0.1> -p <receiver port, def:8080> -m <max packet length> -v (for verbose prints) -q (quiet)");;
            return 1;
        }
    }
    // Create a UDP socket
    UDPSocket us;
    us.Connect(rcv_addr, rcv_port);

    char receivebuffer[BUFFER_SIZE];
    uint32_t sendbuffer[BUFFER_SIZE/4];
    // init payload with dummy data
    for (int i = 0; i < BUFFER_SIZE/4; i++)
        sendbuffer[i] = htonl(i);

    struct ackmessage_t& ack_msg = (struct ackmessage_t&)(receivebuffer);  // overlaying the receive buffer
    struct datamessage_t& data_msg = (struct datamessage_t&)(sendbuffer);  // overlaying the send buffer

    printf("UDP Prague sender sending to %s on port %d with max packet size %lld bytes.\n", rcv_addr, rcv_port, max_pkt);

    // create a PragueCC object. Using default parameters for the Prague CC in line with TCP_Prague
    PragueCC pragueCC(max_pkt);
    // outside PragueCC CC-loop state
    time_tp now = pragueCC.Now();
    time_tp nextSend = now;
    count_tp seqnr = 0;
    count_tp inflight = 0;
    rate_tp pacing_rate;
    count_tp packet_window;
    count_tp packet_burst;
    size_tp packet_size;
    // state for verbose reporting
    time_tp send_tm = now;      // diff reference
    // state for default (non-quiet) reporting
    time_tp rept_tm = now + 1000000;      // timer for reporting interval
    rate_tp accbytes_sent = 0;  // accumulative bytes sent
    rate_tp acc_rtts_data = 0;  // accumulative rtts to calculate the average
    count_tp count_rtts = 0;    // count the RTT reports
    count_tp prev_packets = 0;  // prev packets received
    count_tp prev_marks = 0;    // prev marks received
    count_tp prev_losts = 0;    // prev losts received

    // get initial CC state
    pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
    if (verbose) {
        printf("s: time, timestamp, echoed_timestamp, time_diff, pacing_rate, packet_window, packet_burst, packet_size, seqnr, inflight, inburst, nextSend\n");
        printf("r: time, timestamp, echoed_timestamp, packets_received, packets_CE, packets_lost, seqnr, error_L4S, inflight\n");
    }
    while (true) {
        count_tp inburst = 0;
        time_tp timeout = 0;
        time_tp startSend = 0;
        time_tp now = pragueCC.Now();
        while ((inflight < packet_window) && (inburst < packet_burst) && (nextSend - now <= 0)) {
            ecn_tp new_ecn;
            pragueCC.GetTimeInfo(data_msg.timestamp, data_msg.echoed_timestamp, new_ecn);
            if (startSend == 0)
                startSend = now;
            data_msg.seq_nr = ++seqnr;
            if (verbose) {
                printf("s: %d, %d, %d, %d, %lld, %d, %d, %lld, %d, %d, %d, %d\n",
                       now, data_msg.timestamp, data_msg.echoed_timestamp, data_msg.timestamp - send_tm, pacing_rate, packet_window, packet_burst, packet_size, seqnr, inflight, inburst, nextSend);
                send_tm = data_msg.timestamp;
            }
            data_msg.hton();
            size_tp bytes_sent = us.Send((char*)(&data_msg), packet_size, new_ecn);
            if (((size_tp) bytes_sent) != packet_size) {
                perror("invalid data packet length sent");
                exit(1);
            }
            if (!quiet) accbytes_sent += bytes_sent;
            inburst++;
            inflight++;
        }
        if (startSend != 0) {
            time_tp prev_sn = nextSend;
            nextSend = time_tp(startSend + packet_size * inburst * 1000000 / pacing_rate);
        }
        time_tp waitTimeout = 0;
        now = pragueCC.Now();
        if (inflight < packet_window)
            waitTimeout = nextSend;
        else
            waitTimeout = now + 1000000;
        ecn_tp rcv_ecn;
        size_tp bytes_received = 0;
        do {
            timeout = (waitTimeout - now > 0) ? (waitTimeout - now) : 1;
            SOCKADDR_IN src_addr;
            socklen_t src_len = sizeof(src_addr);

            bytes_received = us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, timeout);
            now = pragueCC.Now();
        } while ((waitTimeout - now > 0) && (bytes_received == 0));
        if (bytes_received >= ssize_t(sizeof(ack_msg))) {
            ack_msg.hton();
            pragueCC.PacketReceived(ack_msg.timestamp, ack_msg.echoed_timestamp);
            pragueCC.ACKReceived(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, seqnr, ack_msg.error_L4S, inflight);
            if (!quiet) {
                acc_rtts_data += (now - ack_msg.echoed_timestamp);
                count_rtts++;
                // Display sender side info
                if (now - rept_tm >= 0) {
                    float rate_send = 8.0f * accbytes_sent / (now - rept_tm + 1000000);
                    float rtts_data = (count_rtts > 0) ? 0.001f * acc_rtts_data / count_rtts : 0.0f;
                    float mark_prob = (ack_msg.packets_received - prev_packets > 0) ?
                        100.0f * (ack_msg.packets_CE - prev_marks) / (ack_msg.packets_received - prev_packets) : 0.0f;
                    float loss_prob = (ack_msg.packets_received - prev_packets > 0) ?
                        100.0f*(ack_msg.packets_lost - prev_losts) / (ack_msg.packets_received - prev_packets) : 0.0f;
                    printf("[SENDER]: %.2f sec, %.3f Mbps, Data RTT: %.3f ms, Mark: %.2f%%(%d/%d), Lost: %.2f%%(%d/%d)\n",
                        now / 1000000.0f, rate_send, rtts_data,
                        mark_prob, ack_msg.packets_CE - prev_marks, ack_msg.packets_received - prev_packets,
                        loss_prob, ack_msg.packets_lost - prev_losts, ack_msg.packets_received - prev_packets);
                    rept_tm = now + 1000000;
                    accbytes_sent = 0;
                    acc_rtts_data = 0;
                    count_rtts = 0;
                    prev_packets = ack_msg.packets_received;
                    prev_marks = ack_msg.packets_CE;
                    prev_losts = ack_msg.packets_lost;
                }
            }
            if (verbose)
                printf("r: %d, %d, %d, %d, %d, %d, %d, %d, %d\n",
                           now, ack_msg.timestamp, ack_msg.echoed_timestamp, ack_msg.packets_received,
                           ack_msg.packets_CE, ack_msg.packets_lost, seqnr, ack_msg.error_L4S, inflight);
        }
        else // timeout, reset state
            if (inflight >= packet_window) {
                pragueCC.ResetCCInfo();
                inflight = 0;
            }
        pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
    }
}
