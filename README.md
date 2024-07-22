***We are currently working on this at the IETF #120, for the rest of this week (22th till 26th of July). You can find us in the "Shared Workspace" on the 4th floor Bighton/Constable room.
Open invite for UDP and QUIC stack/app developers to integrate it in your code. We currently are focusing on having UDP-Prague in iperf2, but others can be done in parallel.***

# Some basic info and instructions (to improve later):

## What is it
UDP-Prague is fully compatible with TCP-Prague (in Linux and Apple), both in terms of convergence of rate and the responsiveness/inertia compromise.
It consists only of the **prague_cc.h** file and the **prague_cc.cpp** file that have no dependencies (except for a function that returns a monotonic time in Now()).
It currently has been running on Windows and Linux, but should be compilable on any C++ compiler on any platform. 
We try to get to a common stable API (currently at this stage still evolvable), so the CC can further evolve independently and should be easy to get the latest version in your app.
It can work in 2 modes.

### Continuous streaming mode (aka UDP-Prague)
Like TCP, but possible to adapt the source(s) of the data directly (like the CBR of a real-time encoder). A single pacing rate, window and micro-burst size is given that can be used and divided over different in-app streams.
We also included an example sender and receiver for this below.  

### Frame mode (aka RT-Prague)
To make it RT-Prague. Just provide an "fps" frame rate in Hz and "frame_budget" time in µs. The frame budget is the time you want to allocate to send the frame over. In continuous mode that would be 1/fps, but here it can be shorter, so leaving pauses in between frames. It assumes that each frame can be reasonable equal (so no full I-frames, only P-frames). This mode can reduce the throughput (depends on the bottleneck), but should further reduce the photon to photon latency for very interactive apps. If an fps and frame budget is provided, the GetCCVideoInfo() will tell you an extra output giving the frame size that the encoder should target for the next frame. No full support or example yet, but can be worked on, on request if you join this week.  

## How to use continuous mode (examples)
Use the PragueCC object as follows in a sender and a receiver in continuous mode. The same object can be used for bidirectionally data sending too.

### You want to send data as a "Sender"
Simplified flow essentials for a Prague congestion controlled sender. The full runnable code is in **udp_prague_sender.cpp** (a single file that compiles to an executable).
```
open(socket);
nextSend=now;
seqnr=1;
inflight=0;
PragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
while (true)
    inburst = 0;
    timeout = 0;
    startSend = 0;
    now_temp = now();
    while (inflight < packet_window) && (inburst < packet_burst) && (nextSend <= now_temp)
        PragueCC.getTimeInfo(data_msg.timestamp, data_msg.echoed_timestamp, data_msg.seq_nr, new_ecn)
        data_msg.payload = 0; // or other content
        if (startSend == 0)
            startSend = now();
        sendMsg(data_msg);
        inburst++;
        inflight++;
        seqnr++;
    if (startSend != 0)
        nextSend = startSend + packet_size*inburst*µs_per_second/pacing_rate;
    waitTimeout = 0;
    ack_msg = 0;
    if (inflight < packet_window)
        waitTimeout = nextSend;
    else
        waitTimeout = now() + 1000000;
    while (waitTimeout > now() && ack_msg == 0)
        timeout = waitTimeout – now();
        ack_msg = receiveMsg(timeout);
    if (ack_msg)
        PragueCC.PacketReceived(pass ack_msg fields);
        PragueCC.AckReceived(pass ack_msg fields and &inflight);
    else // timeout, reset state
        if (inflight >= packet_window);
            PragueCC->ResetCCInfo();
    PragueCC->GetCCInfo(get pacing_rate, packet_window, packet_burst, packet_size);
```

### You need to ACK data as a "Receiver"
Simplified flow essentials for a receiver that only ACKs (here every packet, but can skip x packets as in delayed ACKs in TCP). The full runnable code is in **udp_prague_receiver.cpp** (a single file that compiles to an executable).
```
open(socket);
while (true)
    data_msg = receiveMsg();
    if (data_msg)
        PragueCC.PacketReceived(data_msg.timestamp, data_msg.echoedTimestamp);
        PragueCC.DataReceivedSequence(tos & 3, data_msg.seq_nr);
    PragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoedTimestamp, new_ecn);
    PragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);
    if (new_ecn changed)
        set the new_ecn with a socket option
    sendMsg(ack_msg);
```
