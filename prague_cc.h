#ifndef PRAGUE_CC_H
#define PRAGUE_CC_H

#include <stdint.h>
#include <iostream>


typedef uint64_t size_tp;    // size in Bytes
typedef uint64_t window_tp;  // fractional window size in µBytes (to match time in µs, for easy Bytes/second rate calculations)
typedef uint64_t rate_tp;    // rate in Bytes/second
typedef int32_t time_tp;     // timestamp or interval in microseconds, timestamps have a fixed but no meaningful reference,
                             // so use only for intervals beteen 2 timestamps
                             // signed because it can wrap around, and we need to compare both ways (< 0 and > 0)
typedef int32_t count_tp;    // count in packets (or frames), signed because it can wrap around, and we need to compare both ways
enum ecn_tp: uint8_t {ecn_not_ect=0, ecn_l4s_id=1, ecn_ect0=2, ecn_ce=3}; 
                             // 2 bits in the IP header, only values 0-3 are valid, and 1 (0b01) and 3 (0b11) are L4S valid
typedef uint8_t fps_tp;      // frames per second: any value from 1 till 255 can be used, 0 must be used for bulk
typedef int64_t prob_tp;
enum cs_tp {cs_init, cs_cong_avoid, cs_in_loss, cs_in_cwr}; 

struct PragueState {
    // parameters
        rate_tp   m_init_rate;
        window_tp m_init_window;
        rate_tp   m_min_rate;
        rate_tp   m_max_rate;
        size_tp   m_max_packet_size;
        time_tp   m_frame_interval;
        time_tp   m_frame_budget;
    // both-end variables
        time_tp   m_ts_remote;     // to keep the frozen timestamp from the peer, and echo it back defrosted
        time_tp   m_srtt;          // our own measured and smoothed RTT (smoothing factor = 1/8)
        time_tp   m_vrtt;          // our own virtual RTT = max(srtt, 25ms)
    // receiver-end variables (to be echoed to sender)
        time_tp   m_r_prev_ts;            // used to see if an ack isn't older than the previous ack
        count_tp  m_r_packets_received;   // as a receiver, keep counters to echo back
        count_tp  m_r_packets_CE;
        count_tp  m_r_packets_lost;
        bool      m_r_error_L4S;          // as a receiver, check L4S-ECN validity to echo back an error
    // sender-end variables
        time_tp   m_cc_ts;
        count_tp  m_packets_received;     // latest known receiver end counters
        count_tp  m_packets_CE;
        count_tp  m_packets_lost;
        count_tp  m_packets_sent;
        bool      m_error_L4S;            // latest known receiver-end error state
        // for alpha calculation, keep the previous alpha variables' state
        time_tp   m_alpha_ts;
        count_tp  m_alpha_packets_received;
        count_tp  m_alpha_packets_CE;
        count_tp  m_alpha_packets_lost;
        count_tp  m_alpha_packets_sent;
        // for loss and recovery calculation
        time_tp   m_loss_ts;
        window_tp m_lost_window;
        count_tp  m_loss_packets_lost;
        count_tp  m_loss_packets_sent;
        // for congestion experienced and window reduction (cwr) calculation
        time_tp   m_cwr_ts;
        count_tp  m_cwr_packets_sent;
        // state updated for the actual congestion control variables
        cs_tp     m_cc_state;
        prob_tp   m_alpha;
        rate_tp   m_pacing_rate;
        window_tp m_fractional_window;
        count_tp  m_packet_burst;
        size_tp   m_packet_size;
        count_tp  m_packet_window;
};

class PragueCC: private PragueState {
    public:

        PragueCC(
            fps_tp fps = 0,                   // only used for video; frames per second, 0 must be used for bulk transfer
            time_tp frame_budget = 0,         // only used for video; over what time [µs] you want to pace the frame (max 1000000/fps [µs])
            rate_tp init_rate = 12500,        // 12500 Byte/s (equiv. 100kbps)
            count_tp init_window = 10,        // 10 packets
            rate_tp min_rate = 12500,         // 12500 Byte/s (equiv. 100kbps)
            rate_tp max_rate = 12500000000,   // 12500000000 Byte/s (equiv. 100Gbps)
            size_tp max_packet_size = 1400)   // use MTU detection, or a low enough value. Can be updated on the fly
        {
            time_tp ts_now = Now();
        // parameters
            m_init_rate = init_rate;
            m_init_window = window_tp(init_window)*max_packet_size*1000000;
            m_min_rate = min_rate;
            m_max_rate = max_rate;
            m_max_packet_size = max_packet_size;
            m_frame_interval = fps?1000000/fps:0;
            m_frame_budget = frame_budget;
            if (m_frame_budget > m_frame_interval)
                m_frame_budget = m_frame_interval;
        // both end variables
            m_ts_remote = 0;    // to keep the frozen timestamp from the peer, and echo it back defrosted
            m_srtt = 0;         // our own measured and smoothed RTT (smoothing factor = 1/8)
            m_vrtt = 0;         // our own virtual RTT = max(srtt, 25ms)
        // receiver end variables (to be echoed to sender)
            m_r_prev_ts = 0;      // used to see if an ack isn't older than the previous ack
            m_r_packets_received = 0; // as a receiver, keep counters to echo back
            m_r_packets_CE = 0;
            m_r_packets_lost = 0;
            m_r_error_L4S = false; // as a receiver, check L4S-ECN validity to echo back an error
        // sender end variables
            m_cc_ts = ts_now;   // time of last cc update
            m_packets_received = 0; // latest known receiver end counters
            m_packets_CE = 0; 
            m_packets_lost = 0;
            m_packets_sent = 0;
            m_error_L4S = false; // latest known receiver end error state
            // for alpha calculation, keep the previous alpha variables' state
            m_alpha_ts = ts_now;  // start recording alpha from now on (every vrtt)
            m_alpha_packets_received = 0; 
            m_alpha_packets_CE = 0;
            m_alpha_packets_lost = 0;
            m_alpha_packets_sent = 0;
            // for loss and recovery calculation
            m_loss_ts = 0;
            m_lost_window = 0;
            m_loss_packets_lost = 0;
            m_loss_packets_sent = 0;
            // for congestion experienced and window reduction (cwr) calculation
            m_cwr_ts = 0;
            m_cwr_packets_sent = 0;
            // state updated for the actual congestion control variables
            m_cc_state = cs_init;
            m_alpha = 0;
            m_pacing_rate = init_rate;
            m_fractional_window = m_init_window;
            m_packet_size = m_pacing_rate * 25 / 1000 / 2;            // B/p = B/s * 25ms/burst / 2p/burst
            if (m_packet_size < 150)
                m_packet_size = 150;
            if (m_packet_size > m_max_packet_size)
                m_packet_size = m_max_packet_size;
            m_packet_burst = m_pacing_rate * 250 / 1000000 / m_packet_size;  // p = B/s * 250µs / B/p
            if (m_packet_burst < 1) {
                m_packet_burst = 1;
            }
            m_packet_window = (m_fractional_window/1000000 + m_packet_size -1)/m_packet_size;
            if (m_packet_window < 2) {
                m_packet_window = 2;
            }
        }
        ~PragueCC() {}

        time_tp Now();           // will have to return a monotonic increasing signed int 32 which will wrap around (after 4000 seconds)

        bool PacketReceived(	 // call this when a packet is received from peer, returns false if the old packet is ignored
            time_tp timestamp,	       // timestamp from peer, freeze and keep this time
            time_tp echoed_timestamp); // echoed_timestamp can be used to calculate the RTT

        bool ACKReceived(        // call this when an ACK is received from peer, returns false if the old ack is ignored
            count_tp packets_received, // echoed_packet counter
            count_tp packets_CE,	   // echoed CE counter
            count_tp packets_lost,	   // echoed lost counter
            count_tp packets_sent,     // local counter of packets sent up to now, an RTT is reached if remote ACK packets_received+packets_lost
            bool error_L4S,             // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!
            count_tp &inflight);         // how many packets are in flight after the ACKed);

        bool FrameACKReceived(   // call this when a frame ACK is received from peer
            count_tp packets_received, // echoed_packet counter
            count_tp packets_CE,       // echoed CE counter
            count_tp packets_lost,     // echoed lost counter
            bool error_L4S);           // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!

        void DataReceived(  // call this when a data packet is received as a receiver and you can identify lost packets
            ecn_tp ip_ecn,             // IP.ECN field value
            count_tp packets_lost);     // packets skipped; can be optionally -1 to potentially undo a previous cwindow reduction 

        void DataReceivedSequence(  // call this every time when a data packet with a sequence number is received as a receiver
            ecn_tp ip_ecn,             // IP.ECN field value
            count_tp packet_seq_nr);     // sequence number of the received packet 

        void ResetCCInfo();      // call this when there is a RTO detected
        
        void GetTimeInfo(        // when the any-app needs to send a packet
            time_tp &timestamp,        // Own timestamp to echo by peer
            time_tp &echoed_timestamp, // defrosted timestamp echoed to peer
            ecn_tp &ip_ecn);           // ecn field to be set in the IP header

        void GetCCInfo(          // when the sending-app needs to send a packet
            rate_tp &pacing_rate,      // rate to pace the packets
            count_tp &packet_window,   // the congestion window in number of packets
            count_tp &packet_burst,    // number of packets that can be paced at once (<250µs)
            size_tp &packet_size);     // the packet size to transmit

        void GetACKInfo(         // when the receiving-app needs to send a packet
            count_tp &packets_received,// packet counter to echo
            count_tp &packets_CE,      // CE counter to echo
            count_tp &packets_lost,    // lost counter to echo (if used)
            bool &error_L4S);          // bleached/error ECN status to echo

        void GetCCInfoVideo(     // when the sending app needs to send a frame
            rate_tp &pacing_rate,      // rate to pace the packets
            size_tp &frame_size,       // the size of a single frame in Bytes
            count_tp &frame_window,    // the congestion window in number of frames
            count_tp &packet_burst,    // number of packets that can be paced at once (<250µs)
            size_tp &packet_size);     // the packet size to transmit

        void GetStats(          // For logging purposes
            PragueState &stats)
        {
            stats = *this;  // makes a copy of the internal state and parameters
        }

        time_tp getSrtt() {return m_srtt;}  // get the smoothed RTT
        prob_tp getAlpha() {return m_alpha;}  // get alpha

};
#endif //PRAGUE_CC_H
