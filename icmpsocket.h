#ifndef ICMPSOCKET_H
#define ICMPSOCKET_H

#ifdef WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
//#ifndef WIN32_LEAN_AND_MEAN
//#define WIN32_LEAN_AND_MEAN
//#endif
#include <winsock2.h>
#include <ws2ipdef.h>
//#include <ws2tcpip.h>
//#include <mstcpip.h>
//#include <mswsock.h>
#elif __linux__
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#endif
#include "prague_cc.h"

#ifdef WIN32
typedef int socklen_t;
typedef int ssize_t;
#define S_ADDR S_un.S_addr
#else // Unix/Linux type of OSs
typedef int SOCKET;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct sockaddr SOCKADDR;
#define S_ADDR s_addr
#define ECN_MASK ecn_ce
#define SOCKET_ERROR SO_ERROR
#endif

class ICMPSocket
{
    SOCKADDR_IN peer_addr;
    socklen_t peer_len;
    SOCKET sockfd;

public:
    ICMPSocket(const char* dst_addr, ecn_tp ecn = ecn_l4s_id) : peer_len(sizeof(peer_addr))
    {
        sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
        if (int(sockfd) < 0) {
            // Check `net.ipv4.ping_group_range` in sysctl
            perror("ICMP socket creation failed (DGRAM).\n");
            exit(1);
        }
        unsigned int val = IP_PMTUDISC_DO;
        if (setsockopt(sockfd, IPPROTO_IP, IP_MTU_DISCOVER, &val, sizeof(val)) < 0) {
            perror("Colud not set IP_DF\n");
            exit(1);
        }
        unsigned int ecn_set = ecn;
        if (setsockopt(sockfd, IPPROTO_IP, IP_TOS, &ecn_set, sizeof(ecn_set)) < 0) {
            perror("Could not setsockopt IP_TOS\n");
            exit(1);
        }
        memset(&peer_addr, 0, peer_len);
        peer_addr.sin_family = AF_INET;
        peer_addr.sin_addr.S_ADDR = inet_addr(dst_addr);
    }
    ICMPSocket() = delete;

    ~ICMPSocket()
    {
#ifdef WIN32
        WSACleanup();
#else
        printf("close sockfd: %d\n", sockfd);
        close(sockfd);
        sockfd = -1;
#endif
    }

    uint16_t checkSum(void *data, uint16_t len)
    {
        uint32_t sum = 0;
        uint16_t *buf = (uint16_t *)data;
        while (len > 1) {
            sum += *buf++;
            len -= 2;
        }
        if (len > 0)
            sum += *(uint8_t*)buf;
        sum = (sum >> 16) + (sum & 0xFFFF);
        sum += (sum >> 16);
        return ~sum;
    }

    int checkPacket(const icmphdr *icmp_rcv, const SOCKADDR_IN pkt_src, uint16_t id = 0)
    {
        if (icmp_rcv->type == ICMP_ECHOREPLY) {
            if (pkt_src.sin_addr.S_ADDR != peer_addr.sin_addr.S_ADDR) {
                return 0;
            }
            if (id != 0 && icmp_rcv->un.echo.id != id) {
                return 0;
            }
        } else if (icmp_rcv->type == ICMP_DEST_UNREACH) {
            return -(icmp_rcv->code + 1);
        } else {
            return -256;
        }
        return 1;
    }

    void setICMPhdr(icmphdr *icmp_snd, uint16_t id) {
        icmp_snd->type             = ICMP_ECHO;      // ICMP_ECHO
        icmp_snd->un.echo.sequence = 0;              // Sequence: filled in before sendto()

        icmp_snd->checksum         = 0;              // Checksum, wil be canged by socket in Linux
        icmp_snd->un.echo.id       = htons(id);      // ICMP Identitiy, will be changed by socket in Linux
    }

    size_tp mtu_discovery(size_tp min_mtu, size_tp max_mtu, time_tp timeout = 200000, count_tp maxtry = 1)
    {
        SOCKADDR_IN recv_addr;
        socklen_t recv_len = sizeof(recv_addr);
        memset(&recv_addr, 0, sizeof(recv_addr));

        char pkt_snd[max_mtu];
        icmphdr *icmp_snd;
        uint16_t icmp_iden = 0;
        uint16_t icmp_seqn = 0;
        memset(pkt_snd, 0, max_mtu);
        icmp_snd = (icmphdr *) pkt_snd;
        setICMPhdr(icmp_snd, icmp_iden);

        char pkt_rcv[max_mtu];
        icmphdr *icmp_rcv;
        memset(pkt_rcv, 0, max_mtu);
        icmp_rcv = (icmphdr *) pkt_rcv;

        size_tp  mtu_lbound = min_mtu - sizeof(iphdr);
        size_tp  mtu_ubound = max_mtu - sizeof(iphdr);
        size_tp  mtu_best = 0;
        count_tp numtry = maxtry;

        if (timeout > 0) {
            struct timeval tv_in;
            tv_in.tv_sec = ((uint32_t)timeout) / 1000000;
            tv_in.tv_usec = ((uint32_t)timeout) % 1000000;
            if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (struct timeval *)&tv_in, sizeof(tv_in)) < 0) {
                perror("Coulld not set SO_RCVTIMEO");
                exit(1);
            }
        }

        while (mtu_lbound <= mtu_ubound) {

            size_tp mtu_now = (mtu_lbound + mtu_ubound) / 2;

            icmp_snd->un.echo.sequence = htons(++icmp_seqn);
            icmp_snd->checksum = 0;
            icmp_snd->checksum = checkSum(icmp_snd, mtu_now);

            if (sendto(sockfd, pkt_snd, mtu_now, 0, (SOCKADDR *) &peer_addr, peer_len) < 0) {
                if (errno == EMSGSIZE) {
                    //printf("Packet size %lu too big for local interface\n", mtu_now);
                    mtu_ubound = mtu_now - 1;
                    continue;
                }
                perror("Fail to send ICMP request.");
                exit(1);
            }
            if (recvfrom(sockfd, pkt_rcv, sizeof(pkt_rcv), 0, (SOCKADDR *) &recv_addr, &recv_len) < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    if (--numtry == 0) {
                        //printf("Timeout due to no response, invalid size %lu\n", mtu_now);
                        numtry = maxtry;
                        mtu_ubound = mtu_now - 1;
                    }
                    continue;
                }
                perror("Fail to recv ICMP repsonse.");
                exit(1);
            }
            int rc = checkPacket(icmp_rcv, recv_addr, icmp_iden);
            if (rc > 0) {
                //printf("Valid MTU %lu\n", mtu_now);
                numtry = maxtry;
                mtu_lbound = mtu_now + 1;
                if (mtu_now > mtu_best)
                    mtu_best = mtu_now;
            } else if (rc == 0) {
                continue;
            } else {
                switch(rc)
                {
                    case -2:    printf("ICMP error, host unreachable\n");     break; // ICMP_HOST_UNREACH
                    case -4:    printf("ICMP error, port unreachable\n");     break; // ICMP_PORT_UNREACH
                    case -5:    printf("ICMP error, fragmentation needed\n"); break; // ICMP_FRAG_NEEDED
                    case -256:  printf("Unknown error\n");                    break;
                    default:    printf("Other ICMP error\n");                 break;
                }
                numtry = maxtry;
                mtu_ubound = mtu_now - 1;
            }
        }
        return mtu_best + sizeof(iphdr);
    }
};
#endif
