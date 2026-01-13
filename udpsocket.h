#ifndef UDPSOCKET_H
#define UDPSOCKET_H

#ifdef _WIN32
#define _WINSOCK_DEPRECATED_NO_WARNINGS
// #ifndef WIN32_LEAN_AND_MEAN
// #define WIN32_LEAN_AND_MEAN
// #endif

#include <winsock2.h>

#include <mstcpip.h>
#include <mswsock.h>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#elif __linux__
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>
#elif __FreeBSD__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#elif __APPLE__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#ifdef __MUSL__
#include <pthread.h>
#include <sys/time.h>
#endif
#include "prague_cc.h"

#ifdef _WIN32
typedef int ssize_t;
#endif

// Holds a resolved socket address (IPv4 or IPv6) and its length.
struct Endpoint {
  sockaddr_storage sa{};
  socklen_t len{0};

  bool is_v4() const { return sa.ss_family == AF_INET; }
  bool is_v6() const { return sa.ss_family == AF_INET6; }
  int family() const { return sa.ss_family; }
};

// Platform-abstracted socket type (SOCKET on Windows, else int).
using SocketHandle =
#ifdef _WIN32
    SOCKET;
#else
    int;
#endif

class UDPSocket {
public:
  UDPSocket();
  ~UDPSocket();

  void Bind(const char *addr, uint16_t port);
  void Connect(const char *addr, uint16_t port);

  size_tp Receive(char *buf, size_tp len, ecn_tp &ecn, time_tp timeout);
  size_tp Send(char *buf, size_tp len, ecn_tp ecn);

private:
  void init_io();

private:
#ifdef _WIN32
  WSADATA wsaData;            // Winsock state data
  LPFN_WSARECVMSG WSARecvMsg; // Pointer to WSARecvMsg extension function
  LPFN_WSASENDMSG WSASendMsg; // Pointer to WSASendMsg extension function

  WSABUF dataBuf;

  CHAR sendControl[WSA_CMSG_SPACE(sizeof(INT))] = {0};
  WSABUF sendControlBuf;
  WSAMSG sendMsg;

  CHAR recvControl[WSA_CMSG_SPACE(sizeof(INT))] = {0};
  WSABUF recvControlBuf;
  WSAMSG recvMsg;
#else
  msghdr send_msg{};
  iovec send_iov{};
  alignas(cmsghdr) char send_ctrl[CMSG_SPACE(sizeof(int))];

  msghdr recv_msg{};
  iovec recv_iov{};
  alignas(cmsghdr) char recv_ctrl[CMSG_SPACE(sizeof(int))];
#endif
  SocketHandle socket;
  Endpoint peer;

  bool connected;
};
#endif // UDPSOCKET_H
