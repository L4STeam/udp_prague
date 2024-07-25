// udp_prague_sender.cpp:
// An example of a (dummy data) UDP sender that needs to receive ACKs from a UDP receiver for congestion control
//

#ifdef WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#elif __linux__
#include <cassert>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#endif

#include "prague_cc.h"

#define BUFFER_SIZE 8192       // in bytes (depending on MTU) 
#define ECN_MASK ecn_ce

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

#ifdef WIN32
WSADATA wsaData;
typedef int socklen_t;
typedef int ssize_t;
#define S_ADDR S_un.S_addr
#elif __linux__
typedef int SOCKET;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct sockaddr SOCKADDR;
#define S_ADDR s_addr
#endif

void initsocks()
{
#ifdef WIN32
    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed.\n");
        exit(1);
    }
#endif
}

void cleanupsocks()
{
#ifdef WIN32
    WSACleanup();
#endif
}

ecn_tp current_ecn = ecn_not_ect;

ssize_t recvfrom_ecn_timeout(int sockfd, char *buf, size_t len, ecn_tp &ecn, time_tp timeout, SOCKADDR *src_addr, socklen_t *addrlen)
{
#ifdef WIN32
    return recvfrom(sockfd, buf, len, 0, src_addr, addrlen);
#elif __linux__
    ssize_t r;
    char ctrl_msg[CMSG_SPACE(sizeof(ecn))];

    struct msghdr rcv_msg;
    struct iovec rcv_iov[1];
    rcv_iov[0].iov_len = len;
    rcv_iov[0].iov_base = buf;

    rcv_msg.msg_name = (SOCKADDR_IN *) src_addr;
    rcv_msg.msg_namelen = *addrlen;
    rcv_msg.msg_iov = rcv_iov;
    rcv_msg.msg_iovlen = 1;
    rcv_msg.msg_control = ctrl_msg;
    rcv_msg.msg_controllen = sizeof(ctrl_msg);

    struct timeval tv_in;
    tv_in.tv_sec =  ((uint32_t) timeout) / 1000000;
    tv_in.tv_usec = ((uint32_t) timeout) % 1000000;
    //if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv_in, sizeof(tv_in)) < 0) {
    //    perror("setsock timeout failed\n");
    //    return -1;
    //}
    //struct timeval tv_out;
    //socklen_t vslen = sizeof(tv_out);
    //getsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv_out, &vslen);
    //printf("After setsockopt:  tv_sec = %ld ; tv_usec = %ld TO: %d\n", tv_out.tv_sec, tv_out.tv_usec, timeout);
    fd_set recvsds;
    FD_ZERO(&recvsds);
    FD_SET((unsigned int) sockfd, &recvsds);
    int rv = select(sockfd + 1, &recvsds, NULL, NULL, &tv_in);
    if (rv < 0) {
        // select error
        return -1;
    } else if (rv == 0)  {
        // Timeout
	return 0;
    } else {
        // socket has something to read
        if ((r = recvmsg(sockfd, &rcv_msg, 0)) < 0) {
            perror("Fail to recv UDP message from socket\n");
            return -1;
        }
    }
    //if ((r = recvmsg(sockfd, &rcv_msg, 0)) < 0)
    //{
    //    perror("Fail to recv UDP message from socket\n");
    //    return -1;
    //}
    auto cmptr = CMSG_FIRSTHDR(&rcv_msg);
    assert(cmptr->cmsg_level == IPPROTO_IP && cmptr->cmsg_type == IP_TOS);
    ecn = (ecn_tp)((unsigned char)(*(uint32_t*)CMSG_DATA(cmptr)) & ECN_MASK);

    return r;
#endif
}
ssize_t sendto_ecn(SOCKET sockfd, char *buf, size_t len, ecn_tp ecn, SOCKADDR *dest_addr, socklen_t addrlen)
{
#ifdef WIN32_not_yet_ok
    DWORD numBytes;
    INT error;
    CHAR control[WSA_CMSG_SPACE(sizeof(INT))] = { 0 };
    WSABUF dataBuf;
    WSABUF controlBuf;
    WSAMSG wsaMsg;
    PCMSGHDR cmsg;
    dataBuf.buf = buf;
    dataBuf.len = len;
    controlBuf.buf = control;
    controlBuf.len = sizeof(control);
    wsaMsg.name = dest_addr;
    wsaMsg.namelen = addrlen;
    wsaMsg.lpBuffers = &dataBuf;
    wsaMsg.dwBufferCount = 1;
    wsaMsg.Control = controlBuf;
    wsaMsg.dwFlags = 0;
    cmsg = WSA_CMSG_FIRSTHDR(&wsaMsg);
    cmsg->cmsg_len = WSA_CMSG_LEN(sizeof(INT));
    cmsg->cmsg_level = (PSOCKADDR_STORAGE(dest_addr)->ss_family == AF_INET) ? IPPROTO_IP : IPPROTO_IPV6;
    cmsg->cmsg_type = (PSOCKADDR_STORAGE(dest_addr)->ss_family == AF_INET) ? IP_ECN : IPV6_ECN;
    *(PINT)WSA_CMSG_DATA(cmsg) = ecn;
    return sendmsg(sockfd, &wsaMsg, 0, &numBytes, NULL, NULL);
#elif __linux__
    if (current_ecn != ecn) {
        if (setsockopt(sockfd, IPPROTO_IP, IP_TOS, &ecn, sizeof(ecn)) < 0) {
            printf("Could not apply ecn %d,\n", ecn);
            return -1;
        }
        current_ecn = ecn;
    }
#endif
    return sendto(sockfd, buf, len, 0, dest_addr, addrlen);
}

int main(int argc, char **argv)
{
    struct sched_param sp;
    sp.sched_priority = sched_get_priority_max(SCHED_RR);
    // SCHED_OTHER, SCHED_FIFO, SCHED_RR
    if (sched_setscheduler(0, SCHED_RR, &sp) < 0) {
        perror("Client set scheduler");
    }

    bool verbose = false;
    const char *rcv_addr = "127.0.0.1";
    uint32_t rcv_port = 8080;
    size_tp max_pkt = 1400;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-a" && i + 1 < argc)
            rcv_addr = argv[++i];
        else if (arg == "-p" && i + 1 < argc)
            rcv_port = atoi(argv[++i]);
        else if (arg == "-m" && i + 1 < argc)
            max_pkt = atoi(argv[++i]);
        else if (arg == "-v")
            verbose = true;
        else {
            perror("Usage: udp_prague_receiver -a <receiver address> -p <receiver port> -m <max packet length> -v (for verbose prints)");;
            return 1;
        }
    }
    initsocks();
    // Create a UDP socket
    SOCKET sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (int(sockfd) < 0) {
        printf("Socket creation failed.\n");
        cleanupsocks();
        exit(1);
    }
    // Set server address
    SOCKADDR_IN server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.S_ADDR = inet_addr(rcv_addr);
    server_addr.sin_port = htons(rcv_port);

    #ifdef WIN32
    #elif __linux__
    unsigned int set = 1;
    if (setsockopt(sockfd, IPPROTO_IP, IP_RECVTOS, &set, sizeof(set)) < 0) {
        printf("Could not set RECVTOS");
        exit(1);
    }
    #endif

    char receivebuffer[BUFFER_SIZE];
    uint32_t sendbuffer[BUFFER_SIZE/4];
    // init payload with dummy data
    for (int i = 0; i < BUFFER_SIZE/4; i++)
        sendbuffer[i] = htonl(i);

    struct ackmessage_t& ack_msg = (struct ackmessage_t&)(receivebuffer);  // overlaying the receive buffer
    struct datamessage_t& data_msg = (struct datamessage_t&)(sendbuffer);  // overlaying the send buffer

    printf("UDP Prague sender sending to %s on port %d with max packet size %ld bytes.\n", rcv_addr, rcv_port, max_pkt);

    // create a PragueCC object. Using default parameters for the Prague CC in line with TCP_Prague
    PragueCC pragueCC(max_pkt);

    time_tp nextSend = pragueCC.Now();
    time_tp ref_tm = nextSend;
    count_tp seqnr = 0;
    count_tp inflight = 0;
    rate_tp pacing_rate;
    count_tp packet_window;
    count_tp packet_burst;
    size_tp packet_size;
    pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
    if (verbose) {
        printf("r: time, timestamp, echoed_timestamp, packets_received, packets_CE, packets_lost, seqnr, error_L4S, inflight\n");
        printf("s: time, pacing_rate, packet_window, packet_burst, packet_size, seqnr, inflight, inburst, nextSend\n");
    }
    while (true) {
        count_tp inburst = 0;
        time_tp timeout = 0;
        time_tp startSend = 0;
        time_tp now = pragueCC.Now();
        while ((inflight < packet_window) && (inburst < packet_burst) && (nextSend <= now)) {
            ecn_tp new_ecn;
            pragueCC.GetTimeInfo(data_msg.timestamp, data_msg.echoed_timestamp, new_ecn);
            if (startSend == 0)
                startSend = now;
            data_msg.seq_nr = ++seqnr;
            data_msg.hton();  // swap byte order if needed
            ssize_t bytes_sent = sendto_ecn(sockfd, (char*)(&data_msg), packet_size, new_ecn, (SOCKADDR *)&server_addr, sizeof(server_addr));
            if (verbose)
	        printf("s: %d,  %ld, %d, %d, %ld, %d, %d, %d, %d\n", now - ref_tm, pacing_rate, packet_window, packet_burst, packet_size, seqnr, inflight, inburst, nextSend - ref_tm);
	    if (bytes_sent < 0 || ((size_tp) bytes_sent) != packet_size) {
                perror("invalid data packet length sent");
                exit(1);
            }
            //printf("Infight %d, Inburst %d, nextSend %d\n", inflight, inburst, nextSend);
            inburst++;
            inflight++;
        }
        if (startSend != 0)
            nextSend = startSend + packet_size * inburst * 1000000 / pacing_rate;
        time_tp waitTimeout = 0;
        now = pragueCC.Now();
        if (inflight < packet_window)
            waitTimeout = nextSend;
        else
            waitTimeout = now + 1000000;
        ecn_tp rcv_ecn;
        ssize_t bytes_received = -1;
        do {
            timeout = waitTimeout - now;
            SOCKADDR_IN src_addr;
            socklen_t src_len = sizeof(src_addr);

            bytes_received = recvfrom_ecn_timeout(sockfd, receivebuffer, sizeof(receivebuffer), rcv_ecn, timeout, (SOCKADDR *)&src_addr, &src_len);
	    //printf("Time diff: %d, TO: %d, B_recv: %ld\n",  pragueCC.Now() - now, timeout, bytes_received);
            //printf("From:\t %s:%d\n", inet_ntoa(src_addr.sin_addr), ntohs(src_addr.sin_port));
            //if ((bytes_received == -1) && (errno != EWOULDBLOCK) && (errno != EAGAIN)) {
            if (bytes_received == -1) {
                perror("ERROR on recvfrom");
                exit(1);
            }
            now = pragueCC.Now();
        } while ((waitTimeout > now) && (bytes_received < 0));
        if (bytes_received >= ssize_t(sizeof(ack_msg))) {
            ack_msg.hton();
	    pragueCC.PacketReceived(ack_msg.timestamp, ack_msg.echoed_timestamp);
	    //printf("ack_msg.packets_received: %d\n", ack_msg.packets_received);
            pragueCC.ACKReceived(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, seqnr, ack_msg.error_L4S, inflight);
            if (verbose)
	        printf("r: %d, %d, %d, %d, %d, %d, %d, %d,%d\n", now - ref_tm, ack_msg.timestamp, ack_msg.echoed_timestamp, ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, seqnr, ack_msg.error_L4S, inflight);
	}
        else // timeout, reset state
            if (inflight >= packet_window)
                pragueCC.ResetCCInfo();
        pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
    }
}
