// udp_prague_receiver.cpp:
// An example of a (dummy data) UDP receiver that needs to send ACKs for a congestion controlled UDP sender
//

#ifdef WIN32
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#endif // WINDOWS
#ifdef LINUX
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#endif // LINUX

#include "prague_cc.h"


#define PORT 8080              // Port to listen on
#define MAX_PACKET_SIZE 1500   // in bytes (depending on MTU) 
#define BUFFER_SIZE 8192       // in bytes (depending on MTU) 
#define ECN_MASK ecn_ce

#pragma pack(push, 1)
struct datamessage_t {
    time_tp timestamp;	       // timestamp from peer, freeze and keep this time
    time_tp echoed_timestamp;  // echoed_timestamp can be used to calculate the RTT
    count_tp seq_nr;           // packet sequence number, should start with 1 and increase monotonic with packets sent

    void hton() {              // swap the bytes if needed
        htonl(timestamp);
        htonl(echoed_timestamp);
        htonl(seq_nr);
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
        htonl(timestamp);
        htonl(echoed_timestamp);
        htonl(packets_received);
        htonl(packets_CE);
        htonl(packets_lost);
    }
};
#pragma pack(pop)

#ifdef WIN32
WSADATA wsaData;
typedef int socklen_t;
typedef int ssize_t;
#define SIN_ADDR sin_addr.S_un
#endif
#ifdef LINUX
typedef int SOCKET;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct sockaddr SOCKADDR;
#define SIN_ADDR sin_addr
#endif // LINUX

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

ssize_t recvfromecn(int sockfd, void *buf, size_t len, ecn_tp &ecn, SOCKADDR *src_addr, socklen_t *addrlen)
{
    return 0;
}
ssize_t sendtoecn(SOCKET sockfd, const void *buf, size_t len, ecn_tp ecn, const SOCKADDR *dest_addr, socklen_t addrlen)
{
    return 0;
}
/*
void sendtoEcn(SOCKET sock, PSOCKADDR_STORAGE addr, LPFN_WSASENDMSG sendmsg, PCHAR data, INT datalen)
{
    DWORD numBytes;
    INT error;

    CHAR control[WSA_CMSG_SPACE(sizeof(INT))] = { 0 };
    WSABUF dataBuf;
    WSABUF controlBuf;
    WSAMSG wsaMsg;
    PCMSGHDR cmsg;

    dataBuf.buf = PCHAR(buf);
    dataBuf.len = len;
    controlBuf.buf = control;
    controlBuf.len = sizeof(control);
    wsaMsg.name = (PSOCKADDR)dest_addr;
    wsaMsg.namelen = (INT)INET_SOCKADDR_LENGTH(dest_addr->ss_family);
    wsaMsg.lpBuffers = &dataBuf;
    wsaMsg.dwBufferCount = 1;
    wsaMsg.Control = controlBuf;
    wsaMsg.dwFlags = 0;

    cmsg = WSA_CMSG_FIRSTHDR(&wsaMsg);
    cmsg->cmsg_len = WSA_CMSG_LEN(sizeof(INT));
    cmsg->cmsg_level = (addr->ss_family == AF_INET) ? IPPROTO_IP : IPPROTO_IPV6;
    cmsg->cmsg_type = (addr->ss_family == AF_INET) ? IP_ECN : IPV6_ECN;
    *(PINT)WSA_CMSG_DATA(cmsg) = ECN_ECT_0;

    error =
        sendmsg(
            sock,
            &wsaMsg,
            0,
            &numBytes,
            NULL,
            NULL);
    if (error == SOCKET_ERROR) {
        printf("sendmsg failed %d\n", WSAGetLastError());
    }
}*/


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
    server_addr.SIN_ADDR.S_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    // Bind the socket to the server address
    if (int(bind(sockfd, (SOCKADDR *)&server_addr, sizeof(server_addr))) < 0) {
        printf("Bind failed.\n");
        closesocket(sockfd);
        cleanupsocks();
        exit(1);
    }

    char receivebuffer[BUFFER_SIZE];
//    struct iphdr *iph = (iphdr*)receivebuffer; // Obverlay on IP header in receivebuffer

    struct datamessage_t *data_msg;  // pointer in the receive buffer
    struct ackmessage_t ack_msg;     // the send buffer

    // create a PragueCC object. No parameters needed if only ACKs are sent
    PragueCC pragueCC(0, 0, 0, 0, 0, 0, 0);


    // Receive data from client
    while (true) {
        // Wait for an incoming data message
        SOCKADDR_IN client_addr;
        socklen_t client_len = sizeof(client_addr);
        ecn_tp rcv_ecn;
        ssize_t bytes_received = recvfromecn(sockfd, receivebuffer, sizeof(receivebuffer), rcv_ecn, (SOCKADDR *)&client_addr, &client_len);

        while (bytes_received == -1) {   // repeat if timeout or interrupted
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                perror("ERROR on recvfrom");
                exit(1);
            }
            bytes_received = recvfromecn(sockfd, receivebuffer, sizeof(receivebuffer), rcv_ecn, (SOCKADDR *)&client_addr, &client_len);
        }

        // Extract the data message
        data_msg = (struct datamessage_t *)(receivebuffer);// +iph->ihl * 4); ???
        data_msg->hton();  // swap byte order if needed

        // Pass the relevant data to the PragueCC object:
        pragueCC.PacketReceived(data_msg->timestamp, data_msg->echoed_timestamp);
        pragueCC.DataReceivedSequence(rcv_ecn, data_msg->seq_nr);

        // Return a corresponding acknowledge message
        ecn_tp new_ecn;
        pragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
        pragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE,
            ack_msg.packets_lost, ack_msg.error_L4S);

        ack_msg.hton();  // swap byte order if needed
        ssize_t bytes_sent = sendtoecn(sockfd, &ack_msg, sizeof(ack_msg), new_ecn, (SOCKADDR *)&client_addr, client_len);
        if (bytes_sent != sizeof(ack_msg)) {
            perror("invalid ack packet length sent");
            exit(1);
        }
    }
}
