//  receiver_base.h:
//  An example of a (dummy data) UDP receiver that needs to send ACKs for a
//  congestion controlled UDP sender

#include "app_stuff.h"
#include "pkt_format.h"
#include "prague_cc.h"
#include "udpsocket.h"

class Receiver {
public:
  Receiver(AppStuff app) : app(app) {
    if (app.connect)
      us.Connect(app.rcv_addr, app.rcv_port);
    else
      us.Bind(app.rcv_addr, app.rcv_port);
    data_msg = (struct datamessage_t *)(receivebuffer);
    now = prague_cc.Now();
  }
  virtual ~Receiver() = default;

  virtual void Run() = 0;

protected:
  AppStuff app;
  UDPSocket us;
  PragueCC prague_cc;
  datamessage_t *data_msg;
  ackmessage_t ack_msg;
  char receivebuffer[BUFFER_SIZE];
  ecn_tp new_ecn;
  time_tp now;

  void sendTrigger() {
    prague_cc.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
    prague_cc.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE,
                         ack_msg.packets_lost, ack_msg.error_L4S);
    ack_msg.set_stat();
    app.ExitIf(us.Send((char *)(&ack_msg), sizeof(ack_msg), new_ecn) !=
                   sizeof(ack_msg),
               "Invalid ack packet length sent.\n");
  }
};
