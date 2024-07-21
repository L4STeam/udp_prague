// udp_prague_sender.cpp:
// An example of a (dummy data) UDP sender that needs to receive ACKs from a UDP receiver for congestion control
//

#ifdef WIN32
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#elif __linux__
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#endif

#include "prague_cc.h"


#define SERVER_PORT 8080       // Port to send to
#define SERVER_IP 0x7F000001   // IP to send to
#define MAX_PACKET_SIZE 1400   // in bytes (depending on MTU) 
#define BUFFER_SIZE 8192       // in bytes (depending on MTU) 
#define ECN_MASK ecn_ce

#pragma pack(push, 1)
struct datamessage_t {
    time_tp timestamp;	       // timestamp from peer, freeze and keep this time
    time_tp echoed_timestamp;  // echoed_timestamp can be used to calculate the RTT
    count_tp seq_nr;           // packet sequence number, should start with 1 and increase monotonic with packets sent

    void hton() {              // swap the bytes if needed
        timestamp = htonl(timestamp);
        echoed_timestamp = htonl(echoed_timestamp);
        seq_nr = htonl(seq_nr);
    }
};

struct ackmessage_t {
    time_tp timestamp;	       // timestamp from peer, freeze and keep this time
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
#define SIN_ADDR sin_addr.S_un.S_addr
#elif __linux__
typedef int SOCKET;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct sockaddr SOCKADDR;
#define SIN_ADDR sin_addr.s_addr
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

ssize_t recvfromecn(int sockfd, char *buf, size_t len, ecn_tp &ecn, SOCKADDR *src_addr, socklen_t *addrlen)
{
    return recvfrom(sockfd, buf, len, 0, src_addr, addrlen);
}
ssize_t sendtoecn(SOCKET sockfd, const char *buf, size_t len, ecn_tp ecn, const SOCKADDR *dest_addr, socklen_t addrlen)
{
    return sendto(sockfd, buf, len, 0, dest_addr, addrlen);
}

int main()
{
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
    server_addr.SIN_ADDR = htonl(SERVER_IP);
    server_addr.sin_port = htons(SERVER_PORT);

/*    // Bind the socket to the server address
    if (int(bind(sockfd, (SOCKADDR *)&server_addr, sizeof(server_addr))) < 0) {
        printf("Bind failed.\n");
        closesocket(sockfd);
        cleanupsocks();
        exit(1);
    }*/

    char receivebuffer[BUFFER_SIZE];
    uint32_t sendbuffer[BUFFER_SIZE/4];
    // init payload with dummy data
    for (int i = 0; i < BUFFER_SIZE/4; i++)
        sendbuffer[i] = htonl(i);

    struct ackmessage_t& ack_msg = (struct ackmessage_t&)(receivebuffer);  // overlaying the receive buffer
    struct datamessage_t& data_msg = (struct datamessage_t&)(sendbuffer);  // overlaying the send buffer

    // create a PragueCC object. Using default parameters for the Prague CC in line with TCP_Prague
    PragueCC pragueCC;

    time_tp nextSend = pragueCC.Now();
    count_tp seqnr = 1;
    count_tp inflight = 0;
    rate_tp pacing_rate;
    count_tp packet_window;
    count_tp packet_burst;
    size_tp packet_size;
    pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
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
            data_msg.seq_nr = seqnr;
            data_msg.hton();  // swap byte order if needed
            ssize_t bytes_sent = sendtoecn(sockfd, (char*)(&data_msg), packet_size, new_ecn, (SOCKADDR *)&server_addr, sizeof(server_addr));
            if (bytes_sent < 0 || ((size_tp) bytes_sent) != packet_size) {
                perror("invalid data packet length sent");
                exit(1);
            }
            inburst++;
            inflight++;
            seqnr++;
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
            bytes_received = recvfromecn(sockfd, receivebuffer, sizeof(receivebuffer), rcv_ecn, (SOCKADDR *)&src_addr, &src_len);
            if ((bytes_received == -1) && (errno != EWOULDBLOCK) && (errno != EAGAIN)) {
                perror("ERROR on recvfrom");
                exit(1);
            }
            now = pragueCC.Now();
        } while ((waitTimeout > now) && (bytes_received < 0));
        if (bytes_received >= ssize_t(sizeof(ack_msg))) {
            pragueCC.PacketReceived(ack_msg.timestamp, ack_msg.echoed_timestamp);
            pragueCC.ACKReceived(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, seqnr, ack_msg.error_L4S, inflight);
        }
        else // timeout, reset state
            if (inflight >= packet_window)
                pragueCC.ResetCCInfo();
        pragueCC.GetCCInfo(pacing_rate, packet_window, packet_burst, packet_size);
    }
}
