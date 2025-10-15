***We will be working on this at the IETF #124 (1th till 7th of Nov 2025).
Open invite for UDP and QUIC stack/app developers to integrate it in your code.***

# Some basic info and instructions (to improve later):

## What is it
UDP-Prague is fully compatible with TCP-Prague (in Linux and Apple), both in terms of convergence of rate and the responsiveness/inertia compromise.
It consists only of the **prague_cc.h** file and the **prague_cc.cpp** file that have no dependencies (you only need to provide a monotonic time for Now()).
It currently has been running on Windows, Linux, Apple and FreeBSD but should be compilable on any C++ compiler on any platform.
We try to get to a common stable API (currently at this stage still evolvable), so the CC can further evolve independently and should be easy to get the latest version in your app.
It can work in 2 modes.

### Continuous streaming mode (aka UDP-Prague)
Like TCP, but possible to adapt the source(s) of the data directly (like the CBR of a real-time encoder). A single pacing rate, window and micro-burst size is given that can be used and divided over different in-app streams.
We also included an example sender and receiver for this below. They run by default as a server. Use the -c option on the one that you want to connect as a client.

### Frame mode (aka RT-Prague)
To make it RT-Prague. Just provide an "fps" frame rate in Hz and "frame_budget" time in µs. The frame budget is the time you want to allocate to send the frame over. In continuous mode that would be 1/fps, but here it can be shorter, so leaving pauses in between frames. It assumes that each frame can be reasonable equal (so no full I-frames, only P-frames). This mode can reduce the throughput (depends on the bottleneck), but should further reduce the photon to photon latency for very interactive apps. If an fps and frame budget is provided, the GetCCVideoInfo() will tell you an extra output giving the frame size that the encoder should target for the next frame. No full support or example yet, but can be worked on, on request.

## How to use continuous mode (examples)
Use the PragueCC object as follows in a sender and a receiver in continuous mode. The same object can be used for bidirectionally data sending too.

### You need to carry this between Sender (sends data_msg) and Reciever (sends ack_msg)
These message fields are provided and used by PragueCC for unidirectional data transfers, and need to be exchanged between both ends. For bidirectional data both need to be combined in a single bidirectional message format as described in the comments. Alternative timing, sequence and counter feedback schemes can be used... This is just an example format.
```
struct datamessage_t {
    time_tp timestamp;	       // timestamp from peer
    time_tp echoed_timestamp;  // echoed_timestamp can be used to calculate the RTT
    count_tp seq_nr;           // packet sequence number, should start with 1 and increase monotonic with packets sent
    // ACK fields for receiving data and/or payload comes here...
};
struct ackmessage_t {
    time_tp timestamp;	       // timestamp from peer, freeze and keep this time
    time_tp echoed_timestamp;  // echoed_timestamp can be used to calculate the RTT
    // fields for data messages can come in here...
    count_tp packets_received; // echoed_packet counter
    count_tp packets_CE;       // echoed CE counter
    count_tp packets_lost;     // echoed lost counter
    bool error_L4S;            // receiver found a bleached/error ECN; stop using L4S_id on the sending packets!
    // payload for bidirectional data can come here...
};
```

### You want to send data as a "Sender"
Simplified flow essentials for a Prague congestion controlled sender. The full runnable code is in **udp_prague_sender.cpp** (a single file that compiles to an executable).
```
int main()
{
    opensocket(); // connect to the server ip/port
    PragueCC pragueCC;    // create a PragueCC object. Using default parameters for the Prague CC in line with TCP_Prague
    // allocate CC variables
    time_tp nextSend = pragueCC.Now();
    count_tp seqnr = 1;
    count_tp inflight = 0;
    rate_tp pacing_rate;
    count_tp packet_window;
    count_tp packet_burst;
    size_tp packet_size;
    ecn_tp snd_ecn, rcv_ecn;
    pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size); // and retrieve them
    while (true) {
        // allocate pacing variables
        count_tp inburst = 0;
        time_tp timeout = 0;
        time_tp startSend = 0;
        time_tp now = pragueCC.Now();
        while ((inflight < packet_window) && (inburst < packet_burst) && (nextSend <= now)) {
            // send messages with the retrieved timestamps and snd_ecn
            pragueCC.GetTimeInfo(data_msg.timestamp, data_msg.echoed_timestamp, snd_ecn);
            if (startSend == 0)
                startSend = now;
            data_msg.seq_nr = seqnr;
            sendto_ecn(data_msg, snd_ecn);
            inburst++;
            inflight++;
            seqnr++;
        }
        if (startSend != 0) // we have send some data
            nextSend = startSend + packet_size * inburst * 1000000 / pacing_rate;
        time_tp waitTimeout = (inflight < packet_window) ? nextSend : pragueCC.Now() + 1000000;
        do {
            timeout = waitTimeout - pragueCC.Now();
            bytes_received = recvfrom_ecn_timeout(ack_msg, rcv_ecn, timeout);
        } while ((waitTimeout > pragueCC.Now()) && (bytes_received < 0));
        if ((bytes_received > 0) && isvalid(ack_msg)) {
            pragueCC.PacketReceived(ack_msg.timestamp, ack_msg.echoed_timestamp);
            pragueCC.ACKReceived(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, seqnr, ack_msg.error_L4S, inflight);
        }
        else // timeout, reset state
            if (inflight >= packet_window)
                pragueCC.ResetCCInfo();
        pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
    }
}
```

### You need to ACK data as a "Receiver"
Simplified flow essentials for a receiver that only ACKs (here every packet, but can skip x packets as in delayed ACKs in TCP). The full runnable code is in **udp_prague_receiver.cpp** (a single file that compiles to an executable).
```
int main()
{
    opensocket(); // bind to listening port
    PragueCC pragueCC;    // create a PragueCC object. No parameters needed if only ACKs are sent
    while (true) {
        recvfrom_ecn_timeout(data_msg, rcv_ecn, infinite_timeout, client);

        // Pass the relevant data to the PragueCC object:
        pragueCC.PacketReceived(data_msg.timestamp, data_msg.echoed_timestamp);
        pragueCC.DataReceivedSequence(rcv_ecn, data_msg.seq_nr);

        // Return a corresponding acknowledge message
        pragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, snd_ecn); // get timestamps and ecn bits to use
        pragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);
        sendtoecn(ack_msg, snd_ecn, client); // if snd_ecn changed, it will set the new bits in the socket option
    }
}
```
