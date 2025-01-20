// udp_prague_receiver.cpp:
// An example of a (dummy data) UDP receiver that needs to send ACKs for a congestion controlled UDP sender
//

#include <string>
#include "prague_cc.h"
#include "udpsocket.h"
#include "app_stuff.h"

#define BUFFER_SIZE 8192       // in bytes (depending on MTU)
#define REPORT_SIZE (BUFFER_SIZE / 4)
#define TIME_BUFFER_SIZE 16384 // [RFC8888] each report MUST NOT include more than 16384 blocks

enum pktstat_tp {pkt_init = 0, pkt_sent, pkt_recv, pkt_lost};

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
    uint8_t type;
    time_tp timestamp;         // timestamp from peer, freeze and keep this time
    time_tp echoed_timestamp;  // echoed_timestamp can be used to calculate the RTT
    count_tp packets_received; // echoed_packet counter
    count_tp packets_CE;       // echoed CE counter
    count_tp packets_lost;     // echoed lost counter
    bool error_L4S;            // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!

    void hton() {              // swap the bytes if needed
        type = 1;
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        packets_received = htonl(packets_received);
        packets_CE = htonl(packets_CE);
        packets_lost = htonl(packets_lost);
    }
};

struct rfc8888ack_t {
    uint8_t type;
    count_tp begin_seq;        // Use 32-bit sequence number
    uint16_t num_reports;
    uint16_t report[REPORT_SIZE];

    uint16_t get_size(uint16_t rptsize) {
        return sizeof(uint16_t) * rptsize + sizeof(type) + sizeof(begin_seq) + sizeof(num_reports);
    }
    uint16_t get_stat(time_tp now, time_tp *sendtime, time_tp *pkt_rtt, count_tp &received, count_tp &lost, count_tp &mark, bool &error, pktstat_tp *pkt_stat, count_tp &last_ackseq) {
        uint16_t num_rtt = 0;
        begin_seq = htonl(begin_seq);
        num_reports = htons(num_reports);
        while (last_ackseq + 1 - begin_seq < 0) {
            if (pkt_stat[(last_ackseq + 1) % TIME_BUFFER_SIZE] == pkt_sent) {
                lost++;
                //printf("LOST, miss: %d, pkt_stat: %u\n", last_ackseq + 1, pkt_stat[(last_ackseq + 1) % TIME_BUFFER_SIZE]);
                pkt_stat[(last_ackseq + 1) % TIME_BUFFER_SIZE] = pkt_lost;
            }
            last_ackseq++;
        }
        for (uint16_t i = 0; i < num_reports; i++, last_ackseq++) {
            uint16_t idx = (begin_seq + i) % TIME_BUFFER_SIZE;
            report[i] = htons(report[i]);
            if ((report[i] & 0x8000) >> 15) {
                if (pkt_stat[idx] == pkt_sent || pkt_stat[idx] == pkt_lost) {
                    received++;
                    mark += ((report[i] & 0x6000) >> 13 == ecn_ce);
                    error |= ((report[i] & 0x2000) >> 13 == 0x0);
                    pkt_rtt[num_rtt++] = now - ((report[i] & 0x1FFF) << 10) - sendtime[idx];
                    if (pkt_stat[idx] == pkt_lost)
                        lost--;
                    //printf("RTT: %d, NOW: %d, Rep: %d, SEND: %d, seq: %u, num_reports: %u, idx: %u, pkt_stat: %u\n",
                    //    pkt_rtt[num_rtt-1], now, report[i], sendtime[idx], begin_seq, num_reports, idx, pkt_stat[idx]);
                    pkt_stat[idx] = pkt_recv;
                }
            } else {
                if (pkt_stat[idx] == pkt_sent) {
                    lost++;
                    //printf("LOST, idx: %u, pkt_stat: %u\n", idx, pkt_stat[idx]);
                    pkt_stat[idx] = pkt_lost;
                }
            }
        }
        return num_rtt;
    }
    uint16_t set_stat(count_tp &seq, count_tp &max_seq, time_tp now, time_tp *recvtime, ecn_tp *recvecn, bool *recvseq, size_tp max_pkt) {
        uint16_t rptsize = sizeof(type) + sizeof(begin_seq) + sizeof(num_reports);
        uint16_t reports = max_seq - seq > (count_tp)((max_pkt - rptsize) / sizeof(uint16_t)) ? (count_tp)((max_pkt - rptsize) / sizeof(uint16_t)) : max_seq - seq;
        begin_seq = seq;
        for (uint16_t i = 0; i < reports; i++, seq++) {
            if (recvseq[(begin_seq + i) % TIME_BUFFER_SIZE]) {
                report[i] = htons((0x1 << 15) + ((recvecn[(begin_seq + i) % TIME_BUFFER_SIZE] & 0x3) << 13) +
                    (((now - recvtime[(begin_seq + i) % TIME_BUFFER_SIZE] + (1 << 9)) >> 10) & 0x1FFF));
                recvseq[(begin_seq + i) % TIME_BUFFER_SIZE] = 0;
            } else
                report[i] = htons(0);
            rptsize += sizeof(uint16_t);
        }

        type = 2;
        begin_seq = htonl(begin_seq);
        num_reports = htons(reports);
        return rptsize;
    }
};
#pragma pack(pop)

int main(int argc, char **argv)
{
    AppStuff app(false, argc, argv); // initialize the app

    // Create a UDP socket
    UDPSocket us;
    if (app.connect)
        us.Connect(app.rcv_addr, app.rcv_port);
    else
        us.Bind(app.rcv_addr, app.rcv_port);

    char receivebuffer[BUFFER_SIZE];

    struct datamessage_t& data_msg = (struct datamessage_t&)(receivebuffer);  // overlaying the receive buffer
    struct ackmessage_t ack_msg;     // the send buffer

    // create a PragueCC object. No parameters needed if only ACKs are sent
    PragueCC pragueCC;
    time_tp now = pragueCC.Now();  // for reporting only
    ecn_tp new_ecn;

    // RFC8888 buffer
    struct rfc8888ack_t rfc8888_ackmsg;
    count_tp start_seq = 0, end_seq = 0;
    time_tp rfc8888_acktime = now + app.rfc8888_ackperiod;
    time_tp recvtime[TIME_BUFFER_SIZE] = {0};
    ecn_tp recvecn[TIME_BUFFER_SIZE] = {ecn_not_ect};
    bool recvseq[TIME_BUFFER_SIZE] = {false};
    if (app.rfc8888_ack && app.max_pkt < rfc8888_ackmsg.get_size(1)) {
        perror("Reset maximum ACK size\n");
        app.max_pkt = rfc8888_ackmsg.get_size(1);
    }

    if (app.connect) { // send a trigger ACK packet, otherwise just wait for data
        pragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
        pragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);
        ack_msg.hton();  // swap byte order if needed
        app.ExitIf(us.Send((char*)(&ack_msg), sizeof(ack_msg), new_ecn) != sizeof(ack_msg), "Invalid ack packet length sent.\n");
    }

    while (true) {
        now = pragueCC.Now();

        // Wait for an incoming data message
        ecn_tp rcv_ecn = ecn_not_ect;
        size_tp bytes_received = 0;
        time_tp waitTime = (app.rfc8888_ack && start_seq != end_seq) ? ((rfc8888_acktime - now > 0) ? (rfc8888_acktime - now) : 1) : 0;

        do {   // repeat if timeout or interrupted
            bytes_received = us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, waitTime);
        } while(bytes_received == 0 && waitTime == 0);

        if (bytes_received != 0) {
            // Extract the data message
            now = pragueCC.Now();
            data_msg.hton();  // swap byte order
            app.LogRecvData(now, data_msg.timestamp, data_msg.echoed_timestamp, data_msg.seq_nr, bytes_received);

            if (app.rfc8888_ack) {
                uint16_t seq_idx = data_msg.seq_nr % TIME_BUFFER_SIZE;
                if (start_seq == end_seq) {
                    start_seq = data_msg.seq_nr;
                    end_seq = data_msg.seq_nr + 1;
                } else {
                    // [start_seq, end_seq) data will be ACKed
                    if (start_seq - data_msg.seq_nr <= 0 && start_seq + TIME_BUFFER_SIZE - data_msg.seq_nr > 0 && data_msg.seq_nr + 1 - end_seq > 0) {
                      end_seq = data_msg.seq_nr + 1;
                    } else if (end_seq - data_msg.seq_nr > 0 && end_seq - TIME_BUFFER_SIZE - data_msg.seq_nr <= 0 && data_msg.seq_nr - start_seq < 0) {
                      start_seq = data_msg.seq_nr;
                    }
                }
                if (!recvseq[seq_idx]) {
                    recvtime[seq_idx] = now;
                    recvecn[seq_idx] = rcv_ecn;
                    recvseq[seq_idx] = 1;
                } else {
                    recvecn[seq_idx] = (rcv_ecn == ecn_ce) ? ecn_ce : recvecn[seq_idx];
                }
            }

            // Pass the relevant data to the PragueCC object:
            pragueCC.PacketReceived(data_msg.timestamp, data_msg.echoed_timestamp);
            pragueCC.DataReceivedSequence(rcv_ecn, data_msg.seq_nr);
        }

        now = pragueCC.Now();
        if (!app.rfc8888_ack) {
            // Return a corresponding acknowledge message
            pragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
            pragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);

            app.LogSendACK(now, ack_msg.timestamp, ack_msg.echoed_timestamp, data_msg.seq_nr, sizeof(ack_msg),
                ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);

            ack_msg.hton();  // swap byte order if needed
            app.ExitIf(us.Send((char*)(&ack_msg), sizeof(ack_msg), new_ecn) != sizeof(ack_msg), "Invalid ack packet length sent.\n");
        } else if (rfc8888_acktime - now <= 0) {
            while (start_seq != end_seq) {
                uint16_t rfc8888_acksize = rfc8888_ackmsg.set_stat(start_seq, end_seq, now, recvtime, recvecn, recvseq, app.max_pkt);
                app.ExitIf(us.Send((char*)(&rfc8888_ackmsg), rfc8888_acksize, ecn_l4s_id) != rfc8888_acksize, "Invalid RFC8888 ack packetlength sent.");
                app.LogSendRFC8888ACK(now, data_msg.seq_nr, rfc8888_acksize,
                    htonl(rfc8888_ackmsg.begin_seq), htons(rfc8888_ackmsg.num_reports), rfc8888_ackmsg.report);
            }

            rfc8888_acktime = now + app.rfc8888_ackperiod;
        }
    }
}
