// udp_prague_sender.cpp:
// An example of a (dummy data) UDP sender that needs to receive ACKs from a UDP receiver for congestion control
//

#include <string>
#include "prague_cc.h"
#include "udpsocket.h"
//#include "icmpsocket.h" TODO: optimize MTU detection

// in bytes (depending on MTU):
#define BUFFER_SIZE 8192
// to avoid int64 printf incompatibility between platforms:
#define C_STR(i) std::to_string(i).c_str()

// app related stuff collected in this object to avoid obfuscation of the main Prague loop
struct AppStuff {
    // Argument parser
    bool verbose;
    bool quiet;
    const char *rcv_addr;
    uint32_t rcv_port;
    size_tp max_pkt;
    // state for verbose reporting
    time_tp send_tm;        // send diff reference
    time_tp ack_tm;         // ack diff reference
    // state for default (non-quiet) reporting
    time_tp rept_tm;        // timer for reporting interval
    rate_tp accbytes_sent;  // accumulative bytes sent
    rate_tp acc_rtts_data;  // accumulative rtts to calculate the average
    count_tp count_rtts;    // count the RTT reports
    count_tp prev_packets;  // prev packets received
    count_tp prev_marks;    // prev marks received
    count_tp prev_losts;    // prev losts received
    AppStuff(int argc, char **argv):
        verbose(false), quiet(false), rcv_addr("127.0.0.1"), rcv_port(8080), max_pkt(1500), send_tm(1), ack_tm(1), rept_tm(1000000),
        accbytes_sent(0), acc_rtts_data(0), count_rtts(0), prev_packets(0), prev_marks(0), prev_losts(0)
    {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "-a" && i + 1 < argc) {
                rcv_addr = argv[++i];
            }
            else if (arg == "-p" && i + 1 < argc) {
                rcv_port = atoi(argv[++i]);
            }
            else if (arg == "-m" && i + 1 < argc) {
                max_pkt = atoi(argv[++i]);
            }
            else if (arg == "-v") {
                verbose = true;
                quiet = true;
            }
            else if (arg == "-q") {
                quiet = true;
            }
            else {
                perror("Usage: udp_prague_receiver -a <receiver address, def: 127.0.0.1> -p <receiver port, def:8080> -m <max packet length> -v (for verbose prints) -q (quiet)");
                exit(1);
            }
        }
        printf("UDP Prague sender sending to %s on port %d with max packet size %s bytes.\n", rcv_addr, rcv_port, C_STR(max_pkt));
        if (verbose) {
            printf("s: time, timestamp, echoed_timestamp, time_diff, seqnr,,,,, pacing_rate, packet_window, packet_burst, packet_size, inflight, inburst, nextSend\n");
            printf("r: time, timestamp, echoed_timestamp, time_diff, seqnr, packets_received, packets_CE, packets_lost, error_L4S,,,,, inflight, inburst, nextSend\n");
        }
    }
    void LogSend(time_tp now, time_tp timestamp, time_tp echoed_timestamp, rate_tp pacing_rate, count_tp packet_window, count_tp packet_burst,
        size_tp packet_size, count_tp seqnr, count_tp inflight, count_tp inburst, time_tp nextSend)
    {
        if (verbose) {
            printf("s: %d, %d, %d, %d, %d,,,,, %s, %d, %d, %s, %d, %d, %d\n",
                now, timestamp, echoed_timestamp, timestamp - send_tm, seqnr, C_STR(pacing_rate),
                packet_window, packet_burst, C_STR(packet_size), inflight, inburst, nextSend - now);
            send_tm = timestamp;
        }
        if (!quiet) accbytes_sent += packet_size;
    }
    void LogRecv(time_tp now, time_tp timestamp, time_tp echoed_timestamp,
        count_tp packets_received, count_tp packets_CE, count_tp packets_lost, bool error_L4S,
        rate_tp pacing_rate, count_tp seqnr, count_tp packet_window, count_tp packet_burst, count_tp inflight, count_tp inburst, time_tp nextSend)
    {
        if (verbose) {
            printf("r: %d, %d, %d, %d, %d, %d, %d, %d, %d,,,,, %d, %d, %d\n",
                now, timestamp, echoed_timestamp, timestamp - ack_tm, seqnr, packets_received, packets_CE, packets_lost,
                error_L4S, inflight, inburst, nextSend - now);
            ack_tm = timestamp;
        }
        if (!quiet) {
            acc_rtts_data += (now - echoed_timestamp);
            count_rtts++;
            // Display sender side info
            if (now - rept_tm >= 0) {
                float rate_send = 8.0f * accbytes_sent / (now - rept_tm + 1000000);
                float rate_pacing = 8.0f * pacing_rate / 1000000.0;
                float rtts_data = (count_rtts > 0) ? 0.001f * acc_rtts_data / count_rtts : 0.0f;
                float mark_prob = (packets_received - prev_packets > 0) ?
                    100.0f * (packets_CE - prev_marks) / (packets_received - prev_packets) : 0.0f;
                float loss_prob = (packets_received - prev_packets > 0) ?
                    100.0f*(packets_lost - prev_losts) / (packets_received - prev_packets) : 0.0f;
                printf("[SENDER]: %.2f sec, %.3f Mbps, Data RTT: %.3f ms, Mark: %.2f%%(%d/%d), Lost: %.2f%%(%d/%d), Pacing rate: %f Mbps, InFlight/W: %d/%d packets, InBurst/B: %d/%d packets\n",
                    now / 1000000.0f, rate_send, rtts_data,
                    mark_prob, packets_CE - prev_marks, packets_received - prev_packets,
                    loss_prob, packets_lost - prev_losts, packets_received - prev_packets, rate_pacing, inflight, packet_window, inburst, packet_burst);
                rept_tm = now + 1000000;
                accbytes_sent = 0;
                acc_rtts_data = 0;
                count_rtts = 0;
                prev_packets = packets_received;
                prev_marks = packets_CE;
                prev_losts = packets_lost;
            }
        }
    }
    void ExitIf(bool stop, const char* reason)
    {
        if (stop) {
            perror(reason);
            exit(1);
        }
    }
};

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
    AppStuff app(argc, argv); // initialize the app

    // Find maximum MTU can be used
    // ICMPSocket icmps(rcv_addr);
    // max_pkt = icmps.mtu_discovery(150, max_pkt, 1000000, 1);

    // Create a UDP socket
    UDPSocket us;
    us.Connect(app.rcv_addr, app.rcv_port);

    char receivebuffer[BUFFER_SIZE];
    uint32_t sendbuffer[BUFFER_SIZE/4];
    // init payload with dummy data
    for (int i = 0; i < BUFFER_SIZE/4; i++)
        sendbuffer[i] = htonl(i);

    struct ackmessage_t& ack_msg = (struct ackmessage_t&)(receivebuffer);  // overlaying the receive buffer
    struct datamessage_t& data_msg = (struct datamessage_t&)(sendbuffer);  // overlaying the send buffer

    // create a PragueCC object. Using default parameters for the Prague CC in line with TCP_Prague
    PragueCC pragueCC(app.max_pkt);
    // outside PragueCC CC-loop state
    time_tp now = pragueCC.Now();
    time_tp nextSend = now;  // time to send the next burst
    count_tp seqnr = 0;      // sequence number of last sent packet (first sequence number will be 1)
    count_tp inflight = 0;   // packets in-flight counter
    rate_tp pacing_rate;     // used for pacing the packets with the right interval (not taking into account the bursts)
    count_tp packet_window;  // allowed maximum packets in-flight
    count_tp packet_burst;   // allowed number of packets to send back-to-back; pacing interval needs to be taken into account for the next burst only
    size_tp packet_size;     // packet size is reduced when rates are low to preserve 2 packets per 25ms pacing interval

    // get initial CC state
    pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
    while (true) {
        count_tp inburst = 0;  // packets in-burst counter
        time_tp timeout = 0;   // pacing or retransmission interval time
        time_tp startSend = 0; // next time to send
        now = pragueCC.Now();
        // if the window and pacing interval allows, send the next burst
        while ((inflight < packet_window) && (inburst < packet_burst) && (nextSend - now <= 0)) {
            ecn_tp new_ecn;
            pragueCC.GetTimeInfo(data_msg.timestamp, data_msg.echoed_timestamp, new_ecn);
            if (startSend == 0)
                startSend = now;
            data_msg.seq_nr = ++seqnr;
            app.LogSend(now, data_msg.timestamp, data_msg.echoed_timestamp, 
                pacing_rate, packet_window, packet_burst, packet_size, seqnr, inflight, inburst, nextSend);
            data_msg.hton();
            size_tp bytes_sent = us.Send((char*)(&data_msg), packet_size, new_ecn);
            app.ExitIf(((size_tp)bytes_sent) != packet_size, "invalid data packet length sent");
            inburst++;
            inflight++;
        }
        if (startSend != 0) {
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

            bytes_received = us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, timeout);
            now = pragueCC.Now();
        } while ((bytes_received == 0) && (waitTimeout - now > 0));
        if (bytes_received >= ssize_t(sizeof(ack_msg))) {
            ack_msg.hton();
            pragueCC.PacketReceived(ack_msg.timestamp, ack_msg.echoed_timestamp);
            pragueCC.ACKReceived(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, seqnr, ack_msg.error_L4S, inflight);
            app.LogRecv(now, ack_msg.timestamp, ack_msg.echoed_timestamp, 
                ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S,
                pacing_rate, seqnr, packet_window, packet_burst, inflight, inburst, nextSend);
        }
        else // timeout, reset state
            if (inflight >= packet_window) {
                pragueCC.ResetCCInfo();
                inflight = 0;
		perror("Reset PragueCC\n");
            }
        pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
    }
}
