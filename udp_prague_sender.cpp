// udp_prague_sender.cpp:
// An example of a (dummy data) UDP sender that needs to receive ACKs from a UDP receiver for congestion control
//

#include <string>
#include "prague_cc.h"
#include "udpsocket.h"
//#include "icmpsocket.h" TODO: optimize MTU detection
#include "app_stuff.h"

#define BUFFER_SIZE 8192       // in bytes (depending on MTU)

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
    AppStuff app(true, argc, argv); // initialize the app

    // Find maximum MTU can be used
    // ICMPSocket icmps(rcv_addr);
    // max_pkt = icmps.mtu_discovery(150, max_pkt, 1000000, 1);

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

    ecn_tp rcv_ecn;
    size_tp bytes_received;

    if (!app.connect)  // wait for a trigger packet, otherwise just start sending
        do {
            bytes_received = us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, 0);
        } while (bytes_received == 0);

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
            app.LogSendData(now, data_msg.timestamp, data_msg.echoed_timestamp, seqnr, packet_size, 
                pacing_rate, packet_window, packet_burst, inflight, inburst, nextSend);
            data_msg.hton();
            size_tp bytes_sent = us.Send((char*)(&data_msg), packet_size, new_ecn);
            app.ExitIf(bytes_sent != packet_size, "invalid data packet length sent");
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
        do {
            timeout = (waitTimeout - now > 0) ? (waitTimeout - now) : 1;
            bytes_received = us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, timeout);
            now = pragueCC.Now();
        } while ((bytes_received == 0) && (waitTimeout - now > 0));
        if (bytes_received >= ssize_t(sizeof(ack_msg))) {
            ack_msg.hton();
            pragueCC.PacketReceived(ack_msg.timestamp, ack_msg.echoed_timestamp);
            pragueCC.ACKReceived(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, seqnr, ack_msg.error_L4S, inflight);
            app.LogRecvACK(now, ack_msg.timestamp, ack_msg.echoed_timestamp, seqnr, bytes_received,
                ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S,
                pacing_rate, packet_window, packet_burst, inflight, inburst, nextSend);
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
