// udp_prague_receiver.cpp:
// An example of a (dummy data) UDP receiver that needs to send ACKs for a congestion controlled UDP sender
//

#include <string>
#include "prague_cc.h"
#include "udpsocket.h"
#include "app_stuff.h"

#define BUFFER_SIZE 8192      // in bytes (depending on MTU)
#define REPORT_SIZE (BUFFER_SIZE / 4)
#define PKT_BUFFER_SIZE 65536 // [RFC8888] calculated using arithmetic modulo 65536
#define SND_TIMEOUT 1000000   // Sender timeout in us when window-limited
#define RCV_TIMEOUT 250000    // Receive timeout for a previously-receiving packet

enum pktsend_tp {snd_init = 0, snd_sent, snd_recv, snd_lost};
enum pktrecv_tp {rcv_init = 0, rcv_recv, rcv_ackd, rcv_lost};

#pragma pack(push, 1)
struct datamessage_t {
    uint8_t type;
    time_tp timestamp;         // timestamp from peer, freeze and keep this time
    time_tp echoed_timestamp;  // echoed_timestamp can be used to calculate the RTT
    count_tp seq_nr;           // packet sequence number, should start with 1 and increase monotonic with packets sent

    void hton() {              // swap the bytes if needed
        type = 1;
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        seq_nr = htonl(seq_nr);
    }
};

struct framemessage_t {
    uint8_t type;
    time_tp timestamp;         // timestamp from peer, freeze and keep this time
    time_tp echoed_timestamp;  // echoed_timestamp can be used to calculate the RTT
    count_tp seq_nr;           // packet sequence number, should start with 1 and increase monotonic with packets sent
    count_tp frame_nr;         // frame sequence number, also start with 1 and increase monitonically

    void hton() {              // swap the bytes if needed
        type = 2;
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        seq_nr = htonl(seq_nr);
        frame_nr = htonl(frame_nr);
    }
};

struct ackmessage_t {
    uint8_t type;
    count_tp ack_seq;
    time_tp timestamp;         // timestamp from peer, freeze and keep this time
    time_tp echoed_timestamp;  // echoed_timestamp can be used to calculate the RTT
    count_tp packets_received; // echoed_packet counter
    count_tp packets_CE;       // echoed CE counter
    count_tp packets_lost;     // echoed lost counter
    bool error_L4S;            // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!

    void set_stat() {
        type = 1;
        ack_seq = htonl(ack_seq);
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        packets_received = htonl(packets_received);
        packets_CE = htonl(packets_CE);
        packets_lost = htonl(packets_lost);
    }
    count_tp get_stat(pktsend_tp *pkt_stat, count_tp m_packets_lost) {
        ack_seq = htonl(ack_seq);
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        packets_received = htonl(packets_received);
        packets_CE = htonl(packets_CE);
        packets_lost = htonl(packets_lost);

        pkt_stat[ack_seq % PKT_BUFFER_SIZE] = snd_recv;
        if (packets_lost - m_packets_lost > 0) {
            for(uint16_t i = 1; i <= (packets_lost - m_packets_lost); i++)
                pkt_stat[(ack_seq - i) % PKT_BUFFER_SIZE] = snd_lost;
        }
        return packets_lost;
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
    uint16_t get_stat(time_tp now, time_tp *sendtime, time_tp *pkt_rtt, count_tp &received, count_tp &lost, count_tp &mark, bool &error, pktsend_tp *pkt_stat, count_tp &last_ackseq) {
        uint16_t num_rtt = 0;
        begin_seq = htonl(begin_seq);
        num_reports = htons(num_reports);
        while (last_ackseq + 1 - begin_seq < 0) {
            if (pkt_stat[(last_ackseq + 1) % PKT_BUFFER_SIZE] == snd_sent) {
                lost++;
                pkt_stat[(last_ackseq + 1) % PKT_BUFFER_SIZE] = snd_lost;
            }
            last_ackseq++;
        }
        for (uint16_t i = 0; i < num_reports; i++, last_ackseq++) {
            uint16_t idx = (begin_seq + i) % PKT_BUFFER_SIZE;
            report[i] = htons(report[i]);
            if ((report[i] & 0x8000) >> 15) {
                if (pkt_stat[idx] == snd_sent || pkt_stat[idx] == snd_lost) {
                    received++;
                    mark += ((report[i] & 0x6000) >> 13 == ecn_ce);
                    error |= ((report[i] & 0x2000) >> 13 == 0x0);
                    pkt_rtt[num_rtt++] = now - ((report[i] & 0x1FFF) << 10) - sendtime[idx];
                    if (pkt_stat[idx] == snd_lost)
                        lost--;
                    pkt_stat[idx] = snd_recv;
                }
            } else {
                if (pkt_stat[idx] == snd_sent) {
                    lost++;
                    pkt_stat[idx] = snd_lost;
                }
            }
        }
        return num_rtt;
    }
    uint16_t set_stat(count_tp &seq, count_tp maxseq, time_tp now, time_tp *recvtime, ecn_tp *recvecn, pktrecv_tp *recvseq, size_tp maxpkt) {
        uint16_t rptsize = sizeof(type) + sizeof(begin_seq) + sizeof(num_reports);
        uint16_t reports = maxseq - seq > (count_tp)((maxpkt - rptsize) / sizeof(uint16_t)) ? (count_tp)((maxpkt - rptsize) / sizeof(uint16_t)) : maxseq - seq;
        begin_seq = seq;
        for (uint16_t i = 0; i < reports; i++, seq++) {
            uint16_t idx = (begin_seq + i) % PKT_BUFFER_SIZE;
            if (recvseq[idx] == rcv_recv || (recvseq[idx] == rcv_ackd && recvtime[idx] + RCV_TIMEOUT - now > 0)) {
                report[i] = htons((0x1 << 15) + ((recvecn[idx] & ecn_ce) << 13) + (((now - recvtime[idx] + (1 << 9)) >> 10) & 0x1FFF));
                recvseq[idx] = rcv_ackd;
            } else {
                report[i] = htons(0);
                recvseq[idx] = rcv_lost;
            }
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
    time_tp recvtime[PKT_BUFFER_SIZE] = {0};
    ecn_tp recvecn[PKT_BUFFER_SIZE] = {ecn_not_ect};
    pktrecv_tp recvseq[PKT_BUFFER_SIZE] = {rcv_init};
    if (app.rfc8888_ack && app.max_pkt < rfc8888_ackmsg.get_size(1)) {
        perror("Reset maximum ACK size\n");
        app.max_pkt = rfc8888_ackmsg.get_size(1);
    }

    if (app.connect) { // send a trigger ACK packet, otherwise just wait for data
        pragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
        pragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);
        ack_msg.set_stat();
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
                uint16_t seq_idx = data_msg.seq_nr % PKT_BUFFER_SIZE;
                if (start_seq == end_seq) {
                    start_seq = data_msg.seq_nr;
                    end_seq = data_msg.seq_nr + 1;
                } else {
                    // [start_seq, end_seq) data will be ACKed
                    if (start_seq - data_msg.seq_nr <= 0 && start_seq + PKT_BUFFER_SIZE - data_msg.seq_nr > 0 && data_msg.seq_nr + 1 - end_seq > 0) {
                      end_seq = data_msg.seq_nr + 1;
                    } else if (end_seq - data_msg.seq_nr > 0 && end_seq - PKT_BUFFER_SIZE - data_msg.seq_nr <= 0 && data_msg.seq_nr - start_seq < 0) {
                      start_seq = data_msg.seq_nr;
                    }
                }
                if (!(recvseq[seq_idx] == rcv_recv)) {
                    recvtime[seq_idx] = now;
                    recvecn[seq_idx] = ecn_tp(rcv_ecn & ecn_ce);
                    recvseq[seq_idx] = rcv_recv;
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
            ack_msg.ack_seq = data_msg.seq_nr;
            pragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
            pragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);

            app.LogSendACK(now, ack_msg.timestamp, ack_msg.echoed_timestamp, data_msg.seq_nr, sizeof(ack_msg),
                ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);

            ack_msg.set_stat();
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
