#include <chrono>
#include "prague_cc.h"

// CUBIC consts and helpers

const uint16_t BETA = 717;                 // Beta used in Cube increase
const uint16_t BETA_SCALE = 1024;          // Beta scale
const uint16_t C_SCALED = 41;              // C used in Cube increase
const uint32_t TIME_SCALE = 250;           // Time unit in # of [us]
const uint64_t RTT_SCALED = (1 << 10) * (1000 / TIME_SCALE) * (1000 / TIME_SCALE) * (1000 / TIME_SCALE) / 10; // Cube scaling factor

int fls(int x)
{
    int r = 32;

    if (!x)
        return 0;
    if (!(x & 0xffff0000u)) {
        x <<= 16;
        r -= 16;
    }
    if (!(x & 0xff000000u)) {
        x <<= 8;
        r -= 8;
    }
    if (!(x & 0xf0000000u)) {
        x <<= 4;
        r -= 4;
    }
    if (!(x & 0xc0000000u)) {
        x <<= 2;
        r -= 2;
    }
    if (!(x & 0x80000000u)) {
        x <<= 1;
        r -= 1;
    }
    return r;
}

uint32_t fls64(uint64_t x)
{
    uint32_t h = x >> 32;
    if (h)
        return fls(h) + 32;
    return fls(x);
}

uint32_t CubicRoot(uint64_t a)
{
    uint32_t x, b, shift;
    static const uint8_t v[] = {
        /* 0x00 */    0,   54,   54,   54,  118,  118,  118,  118,
        /* 0x08 */  123,  129,  134,  138,  143,  147,  151,  156,
        /* 0x10 */  157,  161,  164,  168,  170,  173,  176,  179,
        /* 0x18 */  181,  185,  187,  190,  192,  194,  197,  199,
        /* 0x20 */  200,  202,  204,  206,  209,  211,  213,  215,
        /* 0x28 */  217,  219,  221,  222,  224,  225,  227,  229,
        /* 0x30 */  231,  232,  234,  236,  237,  239,  240,  242,
        /* 0x38 */  244,  245,  246,  248,  250,  251,  252,  254,
    };

    b = fls64(a);
    if (b < 7) {
        return ((uint32_t)v[(uint32_t)a] + 35) >> 6;
    }

    b = ((b * 84) >> 8) - 1;
    shift = (a >> (b * 3));

    x = ((uint32_t)(((uint32_t)v[shift] + 10) << b)) >> 6;
    x = (2 * x + (uint32_t)(a / ((uint64_t)x * (uint64_t)(x - 1))));
    x = ((x * 341) >> 10);
    return x;
}

// Prague consts and methods

const time_tp BURST_TIME = 250;            // 250 us
const time_tp REF_RTT = 25000;             // 25ms
const uint8_t PROB_SHIFT = 20;             // enough as max value that can control up to 100Gbps with r [Mbps] = 1/p - 1, p = 1/(r + 1) = 1/100001
const prob_tp MAX_PROB = 1 << PROB_SHIFT;  // with r [Mbps] = 1/p - 1 = 2^20 Mbps = 1Tbps
const uint8_t ALPHA_SHIFT = 4;             // >> 4 is divide by 16
const count_tp MIN_PKT_BURST = 1;          // 1 packet
const count_tp MIN_PKT_WIN = 2;            // 2 packets

PragueCC::PragueCC(
    size_tp max_packet_size,
    fps_tp fps,
    time_tp frame_budget,
    rate_tp init_rate,
    count_tp init_window,
    rate_tp min_rate,
    rate_tp max_rate)
{
    m_start_ref = 0;
    time_tp ts_now = Now();
// parameters
    m_init_rate = init_rate;
    m_init_window = window_tp(init_window)*max_packet_size * 1000000;
    m_min_rate = min_rate;
    m_max_rate = max_rate;
    m_max_packet_size = max_packet_size;
    m_frame_interval = fps ? 1000000 / fps : 0;
    m_frame_budget = frame_budget;
    if (m_frame_budget > m_frame_interval)
        m_frame_budget = m_frame_interval;
// both end variables
    m_ts_remote = 0;    // to keep the frozen timestamp from the peer, and echo it back defrosted
    m_rtt = 0;          // last reported rtt (only for stats)
    m_rtt_min = 1000000;// minimum report rtt (for cubic)
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
    // Cubic
    m_cubic_epoch_start = 0;
    m_cubic_last_max_fracwin = 0;
    m_cubic_K = 0;
    m_cubic_origin_fracwin = 0;
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
    m_cca_mode = cca_prague_win;
    m_alpha = 0;
    m_pacing_rate = init_rate;
    m_fractional_window = m_init_window;
    m_packet_size = m_pacing_rate * REF_RTT / 1000000 / MIN_PKT_WIN;            // B/p = B/s * 25ms/burst / 2p/window
    if (m_packet_size < PRAGUE_MINMTU)
        m_packet_size = PRAGUE_MINMTU;
    if (m_packet_size > m_max_packet_size)
        m_packet_size = m_max_packet_size;
    m_packet_burst = count_tp(m_pacing_rate * BURST_TIME / 1000000 / m_packet_size);  // p = B/s * 250µs / B/p
    if (m_packet_burst < MIN_PKT_BURST) {
        m_packet_burst = MIN_PKT_BURST;
    }
    m_packet_window = count_tp((m_fractional_window / 1000000 + m_packet_size - 1) / m_packet_size);
    if (m_packet_window < MIN_PKT_WIN) {
        m_packet_window = MIN_PKT_WIN;
    }
}

PragueCC::~PragueCC()
{}

time_tp PragueCC::Now() // Returns number of µs since first call
{
    // Checks if now==0; skip this value used to check uninitialized timepstamp
    if (m_start_ref == 0) {
        m_start_ref = time_tp(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if (m_start_ref == 0) {
            m_start_ref = -1;  // init m_start_ref with -1 to avoid next now to be less than this value
        }
        return 1; // make sure we don't return less than or equal to 0
    }
    time_tp now = time_tp(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()) - m_start_ref;
    if (now == 0) {
        return 1; // make sure we don't return 0
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
    m_ts_remote = ts - timestamp;  // freeze the remote timestamp
    //m_ts_remote = timestamp;  // do not freeze the remote timestamp
    m_rtt = ts - echoed_timestamp;
    m_rtt_min = (m_rtt_min > m_rtt) ? m_rtt : m_rtt_min;
    if (m_cc_state != cs_init)
        m_srtt += (m_rtt - m_srtt) >> 3;
    else
        m_srtt = m_rtt;
    m_vrtt = (m_srtt > REF_RTT) ? m_srtt : REF_RTT;
    m_r_prev_ts = timestamp;
    return true;
}

bool PragueCC::ACKReceived(    // call this when an ACK (or a Frame ACK) is received from peer. Returns true if this is a newer ACK, false if this is an old ACK
    count_tp packets_received, // echoed_packet counter
    count_tp packets_CE,       // echoed CE counter
    count_tp packets_lost,     // echoed lost counter
    count_tp packets_sent,     // local counter of packets sent up to now, an RTT is reached if remote ACK packets_received+packets_lost
    bool error_L4S,            // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!
    count_tp &inflight)        // how many packets (or frames if fps) are in flight after the ACKed
{
    if ((m_packets_received - packets_received > 0) || (m_packets_CE - packets_CE > 0)) // this is an older or invalid ACK (these counters can't go down)
        return false;

    time_tp pacing_interval = m_packet_size * 1000000 / m_pacing_rate; // calculate the max expected rtt from pacing
    //printf("FrW: %ld, SRTT: %d, Pacing interval: %ld, packet_size: %ld, packet_burst: %d, pacing_rate: %ld\n", m_fractional_window, m_srtt, m_packet_size * 1000000 * m_packet_burst / m_pacing_rate, m_packet_size, m_packet_burst, m_pacing_rate);
    time_tp srtt = (m_srtt);// > pacing_interval) ? m_srtt : pacing_interval; // take into account the pacing delay
    if (m_cc_state == cs_init)  // initialize the window with the initial pacing rate
    {
        m_fractional_window = srtt * m_pacing_rate;
        m_cc_state = cs_cong_avoid;
    }
    if ((srtt <= 2000) || (srtt <= pacing_interval)) {
        // keep rate stable when large dip in srtt
        m_cca_mode = cca_prague_rate;
    }
    else {
        // keep rate stable when large jump in srtt
        if (m_cca_mode == cca_prague_rate)
            m_fractional_window = srtt * m_pacing_rate;
        m_cca_mode = cca_prague_win;
    }
    time_tp ts = Now();
    // Update alpha if both a window and a virtual rtt are passed
    if ((packets_received + packets_lost - m_alpha_packets_sent > 0) && (ts - m_alpha_ts - m_vrtt >= 0)) {
    //if ((packets_received - m_alpha_packets_received + packets_lost - m_alpha_packets_lost > max(2, m_fractional_window / m_packet_size / 1000000))
    //    && (now() - m_prev_cycle > 25000)) {
        // prob_tp prob = (packets_CE - m_alpha_packets_CE) << PROB_SHIFT / (packets_received - m_alpha_packets_received);
        prob_tp prob = (prob_tp(packets_CE - m_alpha_packets_CE) << PROB_SHIFT) / (packets_received - m_alpha_packets_received);
        m_alpha += ((prob - m_alpha) >> ALPHA_SHIFT);
        m_alpha = (m_alpha > MAX_PROB) ? MAX_PROB : m_alpha;
        m_alpha_packets_sent = packets_sent;
        m_alpha_packets_CE = packets_CE;
        m_alpha_packets_received = packets_received;
        m_alpha_ts = ts;
    }
    // Undo that window reduction if the lost count is again down to the one that caused a reduction (reordered iso loss)
    if (m_lost_window && (m_loss_packets_lost - packets_lost >= 0)) {
        if (m_cca_mode == cca_prague_rate) {
            m_pacing_rate += m_lost_rate;          // add the reduction to the rate again
            m_lost_rate = 0;                       // can be done only once
        } else {
            m_fractional_window += m_lost_window;  // add the reduction to the window again
            m_lost_window = 0;                     // can be done only once
        }
        m_cca_mode = m_loss_cca;                   // restore the cca
        m_cc_state = cs_cong_avoid;                // restore the loss state
    }
    // Clear the in_loss state if in_loss and a real and vrtual rtt are passed
    if ((m_cc_state == cs_in_loss) && (packets_received + packets_lost - m_loss_packets_sent > 0) && (ts - m_loss_ts - m_vrtt >= 0)) {
        m_cc_state = cs_cong_avoid;                // set the loss state to avoid multiple reductions per RTT
        // keep all loss info for undo if later reordering is found (loss is reduced to m_loss_packets_lost again)
    }
    // Reduce the window if the loss count is increased
    if ((m_cc_state != cs_in_loss) && (m_packets_lost - packets_lost < 0)) {
        if (m_cca_mode == cca_prague_win) {
            m_lost_window = m_fractional_window / 2;  // remember the reduction
            m_fractional_window -= m_lost_window;     // reduce the window
        } else if (m_cca_mode == cca_cubic) {
            m_cubic_epoch_start = 0;
            if (m_fractional_window < m_cubic_last_max_fracwin)
                m_cubic_last_max_fracwin = (m_fractional_window * (BETA_SCALE + BETA)) / (2 * BETA_SCALE);
            else
                m_cubic_last_max_fracwin = m_fractional_window;

            m_lost_window = m_fractional_window * (BETA_SCALE - BETA) / BETA_SCALE;  // remember the reduction
            m_fractional_window -= m_lost_window;     // reduce the window
        }
        else { // (m_cca_mode == cca_prague_rate)
            m_lost_rate = m_pacing_rate / 2;          // remember the reduction
            m_pacing_rate -= m_lost_rate;             // reduce the rate
        }
        m_cc_state = cs_in_loss;                  // set the loss state to avoid multiple reductions per RTT
        m_loss_cca = m_cca_mode;
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
        if (m_cca_mode == cca_prague_win) {
            m_fractional_window += acks * m_packet_size * srtt * 1000000 / m_vrtt * m_max_packet_size * srtt / m_vrtt * 1000000 / m_fractional_window;
        } else if (m_cca_mode == cca_cubic) { // Cubic code is still unfinished. Don't use for now.
            // Linux Cubic implementation includes further (1) check to stop
            // increasing cwnd when it reaches last_Wmax in a short period,
            // and (2) the boost for SS mode to make m_cubic_cnt >= 20
            // We skip those for now...
            time_tp now = Now();
            if (m_cubic_epoch_start == 0) {
                m_cubic_epoch_start = now;
                if (m_cubic_last_max_fracwin <= m_fractional_window) {
                    m_cubic_K = 0;
                    m_cubic_origin_fracwin = m_fractional_window;
                } else {
                    m_cubic_K = CubicRoot(((m_cubic_last_max_fracwin - m_fractional_window) * RTT_SCALED / m_packet_size / C_SCALED));
                    m_cubic_origin_fracwin = m_cubic_last_max_fracwin;
                }
                //printf("K: %u, last_max: %lu, frac_win: %lu\n", m_cubic_K, m_cubic_last_max_fracwin, m_fractional_window);
            }
            uint32_t t = ((time_tp)(now - m_cubic_epoch_start + m_rtt_min) / TIME_SCALE);                            // t = (now - start + RTT)
            uint64_t offs = (t < m_cubic_K) ? (m_cubic_K - t) : (t - m_cubic_K);                                     // t - K
            uint64_t delta = (C_SCALED * offs * offs * offs) * m_packet_size / RTT_SCALED;                           // mtu*c/rtt*(t-K)^3
            uint64_t target = (t < m_cubic_K) ? (m_cubic_origin_fracwin - delta) : (m_cubic_origin_fracwin + delta); // mtu*c/rtt*(t-K)^3 + W_max
            uint64_t count = (target > m_fractional_window) ? (target - m_fractional_window) : (1);
            //printf("time: %d, K: %u, offs: %lu, delta: %lu/%lu, pkt: %lu, rtt_min: %d, RTT_SCALED: %lu, count: %lu, target: %lu, fw: %lu\n", t, m_cubic_K, offs, (C_SCALED * offs * offs * offs) * m_packet_size, delta, m_packet_size, m_rtt_min, RTT_SCALED, count, target, m_fractional_window);
            m_fractional_window += acks * m_max_packet_size * srtt * 1000000 / m_vrtt * srtt / m_vrtt * count / m_fractional_window;
        } else {
            m_pacing_rate += acks * m_max_packet_size * 1000000 / m_vrtt * m_packet_size / m_vrtt * 1000000 / m_pacing_rate;
        }
    }

    // Clear the in_cwr state if in_cwr and a real and vrtual rtt are passed
    if ((m_cc_state == cs_in_cwr) && (packets_received + packets_lost - m_cwr_packets_sent > 0) && (ts - m_cwr_ts - m_vrtt >= 0)) {
        m_cc_state = cs_cong_avoid;                // set the loss state to avoid multiple reductions per RTT
    }
    // Reduce the window if the CE count is increased, and if not in-loss and not in-cwr
    if ((m_cc_state == cs_cong_avoid) && (m_packets_CE - packets_CE < 0)) {
        if (m_cca_mode == cca_cubic)
            m_cca_mode = cca_prague_win; // use Prague in case of marks
        if (m_cca_mode == cca_prague_win) {
            m_fractional_window -= m_fractional_window * m_alpha >> (PROB_SHIFT + 1);   // reduce the window by a factor alpha/2
        } else {
            m_pacing_rate -= m_pacing_rate * m_alpha >> (PROB_SHIFT + 1);   // reduce the rate by a factor alpha/2
        }
        m_cc_state = cs_in_cwr;                  // set the loss state to avoid multiple reductions per RTT
        m_cwr_packets_sent = packets_sent;       // set when to end in_loss state
        m_cwr_ts = ts;                           // set the cwr timestampt to check if a virtRtt is passed
    }

    // Updating dependant parameters
    if (m_cca_mode != cca_prague_rate)
        m_pacing_rate = m_fractional_window / srtt;  // in B/s
    if (m_pacing_rate < m_min_rate)
        m_pacing_rate = m_min_rate;
    if (m_pacing_rate > m_max_rate)
        m_pacing_rate = m_max_rate;
    m_fractional_window = m_pacing_rate * srtt;  // in uB

    if (m_fractional_window == 0)
        m_fractional_window = 1;

    size_tp old_packet_size = m_packet_size;
    m_packet_size = m_pacing_rate * m_vrtt / 1000000 / MIN_PKT_WIN;            // B/p = B/s * 25ms/burst / 2p/burst
    if (m_packet_size < PRAGUE_MINMTU)
        m_packet_size = PRAGUE_MINMTU;
    if (m_packet_size > m_max_packet_size)
        m_packet_size = m_max_packet_size;
    if (m_packet_size != old_packet_size) {
        m_cubic_K = m_cubic_K * CubicRoot(old_packet_size) / CubicRoot(m_packet_size);
    }
    m_packet_burst = count_tp(m_pacing_rate * BURST_TIME / 1000000 / m_packet_size);  // p = B/s * 250µs / B/p
    if (m_packet_burst < MIN_PKT_BURST) {
        m_packet_burst = MIN_PKT_BURST;
    }
    m_packet_window = count_tp((m_fractional_window/1000000 + m_packet_size -1)/m_packet_size);
    if (m_packet_window < MIN_PKT_WIN) {
        m_packet_window = MIN_PKT_WIN;
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

// Can this be combined with the normal ACKReceived?
/*bool PragueCC::FrameACKReceived(   // call this when a frame ACK is received from peer
    count_tp packets_received,     // echoed_packet counter
    count_tp packets_CE,           // echoed CE counter
    count_tp packets_lost,         // echoed lost counter
    bool error_L4S)                // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!
{
    
    return true;
}*/

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
    m_cca_mode = cca_prague_win;
    m_alpha_ts = m_cc_ts;
    m_alpha = 0;
    m_pacing_rate = m_init_rate;
    m_fractional_window = m_max_packet_size*1000000; // reset to 1 packet
    m_packet_burst = MIN_PKT_BURST;
    m_packet_size = m_max_packet_size;
    m_packet_window = MIN_PKT_WIN;
}

void PragueCC::GetTimeInfo(          // when the any-app needs to send a packet
    time_tp &timestamp,              // Own timestamp to echo by peer
    time_tp &echoed_timestamp,       // defrosted timestamp echoed to peer
    ecn_tp &ip_ecn)
{
    timestamp = Now();
    if (m_ts_remote)
        echoed_timestamp = timestamp - m_ts_remote;  // if frozen
    else
        echoed_timestamp = 0;
    //echoed_timestamp = m_ts_remote;  // if not frozen
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
}

void PragueCC::GetCCInfoVideo( // when the sending app needs to send a frame
    rate_tp &pacing_rate,      // rate to pace the packets
    size_tp &frame_size,       // the size of a single frame in Bytes
    count_tp &frame_window,    // the congestion window in number of frames
    count_tp &packet_burst,    // number of packets that can be paced at once (<250µs)
    size_tp &packet_size)      // the packet size to transmit
{
    pacing_rate = m_pacing_rate;
    packet_burst = m_packet_burst;
    packet_size = m_packet_size;
    frame_size = m_pacing_rate * m_frame_budget / 1000000;
    frame_window = m_packet_window * m_packet_size / frame_size;
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
