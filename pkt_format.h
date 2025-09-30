#ifndef PKT_FORMAT_H
#define PKT_FORMAT_H

// pkt_format.h:
// Collecting all packet formants
//

#include "prague_cc.h"

#define BUFFER_SIZE 8192      // in bytes (depending on MTU)
#define REPORT_SIZE (BUFFER_SIZE / 4)
#define PKT_BUFFER_SIZE 65536 // [RFC8888] calculated using arithmetic modulo 65536
#define FRM_BUFFER_SIZE 2048
#define SND_TIMEOUT 1000000   // Sender timeout in us when window-limited
#define RCV_TIMEOUT 250000    // Receive timeout for a previously-receiving packet

#define BULK_DATA_TYPE   1
#define RT_DATA_TYPE     2
#define PKT_ACK_TYPE     17
#define RFC8888_ACK_TYPE 18

enum pktsend_tp {snd_init = 0, snd_sent, snd_recv, snd_lost};
enum pktrecv_tp {rcv_init = 0, rcv_recv, rcv_ackd, rcv_lost};

#pragma pack(push, 1)
struct datamessage_t {
    uint8_t type;
    time_tp timestamp;         // timestamp from peer, freeze and keep this time
    time_tp echoed_timestamp;  // echoed_timestamp can be used to calculate the RTT
    count_tp seq_nr;           // packet sequence number, should start with 1 and increase monotonic with packets sent

    void hton() {              // swap the bytes if needed
        type = BULK_DATA_TYPE;
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
    count_tp frame_sent;       // frame sent before this packets
    count_tp frame_size;       // frame size in bytes

    void hton() {              // swap the bytes if needed
        type = RT_DATA_TYPE;
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        seq_nr = htonl(seq_nr);
        frame_nr = htonl(frame_nr);
        frame_sent = htonl(frame_sent);
        frame_size = htonl(frame_size);
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
        type = PKT_ACK_TYPE;
        ack_seq = htonl(ack_seq);
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        packets_received = htonl(packets_received);
        packets_CE = htonl(packets_CE);
        packets_lost = htonl(packets_lost);
    }
    void get_stat(pktsend_tp *pkts_stat, count_tp &m_packets_lost) {
        ack_seq = htonl(ack_seq);
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        packets_received = htonl(packets_received);
        packets_CE = htonl(packets_CE);
        packets_lost = htonl(packets_lost);

        pkts_stat[ack_seq % PKT_BUFFER_SIZE] = snd_recv;
        if (packets_lost - m_packets_lost > 0) {
            for(uint16_t i = 1; i <= (packets_lost - m_packets_lost); i++)
                if (pkts_stat[(ack_seq - i) % PKT_BUFFER_SIZE] == snd_sent)
                    pkts_stat[(ack_seq - i) % PKT_BUFFER_SIZE] = snd_lost;
        }
        m_packets_lost = packets_lost;
    }
    void get_frame_stat(pktsend_tp *pkts_stat, count_tp &m_packets_lost, bool is_sending, count_tp frm_sending, count_tp &recv_frame,
                        count_tp &lost_frame, count_tp *frm_idx, count_tp *frm_pktsent, count_tp *frm_pktlost) {
        ack_seq = htonl(ack_seq);
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        packets_received = htonl(packets_received);
        packets_CE = htonl(packets_CE);
        packets_lost = htonl(packets_lost);

        count_tp frm_index = frm_idx[ack_seq % PKT_BUFFER_SIZE];
        if (pkts_stat[ack_seq % PKT_BUFFER_SIZE] == snd_sent) {
            frm_pktsent[frm_index % FRM_BUFFER_SIZE]--;
            if ((frm_index != frm_sending || !is_sending) &&
	        !frm_pktsent[frm_index % FRM_BUFFER_SIZE] &&
		!frm_pktlost[frm_index % FRM_BUFFER_SIZE])
                recv_frame++;
        } else if (pkts_stat[ack_seq % PKT_BUFFER_SIZE] == snd_lost) {
            frm_pktlost[frm_index % FRM_BUFFER_SIZE]--;
            if ((frm_index != frm_sending || !is_sending) &&
	        !frm_pktlost[frm_index % FRM_BUFFER_SIZE]) {
                lost_frame--;
                if (!frm_pktsent[frm_index % FRM_BUFFER_SIZE])
                    recv_frame++;
            }
        }
        pkts_stat[ack_seq % PKT_BUFFER_SIZE] = snd_recv;

        if (packets_lost - m_packets_lost > 0) {
            for(uint16_t i = 1; i <= (packets_lost - m_packets_lost); i++) {
                if (pkts_stat[(ack_seq - i) % PKT_BUFFER_SIZE] == snd_sent) {
                    frm_index = frm_idx[(ack_seq - i) % PKT_BUFFER_SIZE];
                    frm_pktsent[frm_index % FRM_BUFFER_SIZE]--;
                    if ((frm_index != frm_sending || !is_sending) &&
		        !frm_pktlost[frm_index % FRM_BUFFER_SIZE])
                        lost_frame++;
                    frm_pktlost[frm_index % FRM_BUFFER_SIZE]++;
                    pkts_stat[(ack_seq - i) % PKT_BUFFER_SIZE] = snd_lost;
                }
            }
        }
        m_packets_lost = packets_lost;
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
    uint16_t get_stat(time_tp now, time_tp *sendtime, time_tp *pkts_rtt, count_tp &rcvd, count_tp &lost, count_tp &mark, bool &error,
                      pktsend_tp *pkts_stat, count_tp &last_ack) {
        uint16_t num_rtt = 0;
        begin_seq = htonl(begin_seq);
        num_reports = htons(num_reports);
        while (last_ack + 1 - begin_seq < 0) {
            if (pkts_stat[(last_ack + 1) % PKT_BUFFER_SIZE] == snd_sent) {
                lost++;
                pkts_stat[(last_ack + 1) % PKT_BUFFER_SIZE] = snd_lost;
            }
            last_ack++;
        }
        for (uint16_t i = 0; i < num_reports; i++, last_ack++) {
            uint16_t idx = (begin_seq + i) % PKT_BUFFER_SIZE;
            report[i] = htons(report[i]);
            if ((report[i] & 0x8000) >> 15) {
                if (pkts_stat[idx] == snd_sent || pkts_stat[idx] == snd_lost) {
                    rcvd++;
                    mark += ((report[i] & 0x6000) >> 13 == ecn_ce);
                    error |= ((report[i] & 0x2000) >> 13 == 0x0);
                    pkts_rtt[num_rtt++] = now - ((report[i] & 0x1FFF) << 10) - sendtime[idx];
                    if (pkts_stat[idx] == snd_lost)
                        lost--;
                    pkts_stat[idx] = snd_recv;
                }
            } else {
                if (pkts_stat[idx] == snd_sent) {
                    lost++;
                    pkts_stat[idx] = snd_lost;
                }
            }
        }
        return num_rtt;
    }
    uint16_t get_frame_stat(time_tp now, time_tp *sendtime, time_tp *pkts_rtt, count_tp &rcvd, count_tp &lost, count_tp &mark,
                            bool &error, pktsend_tp *pkts_stat, count_tp &last_ack, bool is_sending, count_tp frm_sending,
			    count_tp &recv_frame, count_tp &lost_frame, count_tp *frm_idx, count_tp *frm_pktsent, count_tp *frm_pktlost) {
        uint16_t frm_index;
        uint16_t num_rtt = 0;
        begin_seq = htonl(begin_seq);
        num_reports = htons(num_reports);
        while (last_ack + 1 - begin_seq < 0) {
            if (pkts_stat[(last_ack + 1) % PKT_BUFFER_SIZE] == snd_sent) {
                lost++;
                frm_index = frm_idx[(last_ack + 1) % PKT_BUFFER_SIZE];
                frm_pktsent[frm_index % FRM_BUFFER_SIZE]--;
                if ((frm_index != frm_sending || !is_sending) &&
		    !frm_pktlost[frm_index % FRM_BUFFER_SIZE])
                    lost_frame++;
                frm_pktlost[frm_index % FRM_BUFFER_SIZE]++;
                pkts_stat[(last_ack + 1) % PKT_BUFFER_SIZE] = snd_lost;
            }
            last_ack++;
        }
        for (uint16_t i = 0; i < num_reports; i++, last_ack++) {
            uint16_t idx = (begin_seq + i) % PKT_BUFFER_SIZE;
            report[i] = htons(report[i]);
            if ((report[i] & 0x8000) >> 15) {
                if (pkts_stat[idx] == snd_sent || pkts_stat[idx] == snd_lost) {
                    rcvd++;
                    mark += ((report[i] & 0x6000) >> 13 == ecn_ce);
                    error |= ((report[i] & 0x2000) >> 13 == 0x0);
                    pkts_rtt[num_rtt++] = now - ((report[i] & 0x1FFF) << 10) - sendtime[idx];
                    if (pkts_stat[idx] == snd_lost)
                        lost--;
                    frm_index = frm_idx[idx];
                    if (pkts_stat[idx] == snd_sent) {
                        frm_pktsent[frm_index % FRM_BUFFER_SIZE]--;
                        if ((frm_index != frm_sending || !is_sending) &&
			    !frm_pktsent[frm_index % FRM_BUFFER_SIZE] &&
			    !frm_pktlost[frm_index % FRM_BUFFER_SIZE])
                            recv_frame++;
                    } else if (pkts_stat[idx] == snd_lost) {
                        frm_pktlost[frm_index % FRM_BUFFER_SIZE]--;
                        if ((frm_index != frm_sending || !is_sending) &&
			    !frm_pktlost[frm_index % FRM_BUFFER_SIZE]) {
                            lost_frame--;
                            if (!frm_pktsent[frm_index % FRM_BUFFER_SIZE])
                                recv_frame++;
                        }
                    }
                    pkts_stat[idx] = snd_recv;
                }
            } else {
                if (pkts_stat[idx] == snd_sent) {
                    lost++;
                    frm_index = frm_idx[idx];
                    frm_pktsent[frm_index % FRM_BUFFER_SIZE]--;
                    if ((frm_index != frm_sending || !is_sending) &&
		        !frm_pktlost[frm_index % FRM_BUFFER_SIZE])
                        lost_frame++;
                    frm_pktlost[frm_index % FRM_BUFFER_SIZE]++;
                    pkts_stat[idx] = snd_lost;
                }
            }
        }
        return num_rtt;
    }
    uint16_t set_stat(count_tp &seq, count_tp maxseq, time_tp now, time_tp *recvtime, ecn_tp *recvecn, pktrecv_tp *recvseq, size_tp maxpkt) {
        uint16_t rptsize = sizeof(type) + sizeof(begin_seq) + sizeof(num_reports);
        uint16_t reports = maxseq - seq > (count_tp)((maxpkt - rptsize) / sizeof(uint16_t)) ?
	                   (count_tp)((maxpkt - rptsize) / sizeof(uint16_t)) :
			   maxseq - seq;
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

        type = RFC8888_ACK_TYPE;
        begin_seq = htonl(begin_seq);
        num_reports = htons(reports);
        return rptsize;
    }
};
#pragma pack(pop)

#endif //PKT_FORMAT_H
