// udp_prague_receiver.cpp:
// An example of a (dummy data) UDP receiver that needs to send ACKs for a congestion controlled UDP sender
//

#ifdef WIN32
#include <iostream>
#include <winsock2.h>
#elif __linux__
#include <string.h>
#include <argp.h>
#include <unistd.h>
#include <netinet/ip.h>
#endif

#include "prague_cc.h"

#define PORT 8080              // Port to listen on
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
#define SIN_ADDR sin_addr.S_un.S_addr
#elif __linux__
typedef int SOCKET;
typedef struct sockaddr_in SOCKADDR_IN;
typedef struct sockaddr SOCKADDR;
#define SIN_ADDR sin_addr.s_addr
#endif

static char prog_doc[] = "UDP Packet Receiver";

static struct argp_option prog_options[] =
{
        {"rcv_port",    'p',    "PORT",      0, "Receiver port",         0},
        { 0,             0,      0,          0,  0,                      0}
};

struct Args {
    uint32_t    rcv_port     =  8080;
};

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
    Args *args = static_cast<Args*>(state->input);
    switch (key)
    {
        case 'p':
            args->rcv_port = atoi(arg);
            break;
        case ARGP_KEY_ARG:
            argp_usage(state);
            break;
        case ARGP_KEY_END:
            break;
        default:
            return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = { prog_options, parse_opt, NULL, prog_doc, 0, 0, 0 };

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
    return recvfrom(sockfd, buf,  len, 0, src_addr, addrlen);
}
ssize_t sendtoecn(SOCKET sockfd, const char *buf, size_t len, ecn_tp ecn, const SOCKADDR *dest_addr, socklen_t addrlen)
{
    return sendto(sockfd, buf, len, 0, dest_addr, addrlen);
}

int main(int argc, char **argv)
{
        Args args;
    int err;
    // [TODO] Add Error handing for arguments
    if ((err = argp_parse(&argp, argc, argv, 0, 0, &args)))
    {
        std::cerr << "Failed to parse program arguments: " << strerror(err) << std::endl;
        return err;
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
    server_addr.SIN_ADDR = INADDR_ANY;
    server_addr.sin_port = htons(args.rcv_port);

    // Bind the socket to the server address
    if (int(bind(sockfd, (SOCKADDR *)&server_addr, sizeof(server_addr))) < 0) {
        printf("Bind failed.\n");
#ifdef WIN32
        closesocket(sockfd);
        cleanupsocks();
#elif __linux__
        close(sockfd);
        cleanupsocks();
#endif
        exit(1);
    }

    char receivebuffer[BUFFER_SIZE];

    struct datamessage_t& data_msg = (struct datamessage_t&)(receivebuffer);  // overlaying the receive buffer
    struct ackmessage_t ack_msg;     // the send buffer

    // create a PragueCC object. No parameters needed if only ACKs are sent
    PragueCC pragueCC;

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
        data_msg.hton();  // swap byte order if needed

        // Pass the relevant data to the PragueCC object:
        pragueCC.PacketReceived(data_msg.timestamp, data_msg.echoed_timestamp);
        pragueCC.DataReceivedSequence(rcv_ecn, data_msg.seq_nr);

        // Return a corresponding acknowledge message
        ecn_tp new_ecn;
        pragueCC.GetTimeInfo(ack_msg.timestamp, ack_msg.echoed_timestamp, new_ecn);
        pragueCC.GetACKInfo(ack_msg.packets_received, ack_msg.packets_CE,
            ack_msg.packets_lost, ack_msg.error_L4S);

        ack_msg.hton();  // swap byte order if needed
        ssize_t bytes_sent = sendtoecn(sockfd, (char*)(&ack_msg), sizeof(ack_msg), new_ecn, (SOCKADDR *)&client_addr, client_len);
        if (bytes_sent != sizeof(ack_msg)) {
            perror("invalid ack packet length sent");
            exit(1);
        }
    }
}