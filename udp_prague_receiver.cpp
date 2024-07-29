// udp_prague_receiver.cpp:
// An example of a (dummy data) UDP receiver that needs to send ACKs for a congestion controlled UDP sender
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
    bool verbose = false;
    bool quiet = false;
    const char *rcv_addr = "0.0.0.0"; // any IP address
    int rcv_port = 8080;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-a" && i + 1 < argc) {
            rcv_addr = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            rcv_port = atoi(argv[++i]);
        } else if (arg == "-v") {
            verbose = true;
            quiet = true;
        } else if (arg == "-q") {
            quiet = true;
        } else {
            perror("Usage: udp_prague_receiver -a <receiver address, def: 0.0.0.0> -p <receiver port, def: 8080> -v (for verbose prints) -q (for quiet)");
            return 1;
        }
    }
    // Create a UDP socket
    UDPSocket us;
    us.Bind(rcv_addr, rcv_port);

    char receivebuffer[BUFFER_SIZE];

    struct datamessage_t& data_msg = (struct datamessage_t&)(receivebuffer);  // overlaying the receive buffer
    struct ackmessage_t ack_msg;     // the send buffer

    printf("UDP Prague receiver listening on port %d.\n", rcv_port);

    // create a PragueCC object. No parameters needed if only ACKs are sent
    PragueCC pragueCC;
    time_tp now = pragueCC.Now();  // for reporting only
    // state for verbose reporting
    time_tp send_tm = now;      // diff reference
    // state for default (non-quiet) reporting
    time_tp rept_tm = now + 1000000;      // timer for reporting interval
    rate_tp accbytes_recv = 0;  // accumulative bytes received
    rate_tp accbytes_sent = 0;  // accumulative bytes sent (ACKs)
    rate_tp acc_rtts_acks = 0;  // accumulative rtts to calculate the average
    count_tp count_rtts = 0;    // count the RTT reports
    count_tp prev_packets = 0;  // prev packets received
    count_tp prev_marks = 0;    // prev marks received
    count_tp prev_losts = 0;    // prev losts received

    if (verbose) {
        printf("r: time, timestamp, echoed_timestamp, bytes_received, seqnr\n");
        printf("s: time, timestamp, echoed_timestamp, time_diff, packets_received, packets_CE, packets_lost, seqnr, error_L4S\n");
    }
    while (true) {
        // Wait for an incoming data message
        ecn_tp rcv_ecn = ecn_not_ect;
        size_tp bytes_received = us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, 0);

        while (bytes_received == 0) {   // repeat if timeout or interrupted
            bytes_received = us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, 0);
        }

        // Extract the data message
        data_msg.hton();  // swap byte order
        if (!quiet) {
            now = pragueCC.Now();
            accbytes_recv += bytes_received;
            if (data_msg.echoed_timestamp) {
                acc_rtts_acks += (now - data_msg.echoed_timestamp);
                count_rtts++;
            }
        }
        if (verbose) {
            now = pragueCC.Now();
            printf("r: %d, %d, %d, %lld, %d\n", now, data_msg.timestamp, data_msg.echoed_timestamp, bytes_received, data_msg.seq_nr);
        }

        // Pass the relevant data to the PragueCC object:
        pragueCC.PacketReceived(data_msg.timestamp, data_msg.echoed_timestamp);
        pragueCC.DataReceivedSequence(rcv_ecn, data_msg.seq_nr);

        // Return a corresponding acknowledge message
        ecn_tp new_ecn;
        pragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
        pragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);

        if (verbose) {
            now = pragueCC.Now();
            printf("s: %d, %d, %d, %d, %d, %d, %d, %d, %d\n",
                       now, ack_msg.timestamp, ack_msg.echoed_timestamp, ack_msg.timestamp - send_tm, ack_msg.packets_received,
                       ack_msg.packets_CE, ack_msg.packets_lost, data_msg.seq_nr, ack_msg.error_L4S);
            send_tm = ack_msg.timestamp;
        }

        ack_msg.hton();  // swap byte order if needed
        size_tp bytes_sent = us.Send((char*)(&ack_msg), sizeof(ack_msg), new_ecn);
        if (bytes_sent != sizeof(ack_msg)) {
            perror("invalid ack packet length sent");
            exit(1);
        }
        if (!quiet) {
            ack_msg.hton();  // swap back byte order
            accbytes_sent += bytes_sent;
            now = pragueCC.Now();
            if (now - rept_tm >= 0) {
                float rate_recv = 8.0f * accbytes_recv / (now - rept_tm + 1000000);
                float rate_send = 8.0f * accbytes_sent / (now - rept_tm + 1000000);
                float rtts_acks = (count_rtts > 0) ? 0.001f * acc_rtts_acks / count_rtts : 0.0f;
                float mark_prob = (ack_msg.packets_received > prev_packets) ?
                    100.0f * (ack_msg.packets_CE - prev_marks) / (ack_msg.packets_received - prev_packets) : 0.0f;
                float loss_prob = (ack_msg.packets_received > prev_packets) ?
                    100.0f*(ack_msg.packets_lost - prev_losts) / (ack_msg.packets_received - prev_packets) : 0.0f;
                printf("[RECVER]: %.2f sec, %.3f Mbps, ACKs rate: %.3f Mbps, Acks RTT: %.3f ms, Mark: %.2f%%(%d/%d), Lost: %.2f%%(%d/%d)\n",
                    now / 1000000.0f, rate_recv, rate_send, rtts_acks,
                    mark_prob, ack_msg.packets_CE - prev_marks, ack_msg.packets_received - prev_packets,
                    loss_prob, ack_msg.packets_lost - prev_losts, ack_msg.packets_received - prev_packets);
                rept_tm = now + 1000000;
                accbytes_recv = 0;
                accbytes_sent = 0;
                acc_rtts_acks = 0;
                count_rtts = 0;
                prev_packets = ack_msg.packets_received;
                prev_marks = ack_msg.packets_CE;
                prev_losts = ack_msg.packets_lost;
            }
        }
    }
}
