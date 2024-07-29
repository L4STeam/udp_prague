#ifndef UDPSOCKET_H
#define UDPSOCKET_H

#ifdef WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <iostream>
#include <winsock2.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <mswsock.h>
#elif __linux__
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#endif
#include "prague_cc.h"

#ifdef WIN32
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

class UDPSocket {
#ifdef WIN32
    WSADATA wsaData;
    LPFN_WSARECVMSG WSARecvMsg;
    LPFN_WSASENDMSG WSASendMsg;
#endif
    ecn_tp current_ecn;
    SOCKADDR_IN peer_addr;
    socklen_t peer_len;
    SOCKET sockfd;
public:
    UDPSocket() :
#ifdef WIN32
        WSARecvMsg(NULL), WSASendMsg(NULL), 
#endif
        current_ecn(ecn_not_ect), peer_len(sizeof(peer_addr)) {
#ifdef WIN32
        DWORD dwThreadPri;
        if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
        {
            perror("SetThreadPriority failed.\n");
        }
        dwThreadPri = GetThreadPriority(GetCurrentThread());
        printf("Current thread priority is 0x%x\n", dwThreadPri);
        // Initialize Winsock
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            perror("WSAStartup failed.\n");
            exit(1);
        }
#elif __linux__
        if (geteuid() == 0) {
            struct sched_param sp;
            sp.sched_priority = sched_get_priority_max(SCHED_RR);
            // SCHED_OTHER, SCHED_FIFO, SCHED_RR
            if (sched_setscheduler(0, SCHED_RR, &sp) < 0) {
                perror("Client set scheduler");
            }
        }
#endif

        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
        if (int(sockfd) < 0) {
            perror("Socket creation failed.\n");
            exit(1);
        }
#ifdef WIN32
        // Initialize recv and send functions
        GUID guidWSARecvMsg = WSAID_WSARECVMSG;
        GUID guidWSASendMsg = WSAID_WSASENDMSG;
        DWORD dwBytes = 0;
        if (SOCKET_ERROR == WSAIoctl(sockfd, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &guidWSARecvMsg, sizeof(guidWSARecvMsg), &WSARecvMsg, sizeof(WSARecvMsg), &dwBytes, NULL, NULL)) {
            perror("Get WSARecvMsg function pointer failed.\n");
            exit(1);
        }
        if (SOCKET_ERROR == WSAIoctl(sockfd, SIO_GET_EXTENSION_FUNCTION_POINTER,
            &guidWSASendMsg, sizeof(guidWSASendMsg), &WSASendMsg, sizeof(WSASendMsg), &dwBytes, NULL, NULL)) {
            perror("Get WSASendMsg function pointer failed.\n");
            exit(1);
        }
#elif __linux__
        unsigned int set = 1;
        if (setsockopt(sockfd, IPPROTO_IP, IP_RECVTOS, &set, sizeof(set)) < 0) {
            printf("Could not set RECVTOS");
            exit(1);
        }
#endif
    }
    ~UDPSocket() {
#ifdef WIN32
        WSACleanup();
#endif
    }
    void Bind(const char* addr, uint32_t port) {
        // Set server address
        SOCKADDR_IN own_addr;
        memset(&own_addr, 0, sizeof(own_addr));
        own_addr.sin_family = AF_INET;
        own_addr.sin_addr.S_ADDR = inet_addr(addr);
        own_addr.sin_port = htons(port);
        bind(sockfd, (SOCKADDR *)&own_addr, sizeof(own_addr));
    }
    void Connect(const char* addr, uint32_t port) {
        // Set server address
        peer_len = sizeof(peer_addr);
        memset(&peer_addr, 0, peer_len);
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_addr.S_ADDR = inet_addr(addr);
        peer_addr.sin_port = htons(port);
        connect(sockfd, (SOCKADDR *)&peer_addr, peer_len);
    }
    size_tp Receive(char *buf, size_tp len, ecn_tp &ecn, time_tp timeout)
    {
        ssize_t r;
#ifdef WIN32
        if (timeout > 0) {
            struct timeval tv_in;
            tv_in.tv_sec = ((uint32_t)timeout) / 1000000;
            tv_in.tv_usec = ((uint32_t)timeout) % 1000000;
            fd_set recvsds;
            FD_ZERO(&recvsds);
            FD_SET((unsigned int)sockfd, &recvsds);
            int r = select(sockfd + 1, &recvsds, NULL, NULL, &tv_in);
            if (r < 0) {
                // select error
                perror("Select error.\n");
                exit(1);
            }
            else if (r == 0) {
                // Timeout
                return 0;
            }
        }
        peer_len = sizeof(peer_addr);
        r = recvfrom(sockfd, buf, size_t(len), 0, (SOCKADDR *)&peer_addr, &peer_len);
        if (r < 0) {
            perror("Fail to recv UDP message from socket.\n");
            exit(1);
        }
        ecn = ecn_not_ect; // for now TODO read ECN value
        return r;
#elif __linux__
        char ctrl_msg[CMSG_SPACE(sizeof(ecn))];

        struct msghdr rcv_msg;
        struct iovec rcv_iov[1];
        rcv_iov[0].iov_len = len;
        rcv_iov[0].iov_base = buf;

        rcv_msg.msg_name = &peer_addr;
        rcv_msg.msg_namelen = sizeof(peer_addr);
        rcv_msg.msg_iov = rcv_iov;
        rcv_msg.msg_iovlen = 1;
        rcv_msg.msg_control = ctrl_msg;
        rcv_msg.msg_controllen = sizeof(ctrl_msg);

        if (timeout > 0) {
            struct timeval tv_in;
            tv_in.tv_sec = ((uint32_t)timeout) / 1000000;
            tv_in.tv_usec = ((uint32_t)timeout) % 1000000;
            fd_set recvsds;
            FD_ZERO(&recvsds);
            FD_SET((unsigned int)sockfd, &recvsds);
            int r = select(sockfd + 1, &recvsds, NULL, NULL, &tv_in);
            if (r == SOCKET_ERROR) {
                // select error
                perror("Select error.\n");
                exit(1);
            }
            else if (r == 0) {
                // Timeout
                return 0;
            }
        }
        if ((r = recvmsg(sockfd, &rcv_msg, 0)) < 0) {
            perror("Fail to recv UDP message from socket\n");
            exit(1);
        }
        struct cmsghdr *cmptr = CMSG_FIRSTHDR(&rcv_msg);
        if ((cmptr->cmsg_level != IPPROTO_IP) || (cmptr->cmsg_type != IP_TOS)) {
            perror("Fail to recv IP.ECN field from packet\n");
            exit(1);
        };
        ecn = (ecn_tp)((unsigned char)(*(uint32_t*)CMSG_DATA(cmptr)) & ECN_MASK);
        return r;
#endif
    }
    size_tp Send(char *buf, size_tp len, ecn_tp ecn)
    {
        ssize_t rc = -1;
#ifdef WIN32
        DWORD numBytes;
        CHAR control[WSA_CMSG_SPACE(sizeof(INT))] = { 0 };
        WSABUF dataBuf;
        WSABUF controlBuf;
        WSAMSG wsaMsg;
        PCMSGHDR cmsg;
        dataBuf.buf = buf;
        dataBuf.len = ULONG(len);
        controlBuf.buf = control;
        controlBuf.len = sizeof(control);
        wsaMsg.name = LPSOCKADDR(&peer_addr);
        wsaMsg.namelen = sizeof(peer_addr);
        wsaMsg.lpBuffers = &dataBuf;
        wsaMsg.dwBufferCount = 1;
        wsaMsg.Control = controlBuf;
        wsaMsg.dwFlags = 0;
        cmsg = WSA_CMSG_FIRSTHDR(&wsaMsg);
        cmsg->cmsg_len = WSA_CMSG_LEN(sizeof(INT));
        cmsg->cmsg_level = (PSOCKADDR_STORAGE(&peer_addr)->ss_family == AF_INET) ? IPPROTO_IP : IPPROTO_IPV6;
        cmsg->cmsg_type = (PSOCKADDR_STORAGE(&peer_addr)->ss_family == AF_INET) ? IP_ECN : IPV6_ECN;
        *(PINT)WSA_CMSG_DATA(cmsg) = ecn;
        rc = WSASendMsg(sockfd, &wsaMsg, 0, &numBytes, NULL, NULL);
        if (rc == SOCKET_ERROR) {
            perror("Sent failed.");
            exit(1);
        }
        return numBytes;
#elif __linux__
        if (current_ecn != ecn) {
            if (setsockopt(sockfd, IPPROTO_IP, IP_TOS, &ecn, sizeof(ecn)) < 0) {
                printf("Could not apply ecn %d,\n", ecn);
                return -1;
            }
            current_ecn = ecn;
        }
        rc = sendto(sockfd, buf, len, 0, dest_addr, addrlen);
        if (rc < 0) {
            perror("Sent failed.");
            exit(1);
        }
        return size_tp(rc);
#endif
    }
};

#endif //UDPSOCKET_H
