// udp_prague_receiver.cpp:
// An example of a (dummy data) UDP receiver that needs to send ACKs for a congestion controlled UDP sender
//

#ifdef WIN32
#include <iostream>
#include <winsock2.h>
#elif __linux__
#include <cassert>
#include <string.h>
#include <unistd.h>
#include <netinet/ip.h>
#include <arpa/inet.h>
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
#define closesocket close
#endif

void initsocks()
{
#ifdef WIN32
    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        perror("WSAStartup failed.\n");
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

    if ((r = recvmsg(sockfd, &rcv_msg, 0)) < 0)
    {
        perror("Fail to recv UDP message from socket\n");
        return -1;
    }
    auto cmptr = CMSG_FIRSTHDR(&rcv_msg);
    assert(cmptr->cmsg_level == IPPROTO_IP && cmptr->cmsg_type == IP_TOS);
    ecn = (ecn_tp)((unsigned char)(*(uint32_t*)CMSG_DATA(cmptr)) & ECN_MASK);

    return r;

#endif
}
ssize_t sendto_ecn(SOCKET sockfd, char *buf, size_t len, ecn_tp ecn, const SOCKADDR *dest_addr, socklen_t addrlen)
{
#ifdef WIN32
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
    return sendto(sockfd, buf, len, 0, dest_addr, addrlen);
#endif
}

int main(int argc, char **argv)
{
    int rcv_port = 8080;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-p" && i + 1 < argc)
            rcv_port = atoi(argv[++i]);
        else {
            perror("Usage: udp_prague_receiver -p <receiver port>");;
            return 1;
        }
    }
    initsocks();
    // Create a UDP socket
    SOCKET sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (int(sockfd) < 0) {
        perror("Socket creation failed.\n");
        cleanupsocks();
        exit(1);
    }
    // Set server address
    SOCKADDR_IN server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.S_ADDR = INADDR_ANY;
    server_addr.sin_port = htons(rcv_port);

    // Bind the socket to the server address
    if (int(bind(sockfd, (SOCKADDR *)&server_addr, sizeof(server_addr))) < 0) {
        perror("Bind failed.\n");
        closesocket(sockfd);
        cleanupsocks();
        exit(1);
    }

#ifdef WIN32
#elif __linux__
    unsigned int set = 1;
    if (setsockopt(sockfd, IPPROTO_IP, IP_RECVTOS, &set, sizeof(set)) < 0) {
        perror("Could not set RECVTOS");
        exit(1);
    }
#endif

    printf("UDP Prague receiver listening on port %d.\n", rcv_port);

    char receivebuffer[BUFFER_SIZE];

    struct datamessage_t& data_msg = (struct datamessage_t&)(receivebuffer);  // overlaying the receive buffer
    struct ackmessage_t ack_msg;     // the send buffer

    // create a PragueCC object. No parameters needed if only ACKs are sent
    PragueCC pragueCC;
    time_tp prev_tp = 0;

    // Receive data from client
    while (true) {
        // Wait for an incoming data message
        SOCKADDR_IN client_addr;
        socklen_t client_len = sizeof(client_addr);
        ecn_tp rcv_ecn;
        ssize_t bytes_received = recvfrom_ecn_timeout(sockfd, receivebuffer, sizeof(receivebuffer), rcv_ecn, 0, (SOCKADDR *)&client_addr, &client_len);
        //printf("From:\t %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        while (bytes_received == -1) {   // repeat if timeout or interrupted
            if (errno != EWOULDBLOCK && errno != EAGAIN) {
                perror("ERROR on recvfrom");
                exit(1);
            }
            bytes_received = recvfrom_ecn_timeout(sockfd, receivebuffer, sizeof(receivebuffer), rcv_ecn, 0, (SOCKADDR *)&client_addr, &client_len);
        }
        //printf("UDP Prague receiving ECN %d with size %ld bytes from %s:%d.\n", rcv_ecn, bytes_received, inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        // Extract the data message
        data_msg.hton();  // swap byte order
        //printf("Get timestamp from sender: %d\t%d\n", data_msg.timestamp, data_msg.echoed_timestamp);

        // Pass the relevant data to the PragueCC object:
        pragueCC.PacketReceived(data_msg.timestamp, data_msg.echoed_timestamp);
        pragueCC.DataReceivedSequence(rcv_ecn, data_msg.seq_nr);

        // Return a corresponding acknowledge message
        ecn_tp new_ecn;
        pragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
        pragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE, ack_msg.packets_lost, ack_msg.error_L4S);

        //printf("Send timestamp from receiverr: %d (diff= %d) with %d rcv_pkt\n", ack_msg.timestamp, data_msg.timestamp - prev_tp, ack_msg.packets_received);
        prev_tp = data_msg.timestamp;

        ack_msg.hton();  // swap byte order if needed
	//printf("ack_msg.packets_received: %d\n", ack_msg.packets_received);
        ssize_t bytes_sent = sendto_ecn(sockfd, (char*)(&ack_msg), sizeof(ack_msg), new_ecn, (SOCKADDR *)&client_addr, client_len);
        if (bytes_sent != sizeof(ack_msg)) {
            perror("invalid ack packet length sent");
            exit(1);
        }
    }
}
