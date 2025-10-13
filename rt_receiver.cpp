#include "app_stuff.h"
#include "pkt_format.h"
#include "prague_cc.h"
#include "receiver_base.h"

class RTReceiver : private Receiver {
public:
  RTReceiver(AppStuff app) : Receiver(app) {
    rfc8888_acktime = prague_cc.Now() + app.rfc8888_ackperiod;
  }

  void Run() override {
    if (app.connect) {
      sendTrigger();
    }

    while (true) {
      ecn_tp rcv_ecn = ecn_not_ect;
      size_tp bytes_received = receiveBytes(rcv_ecn);

      // Handle incoming packets
      if (bytes_received != 0) {
        now = prague_cc.Now();
        data_msg->hton();
        app.LogRecvData(now, data_msg->timestamp, data_msg->echoed_timestamp,
                        data_msg->seq_nr, bytes_received);

        uint16_t seq_idx = data_msg->seq_nr % PKT_BUFFER_SIZE;
        if (start_seq == end_seq) {
          start_seq = data_msg->seq_nr;
          end_seq = data_msg->seq_nr + 1;
        } else {
          // [start_seq, end_seq) data will be ACKed
          if (start_seq - data_msg->seq_nr <= 0 &&
              start_seq + PKT_BUFFER_SIZE - data_msg->seq_nr > 0 &&
              data_msg->seq_nr + 1 - end_seq > 0) {
            end_seq = data_msg->seq_nr + 1;
          } else if (end_seq - data_msg->seq_nr > 0 &&
                     end_seq - PKT_BUFFER_SIZE - data_msg->seq_nr <= 0 &&
                     data_msg->seq_nr - start_seq < 0) {
            start_seq = data_msg->seq_nr;
          }
        }
        if (!(recvseq[seq_idx] == rcv_recv)) {
          recvtime[seq_idx] = now;
          recvecn[seq_idx] = ecn_tp(rcv_ecn & ecn_ce);
          recvseq[seq_idx] = rcv_recv;
        } else {
          recvecn[seq_idx] = (rcv_ecn == ecn_ce) ? ecn_ce : recvecn[seq_idx];
        }

        prague_cc.PacketReceived(data_msg->timestamp,
                                 data_msg->echoed_timestamp);
        prague_cc.DataReceivedSequence(rcv_ecn, data_msg->seq_nr);
      }

      now = prague_cc.Now();

      // Early return when no ACK needs to be sent
      if (rfc8888_acktime - now > 0)
        return;

      // Send ACKs
      while (start_seq != end_seq) {
        uint16_t rfc8888_acksize = rfc8888_ackmsg.set_stat(
            start_seq, end_seq, now, recvtime, recvecn, recvseq, app.max_pkt);
        app.ExitIf(us.Send((char *)(&rfc8888_ackmsg), rfc8888_acksize,
                           ecn_l4s_id) != rfc8888_acksize,
                   "Invalid RFC8888 ack packetlength sent.");
        app.LogSendRFC8888ACK(now, data_msg->seq_nr, rfc8888_acksize,
                              htonl(rfc8888_ackmsg.begin_seq),
                              htons(rfc8888_ackmsg.num_reports),
                              rfc8888_ackmsg.report);
      }

      rfc8888_acktime = now + app.rfc8888_ackperiod;
    }
  }

private:
  rfc8888ack_t rfc8888_ackmsg;
  count_tp start_seq = 0, end_seq = 0;
  time_tp rfc8888_acktime; // Periodic timer for when to send RTCP-like feedback
  time_tp recvtime[PKT_BUFFER_SIZE] = {0};          // Stores arrival timestamps
  ecn_tp recvecn[PKT_BUFFER_SIZE] = {ecn_not_ect};  // Recorded ECN markings
  pktrecv_tp recvseq[PKT_BUFFER_SIZE] = {rcv_init}; // Recorded sequence numbers

  time_tp getWaitTime() {
    now = prague_cc.Now();
    if (start_seq == end_seq)
      return 0;

    return ((rfc8888_acktime - now > 0) ? (rfc8888_acktime - now) : 1);
  }

  size_tp receiveBytes(ecn_tp &rcv_ecn) {
    time_tp waitTime = getWaitTime();
    size_tp bytes_received = 0;
    do { // repeat if timeout or interrupted
      bytes_received =
          us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, waitTime);
    } while (bytes_received == 0 && waitTime == 0);
    return bytes_received;
  }
};

int main(int argc, char **argv) {
  RTReceiver receiver(AppStuff(false, argc, argv));

  receiver.Run();
}
