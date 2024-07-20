Use the PragueCC object as follows in a sender and a receiver. The same object can be used bidirectionally too.

Simplified flow essentials for a Prague congestion controlled sender:
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

Simplified flow essentials for a receiver that only ACKs (here every packet, but can skip x packets as in delayed ACKs in TCP):
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
