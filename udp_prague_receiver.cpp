// udp_prague_receiver.cpp:
// An example of a (dummy data) UDP receiver that needs to send ACKs for a congestion controlled UDP sender
//

#include <string>
#include "prague_cc.h"
#include "udpsocket.h"
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

    if (app.connect) { // send a trigger ACK packet, otherwise just wait for data
        pragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
        pragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);
        ack_msg.hton();  // swap byte order if needed
        size_tp bytes_sent = us.Send((char*)(&ack_msg), sizeof(ack_msg), new_ecn);
        app.ExitIf(bytes_sent != sizeof(ack_msg), "invalid ack packet length sent");
    }

    while (true) {
        // Wait for an incoming data message
        ecn_tp rcv_ecn = ecn_not_ect;
        size_tp bytes_received = us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, 0);

        while (bytes_received == 0) {   // repeat if timeout or interrupted
            bytes_received = us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, 0);
        }

        // Extract the data message
        now = pragueCC.Now();
        data_msg.hton();  // swap byte order
        app.LogRecvData(now, data_msg.timestamp, data_msg.echoed_timestamp, data_msg.seq_nr, bytes_received);

        // Pass the relevant data to the PragueCC object:
        pragueCC.PacketReceived(data_msg.timestamp, data_msg.echoed_timestamp);
        pragueCC.DataReceivedSequence(rcv_ecn, data_msg.seq_nr);

        // Return a corresponding acknowledge message
        pragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
        pragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);

        app.LogSendACK(now, ack_msg.timestamp, ack_msg.echoed_timestamp, data_msg.seq_nr, sizeof(ack_msg),
                       ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);

        ack_msg.hton();  // swap byte order if needed
        size_tp bytes_sent = us.Send((char*)(&ack_msg), sizeof(ack_msg), new_ecn);
        app.ExitIf(bytes_sent != sizeof(ack_msg), "invalid ack packet length sent");
    }
}
