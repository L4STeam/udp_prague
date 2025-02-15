// udp_prague_sender.cpp:
// An example of a (dummy data) UDP sender that needs to receive ACKs from a UDP receiver for congestion control
//

#include <string>
#include "prague_cc.h"
#include "udpsocket.h"
//#include "icmpsocket.h" TODO: optimize MTU detection
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
                report[i] = htons((0x1 << 15) + ((recvecn[idx] & 0x3) << 13) + (((now - recvtime[idx] + (1 << 9)) >> 10) & 0x1FFF));
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
    AppStuff app(true, argc, argv); // initialize the app

    // Create a UDP socket
    UDPSocket us;
    if (app.connect)
        us.Connect(app.rcv_addr, app.rcv_port);
    else
        us.Bind(app.rcv_addr, app.rcv_port);

    char receivebuffer[BUFFER_SIZE];
    uint32_t sendbuffer[BUFFER_SIZE/4];
    // init payload with dummy data
    for (int i = 0; i < BUFFER_SIZE/4; i++)
        sendbuffer[i] = htonl(i);

    struct ackmessage_t& ack_msg = (struct ackmessage_t&)(receivebuffer);  // overlaying the receive buffer
    struct datamessage_t& data_msg = (struct datamessage_t&)(sendbuffer);  // overlaying the send buffer

    // RFC8888 buffer
    struct rfc8888ack_t& rfc8888_ackmsg = (struct rfc8888ack_t&)(receivebuffer);  // overlaying the receive buffer
    time_tp sendtime[PKT_BUFFER_SIZE] = {0};
    pktsend_tp pkt_stat[PKT_BUFFER_SIZE] = {snd_init};
    time_tp pkts_rtt[REPORT_SIZE] = {0};
    count_tp last_ackseq = 0;   // Last received ACK sequence
    count_tp pkts_received = 0; // Receivd packets counter for RFC8888 feedback
    count_tp pkts_CE = 0;       // CE packets counter for RFC8888 feedback
    count_tp pkts_lost = 0;     // Lost packets counter for RFC8888 feedback
    bool err_L4S = false;       // L4S error flag for RFC8888 feedback

    // create a PragueCC object. Using default parameters for the Prague CC in line with TCP_Prague
    PragueCC pragueCC(app.max_pkt, 0, 0, PRAGUE_INITRATE, PRAGUE_INITWIN, PRAGUE_MINRATE, app.max_rate);

    // outside PragueCC CC-loop state
    time_tp now = pragueCC.Now();
    time_tp nextSend = now;     // time to send the next burst
    count_tp seqnr = 0;         // sequence number of last sent packet (first sequence number will be 1)
    count_tp inflight = 0;      // packets in-flight counter
    rate_tp pacing_rate;        // used for pacing the packets with the right interval (not taking into account the bursts)
    count_tp packet_window;     // allowed maximum packets in-flight
    count_tp packet_burst;      // allowed number of packets to send back-to-back; pacing interval needs to be taken into account for the next burst only
    size_tp packet_size;        // packet size is reduced when rates are low to preserve 2 packets per 25ms pacing interval
    ecn_tp new_ecn;

    ecn_tp rcv_ecn;
    size_tp bytes_received = 0;
    time_tp compRecv = 0;       // send time compensation
    time_tp waitTimeout = 0;    // time to wait for ACK receiving

    // wait for a trigger packet, otherwise just start sending
    if (!app.connect) {
        do {
            bytes_received = us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, 0);
        } while (bytes_received == 0);
        bytes_received = 0;
    }

    // Find maximum MTU can be used
    // ICMPSocket icmps(rcv_addr);
    // max_pkt = icmps.mtu_discovery(150, max_pkt, 1000000, 1);

    // get initial CC state
    pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
    while (true) {
        count_tp inburst = 0;   // packets in-burst counter
        time_tp startSend = 0;  // next time to send
        now = pragueCC.Now();
        // if the window and pacing interval allows, send the next burst
        while ((inflight < packet_window) && (inburst < packet_burst) && (nextSend - now <= 0)) {
            pragueCC.GetTimeInfo(data_msg.timestamp, data_msg.echoed_timestamp, new_ecn);
            if (!startSend)
                startSend = now;
            data_msg.seq_nr = ++seqnr;
            app.LogSendData(now, data_msg.timestamp, data_msg.echoed_timestamp, seqnr, packet_size,
                pacing_rate, packet_window, packet_burst, inflight, inburst, nextSend);
            data_msg.hton();
            app.ExitIf(us.Send((char*)(&data_msg), packet_size, new_ecn) != packet_size, "invalid data packet length sent");
            sendtime[seqnr % PKT_BUFFER_SIZE] = startSend;
            pkt_stat[seqnr % PKT_BUFFER_SIZE] = snd_sent;
            inburst++;
            inflight++;
        }
        if (startSend != 0) {
            if (compRecv + packet_size * inburst * 1000000 / pacing_rate <= 0)
                nextSend = time_tp(startSend + 1);
            else
                nextSend = time_tp(startSend + compRecv + packet_size * inburst * 1000000 / pacing_rate);
            compRecv = 0;
        }
        waitTimeout = nextSend;
        now = pragueCC.Now();
        if (inflight >= packet_window)
            waitTimeout = now + SND_TIMEOUT;
        do {
            bytes_received = us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, (waitTimeout - now > 0) ? (waitTimeout - now) : 1);
            now = pragueCC.Now();
        } while ((bytes_received == 0) && (waitTimeout - now > 0));
        if (receivebuffer[0] == 1 && bytes_received >= ssize_t(sizeof(ack_msg))) {
            ack_msg.hton();
            pragueCC.PacketReceived(ack_msg.timestamp, ack_msg.echoed_timestamp);
            pragueCC.ACKReceived(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, seqnr, ack_msg.error_L4S, inflight);
            pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);

            app.LogRecvACK(now, ack_msg.timestamp, ack_msg.echoed_timestamp, seqnr, bytes_received,
                ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S,
                pacing_rate, packet_window, packet_burst, inflight, inburst, nextSend);
        } else if (receivebuffer[0] == 2 && bytes_received >= rfc8888_ackmsg.get_size(0)) {
            uint16_t num_rtt = rfc8888_ackmsg.get_stat(now, sendtime, pkts_rtt, pkts_received, pkts_lost, pkts_CE, err_L4S, pkt_stat, last_ackseq);
            if (num_rtt) {
                pragueCC.RFC8888Received(num_rtt, pkts_rtt);
                pragueCC.ACKReceived(pkts_received, pkts_CE, pkts_lost, seqnr, err_L4S, inflight);
                pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
            }

            app.LogRecvRFC8888ACK(now, seqnr, bytes_received, rfc8888_ackmsg.begin_seq, rfc8888_ackmsg.num_reports,
                num_rtt, pkts_rtt, pkts_received, pkts_CE, pkts_lost, err_L4S,
                pacing_rate, packet_window, packet_burst, inflight, inburst, nextSend);
        } else {
            if (inflight >= packet_window) {
                pragueCC.ResetCCInfo();
                inflight = 0;
                perror("Reset PragueCC\n");
                pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
                nextSend = now;
            }
        }
        now = pragueCC.Now();
        if (inflight && waitTimeout - now <= 0) {
            // Exceed time will be compensated (except reset)
            compRecv += (waitTimeout - now);
        }
    }
}
