#ifndef PRAGUE_CC_H
#define PRAGUE_CC_H

#include <stdint.h>

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
enum cca_tp {cca_prague_win, cca_prague_rate, cca_cubic};  // which CC algorithm is active

struct PragueState {
    time_tp   m_start_ref;  // used to have a start time of 0
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
    time_tp   m_rtt;           // last reported rtt (only for stats)
    time_tp   m_rtt_min;       // minimum report rtt (for cubic)
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
    // sender-end cubic variabls
    bool      m_reno_increase;
    time_tp   m_cubic_epoch_start;
    window_tp m_cubic_last_max_fracwin;
    uint32_t  m_cubic_K;
    window_tp m_cubic_origin_fracwin;
    // for alpha calculation, keep the previous alpha variables' state
    time_tp   m_alpha_ts;
    count_tp  m_alpha_packets_received;
    count_tp  m_alpha_packets_CE;
    count_tp  m_alpha_packets_lost;
    count_tp  m_alpha_packets_sent;
    // for loss and recovery calculation
    time_tp   m_loss_ts;
    cca_tp    m_loss_cca;
    window_tp m_lost_window;
    rate_tp   m_lost_rate;
    count_tp  m_loss_packets_lost;
    count_tp  m_loss_packets_sent;
    // for congestion experienced and window reduction (cwr) calculation
    time_tp   m_cwr_ts;
    count_tp  m_cwr_packets_sent;
    // state updated for the actual congestion control variables
    cs_tp     m_cc_state;
    cca_tp    m_cca_mode;
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
        size_tp max_packet_size = 1400,   // use MTU detection, or a low enough value. Can be updated on the fly (todo)
        fps_tp fps = 0,                   // only used for video; frames per second, 0 must be used for bulk transfer
        time_tp frame_budget = 0,         // only used for video; over what time [µs] you want to pace the frame (max 1000000/fps [µs])
        rate_tp init_rate = 12500,        // 12500 Byte/s (equiv. 100kbps)
        count_tp init_window = 10,        // 10 packets
        rate_tp min_rate = 12500,         // 12500 Byte/s (equiv. 100kbps)
        rate_tp max_rate = 12500000000);   // 12500000000 Byte/s (equiv. 100Gbps)
    
    ~PragueCC();

    time_tp Now();             // will have to return a monotonic increasing signed int 32 which will wrap around (after 4000 seconds)

    bool PacketReceived(       // call this when a packet is received from peer, returns false if the old packet is ignored
        time_tp timestamp,         // timestamp from peer, freeze and keep this time
        time_tp echoed_timestamp); // echoed_timestamp can be used to calculate the RTT

    bool ACKReceived(          // call this when an ACK is received from peer, returns false if the old ack is ignored
        count_tp packets_received, // echoed_packet counter
        count_tp packets_CE,       // echoed CE counter
        count_tp packets_lost,     // echoed lost counter
        count_tp packets_sent,     // local counter of packets sent up to now, an RTT is reached if remote ACK packets_received+packets_lost
        bool error_L4S,            // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!
        count_tp &inflight);       // how many packets are in flight after the ACKed);

    // can this be combined with ACKReceived?
    /*bool FrameACKReceived(     // call this when a frame ACK is received from peer
        count_tp packets_received, // echoed_packet counter
        count_tp packets_CE,       // echoed CE counter
        count_tp packets_lost,     // echoed lost counter
        bool error_L4S);           // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!*/

    void DataReceived(         // call this when a data packet is received as a receiver and you can identify lost packets
        ecn_tp ip_ecn,             // IP.ECN field value
        count_tp packets_lost);    // packets skipped; can be optionally -1 to potentially undo a previous cwindow reduction

    void DataReceivedSequence( // call this every time when a data packet with a sequence number is received as a receiver
        ecn_tp ip_ecn,             // IP.ECN field value
        count_tp packet_seq_nr);   // sequence number of the received packet

    void ResetCCInfo();        // call this when there is a RTO detected

    void GetTimeInfo(          // when the any-app needs to send a packet
        time_tp &timestamp,        // Own timestamp to echo by peer
        time_tp &echoed_timestamp, // defrosted timestamp echoed to peer
        ecn_tp &ip_ecn);           // ecn field to be set in the IP header

    void GetCCInfo(            // when the sending-app needs to send a packet
        rate_tp &pacing_rate,      // rate to pace the packets
        count_tp &packet_window,   // the congestion window in number of packets
        count_tp &packet_burst,    // number of packets that can be paced at once (<250µs)
        size_tp &packet_size);     // the packet size to transmit

    void GetACKInfo(           // when the receiving-app needs to send a packet
        count_tp &packets_received,// packet counter to echo
        count_tp &packets_CE,      // CE counter to echo
        count_tp &packets_lost,    // lost counter to echo (if used)
        bool &error_L4S);          // bleached/error ECN status to echo

    void GetCCInfoVideo(       // when the sending app needs to send a frame
        rate_tp &pacing_rate,      // rate to pace the packets
        size_tp &frame_size,       // the size of a single frame in Bytes
        count_tp &frame_window,    // the congestion window in number of frames
        count_tp &packet_burst,    // number of packets that can be paced at once (<250µs)
        size_tp &packet_size);     // the packet size to transmit

    void GetStats(PragueState &stats) // For logging purposes
    {
        stats = *this;  // makes a copy of the internal state and parameters
    }

    const PragueState* GetStatePtr() // For logging purposes
    {
        return this;  // gives a const pointer for reading the live state
    }

};
#endif //PRAGUE_CC_H
