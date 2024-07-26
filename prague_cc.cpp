#include <chrono>
#include "prague_cc.h"

const time_tp REF_RTT = 25000;             // 25ms
const uint8_t PROB_SHIFT = 20;             // enough as max value that can control up to 100Gbps with r [Mbps] = 1/p - 1, p = 1/(r + 1) = 1/100001
const prob_tp MAX_PROB = 1 << PROB_SHIFT;  // with r [Mbps] = 1/p - 1 = 2^20 Mbps = 1Tbps
const uint8_t ALPHA_SHIFT = 4;             // >> 4 is divide by 16

time_tp PragueCC::Now() // TODO: microsecond to time_tp
{
    // Check if now==0; skip this value used to check uninitialized timepstamp
    time_tp now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now == 0) {
        now++;
    }
    return now;   
}

bool PragueCC::PacketReceived(         // call this when a packet is received from peer. Returns true if this is a newer packet, false if this is an older
    const time_tp timestamp,           // timestamp from peer, freeze and keep this time
    const time_tp echoed_timestamp)    // echoed_timestamp can be used to calculate the RTT
{
    if ((m_cc_state != cs_init) && (m_r_prev_ts - timestamp > 0)) // is this an older timestamp?
        return false;
    time_tp ts = Now();
    //m_ts_remote = ts - timestamp;  // freeze the remote timestamp
    m_ts_remote = timestamp;  // do not freeze the remote timestamp
    m_rtt = ts - echoed_timestamp;
    if (m_cc_state != cs_init)
        m_srtt += (m_rtt - m_srtt) >> 3;
    else
        m_srtt = m_rtt;
    // m_vrtt = std::max(m_srtt, REF_RTT);
    m_vrtt = (m_srtt > REF_RTT) ? m_srtt : REF_RTT;
    m_r_prev_ts = timestamp;
    return true;
}

bool PragueCC::ACKReceived(    // call this when an ACK is received from peer. Returns true if this is a newer ACK, false if this is an old ACK
    count_tp packets_received, // echoed_packet counter
    count_tp packets_CE,       // echoed CE counter
    count_tp packets_lost,     // echoed lost counter
    count_tp packets_sent,     // local counter of packets sent up to now, an RTT is reached if remote ACK packets_received+packets_lost
    bool error_L4S,            // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!
    count_tp &inflight)        // how many packets are in flight after the ACKed
{
    if ((m_packets_received - packets_received > 0) || (m_packets_CE - packets_CE > 0)) // this is an older or invalid ACK (these counters can't go down)
        return false;

    time_tp pacing_interval = m_packet_size * 1000000 * m_packet_burst / m_pacing_rate; // calculate the max expected rtt from pacing
    time_tp srtt = (m_srtt > pacing_interval) ? m_srtt : pacing_interval; // take into account the pacing delay
    if (m_cc_state == cs_init)  // initialize the window with the initial pacing rate
    {
        m_fractional_window = srtt * m_pacing_rate;
        m_cc_state = cs_cong_avoid;
    }
    time_tp ts = Now();
    // Update alpha if both a window and a virtual rtt are passed
    if ((packets_received + packets_lost - m_alpha_packets_sent > 0) && (ts - m_alpha_ts - m_vrtt >= 0)) {
    //if ((packets_received - m_alpha_packets_received + packets_lost - m_alpha_packets_lost > max(2, m_fractional_window / m_packet_size / 1000000)) 
    //    && (now() - m_prev_cycle > 25000)) {
        // prob_tp prob = (packets_CE - m_alpha_packets_CE) << PROB_SHIFT / (packets_received - m_alpha_packets_received);
        prob_tp prob = (prob_tp(packets_CE - m_alpha_packets_CE) << PROB_SHIFT) / (packets_received - m_alpha_packets_received);
        // std::cout << "prob: " << prob << "\n";
        m_alpha += ((prob - m_alpha) >> 4);
        m_alpha_packets_sent = packets_sent;
        m_alpha_packets_CE = packets_CE;
        m_alpha_packets_received = packets_received;
        m_alpha_ts = ts;
    }
    // [TODO] Handle lost-after-lost and partial-recovery case?
    // Undo that window reduction if the lost count is again down to the one that caused a reduction (reordered iso loss)
    if (m_lost_window && (m_loss_packets_lost - packets_lost >= 0)) {
        m_fractional_window += m_lost_window;  // add the reduction to the window again
        m_lost_window = 0;                     // can be done only once
        m_cc_state = cs_cong_avoid;            // restore the loss state
    }
    // Clear the in_loss state if in_loss and a real and vrtual rtt are passed
    if ((m_cc_state == cs_in_loss) && (packets_received + packets_lost - m_loss_packets_sent > 0) && (ts - m_loss_ts - m_vrtt >= 0)) {
        m_cc_state = cs_cong_avoid;                // set the loss state to avoid multiple reductions per RTT
        // keep all loss info for undo if later reordering is found (loss is reduced to m_loss_packets_lost again) 
    }
    // Reduce the window if the loss count is increased
    if ((m_cc_state != cs_in_loss) && (m_packets_lost - packets_lost < 0)) {
        m_lost_window = m_fractional_window / 2;  // remember the reduction
        m_fractional_window -= m_lost_window;     // reduce the window
        m_cc_state = cs_in_loss;                  // set the loss state to avoid multiple reductions per RTT
        m_loss_packets_sent = packets_sent;       // set when to end in_loss state
        m_loss_ts = ts;                           // set the loss timestampt to check if a virtRtt is passed
        m_loss_packets_lost = m_packets_lost;     // remember the previous packets_lost for the undo if needed
    }
    // Increase the window if not in-loss for all the non-CE ACKs
    count_tp acks = (packets_received - m_packets_received) - (packets_CE - m_packets_CE);
    if ((m_cc_state != cs_in_loss) && (acks > 0))
    {   // W[p] = W + acks / W * (srrt/vrtt)², but in the right order to not lose precision
        // W[µB] = W + acks * mtu² * 1000000² / W * (srrt/vrtt)²
        // correct order to prevent loss of precision 
        m_fractional_window += acks * m_packet_size * srtt * 1000000 / m_vrtt * m_packet_size * srtt / m_vrtt * 1000000 / m_fractional_window;
    }

    // Clear the in_cwr state if in_cwr and a real and vrtual rtt are passed
    if ((m_cc_state == cs_in_cwr) && (packets_received + packets_lost - m_cwr_packets_sent > 0) && (ts - m_cwr_ts - m_vrtt >= 0)) {
        m_cc_state = cs_cong_avoid;                // set the loss state to avoid multiple reductions per RTT
    }
    // Reduce the window if the CE count is increased, and if not in-loss and not in-cwr
    if ((m_cc_state == cs_cong_avoid) && (m_packets_CE - packets_CE < 0)) { // Todo: change == CA if other states are added
        m_fractional_window -= m_fractional_window * m_alpha >> (PROB_SHIFT + 1);   // reduce the window by a factor alpha/2
        m_cc_state = cs_in_cwr;                  // set the loss state to avoid multiple reductions per RTT
        m_cwr_packets_sent = packets_sent;       // set when to end in_loss state
        m_cwr_ts = ts;                           // set the cwr timestampt to check if a virtRtt is passed
    }

    // Updating dependant parameters
    m_pacing_rate = m_fractional_window / srtt;  // in B/s
    if (m_pacing_rate < m_min_rate)
        m_pacing_rate = m_min_rate;
    if (m_pacing_rate > m_max_rate)
        m_pacing_rate = m_max_rate;
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

    m_cc_ts = ts;
    m_packets_received = packets_received; // can NOT go down
    m_packets_CE = packets_CE;             // can NOT go down
    m_packets_lost = packets_lost;         // CAN go down
    m_packets_sent = packets_sent;         // can NOT go down
    m_error_L4S = error_L4S;
    inflight = packets_sent - m_packets_received - m_packets_lost;
    return true;
}

bool PragueCC::FrameACKReceived(   // call this when a frame ACK is received from peer
    count_tp packets_received,     // echoed_packet counter
    count_tp packets_CE,           // echoed CE counter
    count_tp packets_lost,         // echoed lost counter
    bool error_L4S)                // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!
{
    return true;
}

void PragueCC::DataReceivedSequence(  // call this every time when a data packet is received as a receiver
    ecn_tp ip_ecn,                    // IP.ECN field value
    count_tp packet_seq_nr)           // sequence number of the received packet 
{
    ip_ecn = ecn_tp(ip_ecn & ecn_ce);
    m_r_packets_received++;           // assuming no duplicates (by for instance the NW)
    count_tp skipped = packet_seq_nr - m_r_packets_received - m_r_packets_lost;
    if (skipped >= 0)
        m_r_packets_lost += skipped;  // 0 or more lost
    else if (m_r_packets_lost > 0)
        m_r_packets_lost--;           // reordered packet
    if (ip_ecn == ecn_ce)
    {
        m_r_packets_CE++;
    }
    else if (ip_ecn != ecn_l4s_id)
    {
        m_r_error_L4S = true;
    }
}

void PragueCC::DataReceived(   // call this when a data packet is received as a receiver and you can identify lost packets
    ecn_tp ip_ecn,             // IP.ECN field value
    count_tp packets_lost)     // packets skipped; can be optionally -1 to potentially undo a previous cwindow reduction 
{
    ip_ecn = ecn_tp(ip_ecn & ecn_ce);
    m_r_packets_received++;
    m_r_packets_lost += packets_lost;
    if (ip_ecn == ecn_ce)
    {
        m_r_packets_CE++;
    }
    else if (ip_ecn != ecn_l4s_id)
    {
        m_r_error_L4S = true;
    }
}

void PragueCC::ResetCCInfo()     // call this when there is a RTO detected
{
    m_cc_ts = Now();
    m_cc_state = cs_init;
    m_alpha_ts = m_cc_ts;
    m_alpha = 0;
    m_pacing_rate = m_init_rate;
    m_fractional_window = m_max_packet_size*1000000; // reset to 1 packet
    m_packet_burst = 1;
    m_packet_size = m_max_packet_size;
    m_packet_window = 1;
}

void PragueCC::GetTimeInfo(          // when the any-app needs to send a packet
    time_tp &timestamp,              // Own timestamp to echo by peer
    time_tp &echoed_timestamp,       // defrosted timestamp echoed to peer
    ecn_tp &ip_ecn)
{
    timestamp = Now();
    //echoed_timestamp = timestamp - m_ts_remote;  // if frozen
    echoed_timestamp = m_ts_remote;  // if not frozen
    if (m_error_L4S == true)
    {
        ip_ecn = ecn_not_ect;
    } else {
        ip_ecn = ecn_l4s_id;
    }
}

void PragueCC::GetCCInfo(     // when the sending-app needs to send a packet
    rate_tp &pacing_rate,     // rate to pace the packets
    count_tp &packet_window,  // the congestion window in number of packets
    count_tp &packet_burst,   // number of packets that can be paced at once (<250µs)
    size_tp &packet_size)     // the packet size to transmit
{
    pacing_rate = m_pacing_rate;
    packet_window = m_packet_window;
    packet_burst = m_packet_burst;
    packet_size = m_packet_size;
    // int sent = 0;
    // uint64_t sent_B = 0;
    // if (ns_per_B == 0 || ns_per_B > (1000.0*settings::max_burst_duration_us)/f.framelen()) {
    //     ns_per_B = (1000.0*settings::max_burst_duration_us)/f.framelen();
    // }
}

void PragueCC::GetACKInfo(       // when the receiving-app needs to send a packet
    count_tp &packets_received,  // packet counter to echo
    count_tp &packets_CE,        // CE counter to echo
    count_tp &packets_lost,      // lost counter to echo (if used)
    bool &error_L4S)             // bleached/error ECN status to echo
{
    packets_received = m_r_packets_received;
    packets_CE = m_r_packets_CE;
    packets_lost = m_r_packets_lost;
    error_L4S = m_r_error_L4S;
}

void PragueCC::GetCCInfoVideo( // when the sending app needs to send a frame        
    rate_tp &pacing_rate,      // rate to pace the packets
    size_tp &frame_size,       // the size of a single frame in Bytes
    count_tp &frame_window,    // the congestion window in number of frames
    count_tp &packet_burst,    // number of packets that can be paced at once (<250µs)
    size_tp &packet_size)      // the packet size to transmit
{
    
}
