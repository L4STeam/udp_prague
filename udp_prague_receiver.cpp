// udp_prague_receiver.cpp:
// An example of a (dummy data) UDP receiver that needs to send ACKs for a congestion controlled UDP sender
//

#include <string>
#include "udpsocket.h"
#include "app_stuff.h"
#include "pkt_format.h"

int main(int argc, char **argv)
{
    AppStuff app(false, argc, argv); // initialize the app

    // Create a UDP socket
    UDPSocket us;
    if (app.connect)
        us.Connect(app.rcv_addr, (uint16_t)app.rcv_port);
    else
        us.Bind(app.rcv_addr, (uint16_t)app.rcv_port);

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
