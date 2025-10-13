#include "app_stuff.h"
#include "prague_cc.h"
#include "receiver_base.h"

class PragueReceiver : private Receiver {
public:
  PragueReceiver(AppStuff app) : Receiver(app) {}

  void Run() override {
    if (app.connect) {
      sendTrigger();
    }

    while (true) {
      ecn_tp rcv_ecn = ecn_not_ect;
      size_tp bytes_received = receiveBytes(rcv_ecn);

      if (!bytes_received)
        continue;

      handlePacket(bytes_received, rcv_ecn);
      sendAck();
    }
  }

private:
  size_tp receiveBytes(ecn_tp &rcv_ecn) {
    return us.Receive(receivebuffer, sizeof(receivebuffer), rcv_ecn, 0);
  }

  void handlePacket(size_tp bytes_received, ecn_tp rcv_ecn) {
    data_msg->hton();
    now = prague_cc.Now();
    app.LogRecvData(now, data_msg->timestamp, data_msg->echoed_timestamp,
                    data_msg->seq_nr, bytes_received);

    // Pass the relevant data to the PragueCC object:
    prague_cc.PacketReceived(data_msg->timestamp, data_msg->echoed_timestamp);
    prague_cc.DataReceivedSequence(rcv_ecn, data_msg->seq_nr);

    ack_msg.ack_seq = data_msg->seq_nr;
    prague_cc.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
    prague_cc.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE,
                         ack_msg.packets_lost, ack_msg.error_L4S);
  }

  void sendAck() {
    app.LogSendACK(now, ack_msg.timestamp, ack_msg.echoed_timestamp,
                   data_msg->seq_nr, sizeof(ack_msg), ack_msg.packets_received,
                   ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);

    ack_msg.set_stat();
    app.ExitIf(us.Send((char *)&ack_msg, sizeof(ack_msg), new_ecn) !=
                   sizeof(ack_msg),
               "Invalid ack packet length sent.\n");
  }
};

int main(int argc, char **argv) {
  PragueReceiver receiver(AppStuff(false, argc, argv));

  receiver.Run();
}
