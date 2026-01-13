#include "udpsocket.h"
#include <cassert>
#include <cstring>
#include <system_error>

#ifndef _WIN32
constexpr int SOCKET_ERROR = -1;
#define ECN_MASK ecn_ce
#endif

/*
 * On Linux, ECN is delivered via IP_TOS / IPV6_TCLASS control messages.
 * On macOS, the same information uses IP_RECVTOS / IPV6_RECVTCLASS.
 * We normalize this difference with these macros.
 */
#ifdef __linux__
#define IP_RECV_CMSG_TYPE IP_TOS
#define IPV6_RECV_CMSG_TYPE IPV6_TCLASS
#elif __APPLE__
#define IP_RECV_CMSG_TYPE IP_RECVTOS
#define IPV6_RECV_CMSG_TYPE IPV6_RECVTCLASS
#endif

// Return an invalid socket handle for the current platform
SocketHandle invalid_socket() {
#ifdef _WIN32
  return INVALID_SOCKET;
#else
  return -1;
#endif
}

// Check if a handle represents a valid socket on the current platform
bool is_socket_valid(SocketHandle s) {
#ifdef _WIN32
  return s != INVALID_SOCKET;
#else
  return s >= 0;
#endif
}

// Close a socket handle if it is valid
void close_socket(SocketHandle s) {
  if (is_socket_valid(s)) {
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
  }
}

// Retrieve the last OS-specific socket error code
int last_error_code() {
#ifdef _WIN32
  return WSAGetLastError();
#else
  return errno;
#endif
}

// Wait for a socket to become readable within a timeout
bool wait_for_readable(SocketHandle s, time_tp timeout) {
  assert(is_socket_valid(s));
  assert(timeout >= 0);

#ifdef _WIN32
  // For small timeouts on Windows, select has ~15ms granularity.
  // If <15ms, treat as non-blocking "poll".
  if (timeout > 0 && timeout < 15000)
    timeout = 0;
#endif

  fd_set recvsds;
  FD_ZERO(&recvsds);
  FD_SET(s, &recvsds);

  timeval tv{};
  tv.tv_sec = static_cast<long>(timeout / 1000000);
  tv.tv_usec = static_cast<long>(timeout % 1000000);

  int r = select((int)s + 1, &recvsds, NULL, NULL, &tv);

  if (r == SOCKET_ERROR)
    throw std::system_error(last_error_code(), std::system_category(),
                            "select");

  return r > 0;
}

// Create an IPv4 or IPv6 datagram socket
SocketHandle make_socket(int family) {
  if (!(family == AF_INET || family == AF_INET6)) {
    throw std::system_error(EAFNOSUPPORT, std::system_category(),
                            "Unsupported address family");
  }

  SocketHandle s = ::socket(family, SOCK_DGRAM, 0);

  if (!is_socket_valid(s))
    throw std::system_error(last_error_code(), std::system_category(),
                            "socket");

  return s;
}

// Enable receiving ECN (TOS/TCLASS) on a datagram socket
void enable_recv_ecn(SocketHandle s, int family) {
  assert(is_socket_valid(s));
  assert(family == AF_INET || family == AF_INET6);

#ifdef _WIN32
  (void)family; // unused

  if (WSASetRecvIPEcn(s, TRUE) != 0)
    throw std::system_error(last_error_code(), std::system_category(),
                            "WSASetRecvIPEcn");
#else
  int set = 1;

  switch (family) {
  case AF_INET:
    if (setsockopt(s, IPPROTO_IP, IP_RECVTOS, &set,
                   static_cast<socklen_t>(sizeof(set))) == -1)
      throw std::system_error(errno, std::system_category(),
                              "setsockopt(IP_RECVTOS)");
    break;
  case AF_INET6:
    if (setsockopt(s, IPPROTO_IPV6, IPV6_RECVTCLASS, &set,
                   static_cast<socklen_t>(sizeof(set))) == -1)
      throw std::system_error(errno, std::system_category(),
                              "setsockopt(IPV6_RECVTCLASS)");
    break;
  default:
    throw std::system_error(EAFNOSUPPORT, std::system_category(),
                            "Unsupported socket family for ECN receive");
  }
#endif
}

// Resolve a numeric IPv4 or IPv6 address string and port into an Endpoint
Endpoint resolve_endpoint(const char *addr, uint16_t port) {
  Endpoint ep{};

  // Try IPv4
  {
    auto *v4 = reinterpret_cast<sockaddr_in *>(&ep.sa);
    std::memset(v4, 0, sizeof(*v4));
    v4->sin_family = AF_INET;
    v4->sin_port = htons(port);
    int rc = inet_pton(AF_INET, addr, &v4->sin_addr);
    if (rc == 1) {
      ep.len = static_cast<socklen_t>(sizeof(sockaddr_in));
      return ep;
    }
  }

  // Try IPv6
  {
    auto *v6 = reinterpret_cast<sockaddr_in6 *>(&ep.sa);
    std::memset(v6, 0, sizeof(*v6));
    v6->sin6_family = AF_INET6;
    v6->sin6_port = htons(port);
    int rc = inet_pton(AF_INET6, addr, &v6->sin6_addr);
    if (rc == 1) {
      ep.len = static_cast<socklen_t>(sizeof(sockaddr_in6));
      return ep;
    }
  }

  throw std::system_error(EAFNOSUPPORT, std::system_category(),
                          "Unsupported address type");
}

#ifdef _WIN32
bool parse_ecn_cmsg(PCMSGHDR c, ecn_tp &ecn) {
  if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_ECN) {
    ecn = ecn_tp(*(PINT)WSA_CMSG_DATA(c));
    return true;
  }

  if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_ECN) {
    ecn = ecn_tp(*(PINT)WSA_CMSG_DATA(c));
    return true;
  }

  return false;
}
void fill_ecn_cmsg(PCMSGHDR c, int family, ecn_tp ecn) {
  c->cmsg_len = WSA_CMSG_LEN(sizeof(INT));
  c->cmsg_level = (family == AF_INET) ? IPPROTO_IP : IPPROTO_IPV6;
  c->cmsg_type = (family == AF_INET) ? IP_ECN : IPV6_ECN;
  *(PINT)WSA_CMSG_DATA(c) = ecn;
}
#else
// ECN is stored in the low 2 bits of the IP TOS (IPv4) or Traffic Class (IPv6).
ecn_tp decode_ecn(int tos_or_tc) {
  return static_cast<ecn_tp>(tos_or_tc & ECN_MASK);
}

int encode_ecn(ecn_tp e) { return int(e) & ECN_MASK; }

bool parse_ecn_cmsg(cmsghdr *c, ecn_tp &ecn) {
  if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECV_CMSG_TYPE) {
    int tos;
    memcpy(&tos, CMSG_DATA(c), sizeof(tos));
    ecn = decode_ecn(tos);
    return true;
  }

  if (c->cmsg_level == IPPROTO_IPV6 && c->cmsg_type == IPV6_RECV_CMSG_TYPE) {
    int tc;
    memcpy(&tc, CMSG_DATA(c), sizeof(tc));
    ecn = decode_ecn(tc);
    return true;
  }

  return false;
}

void fill_ecn_cmsg(cmsghdr *c, int family, ecn_tp ecn) {
  c->cmsg_len = CMSG_LEN(sizeof(int));

  if (family == AF_INET) {
    c->cmsg_level = IPPROTO_IP;
    c->cmsg_type = IP_TOS;
  } else {
    c->cmsg_level = IPPROTO_IPV6;
    c->cmsg_type = IPV6_TCLASS;
  }

  int v = encode_ecn(ecn);
  memcpy(CMSG_DATA(c), &v, sizeof(v));
}
#endif

// Elevate process/thread priority to maximize scheduling responsiveness.
void set_max_priority() {
#ifdef _WIN32
  DWORD dwPriClass;
  if (!SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS))
    perror("SetPriorityClass failed.\n");
  dwPriClass = GetPriorityClass(GetCurrentProcess());
  DWORD dwThreadPri;
  if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL))
    perror("SetThreadPriority failed.\n");
  dwThreadPri = GetThreadPriority(GetCurrentThread());
  printf("Current priority class is 0x%x, thread priority is 0x%x\n",
         (uint32_t)dwPriClass, (uint32_t)dwThreadPri);
#elif defined(__MUSL__)
  if (geteuid() == 0) {
    struct sched_param sp;
    sp.sched_priority = sched_get_priority_max(SCHED_RR);
    // SCHED_OTHER, SCHED_FIFO, SCHED_RR
    if (pthread_setschedparam(pthread_self(), SCHED_RR, &sp) < 0) {
      perror("Client set scheduler");
    }
  }
#elif defined(__linux__) || defined(__FreeBSD__)
  if (geteuid() == 0) {
    struct sched_param sp;
    sp.sched_priority = sched_get_priority_max(SCHED_RR);
    // SCHED_OTHER, SCHED_FIFO, SCHED_RR
    if (sched_setscheduler(0, SCHED_RR, &sp) < 0) {
      perror("Client set scheduler");
    }
  }
#elif __APPLE__
  if (geteuid() == 0) {
    struct sched_param sp;
    sp.sched_priority = sched_get_priority_max(SCHED_RR);
    // SCHED_OTHER, SCHED_FIFO, SCHED_RR
    if (pthread_setschedparam(pthread_self(), SCHED_RR, &sp) < 0) {
      perror("Client set scheduler");
    }
  }
#endif
}

UDPSocket::UDPSocket()
    :
#ifdef _WIN32
      WSARecvMsg(NULL), WSASendMsg(NULL),
#endif
      socket(invalid_socket()), peer{}, connected(false) {

  set_max_priority();

#ifdef _WIN32
  // Initialize Winsock
  WORD versionRequested = MAKEWORD(2, 2);
  if (WSAStartup(versionRequested, &wsaData) != 0) {
    perror("WSAStartup failed.\n");
    exit(1);
  }
#endif
}

UDPSocket::~UDPSocket() {
  close_socket(socket);
#ifdef _WIN32
  WSACleanup();
#endif
  socket = invalid_socket();
}

void UDPSocket::init_io() {
#ifdef _WIN32
  // Initialize recv and send functions
  GUID guidWSARecvMsg = WSAID_WSARECVMSG;
  GUID guidWSASendMsg = WSAID_WSASENDMSG;
  DWORD dwBytes = 0;
  if (SOCKET_ERROR == WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                               &guidWSARecvMsg, sizeof(guidWSARecvMsg),
                               &WSARecvMsg, sizeof(WSARecvMsg), &dwBytes, NULL,
                               NULL))
    throw std::system_error(WSAGetLastError(), std::system_category(),
                            "WSAIoctl(WSARecvMsg)");

  if (SOCKET_ERROR == WSAIoctl(socket, SIO_GET_EXTENSION_FUNCTION_POINTER,
                               &guidWSASendMsg, sizeof(guidWSASendMsg),
                               &WSASendMsg, sizeof(WSASendMsg), &dwBytes, NULL,
                               NULL))
    throw std::system_error(WSAGetLastError(), std::system_category(),
                            "WSAIoctl(WSASendMsg)");

  assert(WSARecvMsg != nullptr);
  assert(WSASendMsg != nullptr);

  sendControlBuf.buf = sendControl;
  sendControlBuf.len = sizeof(sendControl);
  sendMsg.lpBuffers = &dataBuf;
  sendMsg.dwBufferCount = 1;
  sendMsg.Control = sendControlBuf;
  sendMsg.dwFlags = 0;

  recvControlBuf.buf = recvControl;
  recvControlBuf.len = sizeof(recvControl);
  recvMsg.lpBuffers = &dataBuf;
  recvMsg.dwBufferCount = 1;
  recvMsg.Control = recvControlBuf;
  recvMsg.dwFlags = 0;

#else
  send_msg.msg_iov = &send_iov;
  send_msg.msg_iovlen = 1;
  send_msg.msg_control = send_ctrl;
  send_msg.msg_controllen = sizeof(send_ctrl);

  recv_msg.msg_iov = &recv_iov;
  recv_msg.msg_iovlen = 1;
  recv_msg.msg_control = recv_ctrl;
  recv_msg.msg_controllen = sizeof(recv_ctrl);
#endif
}

void UDPSocket::Bind(const char *addr, uint16_t port) {
  // Clean up on re-bind
  if (is_socket_valid(socket))
    close_socket(socket);

  Endpoint ep = resolve_endpoint(addr, port);

  socket = make_socket(ep.sa.ss_family);
  init_io();
  enable_recv_ecn(socket, ep.sa.ss_family);

  if (::bind(socket, reinterpret_cast<const sockaddr *>(&ep.sa), ep.len) ==
      SOCKET_ERROR)
    throw std::system_error(last_error_code(), std::system_category(), "bind");
}
void UDPSocket::Connect(const char *addr, uint16_t port) {
  assert(addr != nullptr);

  // Clean up on re-connect
  if (is_socket_valid(socket))
    close_socket(socket);

  peer = resolve_endpoint(addr, port);
  socket = make_socket(peer.sa.ss_family);
  init_io();
  enable_recv_ecn(socket, peer.sa.ss_family);

  if (::connect(socket, reinterpret_cast<sockaddr *>(&peer.sa), peer.len) ==
      SOCKET_ERROR)
    throw std::system_error(last_error_code(), std::system_category(),
                            "connect");

  connected = true;
#ifdef _WIN32
  recvMsg.name = nullptr;
  recvMsg.namelen = 0;
  sendMsg.name = nullptr;
  sendMsg.namelen = 0;
#else
  send_msg.msg_name = nullptr;
  send_msg.msg_namelen = 0;
  recv_msg.msg_name = nullptr;
  recv_msg.msg_namelen = 0;
#endif
}

size_tp UDPSocket::Receive(char *buf, size_tp len, ecn_tp &ecn,
                           time_tp timeout) {
  assert(buf != nullptr);
  assert(len > 0);
  assert(is_socket_valid(socket));

  if (timeout > 0 && !wait_for_readable(socket, timeout))
    return 0;

#ifdef _WIN32
  DWORD numBytes;
  INT error;

  PCMSGHDR cmsg;

  dataBuf.buf = buf;
  dataBuf.len = ULONG(len);

  if (!connected) {
    recvMsg.name = (PSOCKADDR)(&peer.sa);
    recvMsg.namelen = sizeof(peer.sa);
  }

  error = WSARecvMsg(socket, &recvMsg, &numBytes, NULL, NULL);

  if (error == SOCKET_ERROR)
    throw std::system_error(last_error_code(), std::system_category(),
                            "WSARecvMsg");

  peer.len = static_cast<socklen_t>(recvMsg.namelen);

  cmsg = WSA_CMSG_FIRSTHDR(&recvMsg);
  while (cmsg != NULL) {
    if (parse_ecn_cmsg(cmsg, ecn))
      break;
    cmsg = WSA_CMSG_NXTHDR(&recvMsg, cmsg);
  }

  return static_cast<size_t>(numBytes);
#else
  ssize_t r;
  recv_iov.iov_len = len;
  recv_iov.iov_base = buf;

  // On unconnected UDP sockets, recvmsg() uses msg_name as output buffer.
  if (!connected) {
    recv_msg.msg_name = &peer.sa;
    recv_msg.msg_namelen = sizeof(peer.sa);
  }

  if ((r = recvmsg(socket, &recv_msg, 0)) < 0)
    throw std::system_error(last_error_code(), std::system_category(),
                            "Fail to recv UDP message from socket");

  // The kernel filled in the actual sender address length.
  peer.len = static_cast<socklen_t>(recv_msg.msg_namelen);

  // Iterate over all control messages attached to this packet.
  // The kernel may include other control data besides ECN.
  for (cmsghdr *c = CMSG_FIRSTHDR(&recv_msg); c;
       c = CMSG_NXTHDR(&recv_msg, c)) {
    if (!parse_ecn_cmsg(c, ecn)) {
      printf("CMSG LEVEL: %d; CMSG TYPE: %d\n", c->cmsg_level, c->cmsg_type);
      perror("Fail to recv IP.ECN field from packet\n");
      exit(1);
    }
  }

  return static_cast<size_tp>(r);
#endif
}

size_tp UDPSocket::Send(char *buf, size_tp len, ecn_tp ecn) {
  assert(ecn == ecn_not_ect || ecn == ecn_ect0 || ecn == ecn_l4s_id ||
         ecn == ecn_ce);

#ifdef _WIN32
  DWORD numBytes;
  INT error;

  PCMSGHDR cmsg;

  dataBuf.buf = buf;
  dataBuf.len = ULONG(len);

  if (connected) { // Used only with unconnected sockets
    sendMsg.name = nullptr;
    sendMsg.namelen = 0;
  } else {
    sendMsg.name = (PSOCKADDR)(&peer.sa);
    sendMsg.namelen = peer.len;
  }

  cmsg = WSA_CMSG_FIRSTHDR(&sendMsg);
  fill_ecn_cmsg(cmsg, peer.family(), ecn);

  error = WSASendMsg(socket, &sendMsg, 0, &numBytes, NULL, NULL);

  if (error == SOCKET_ERROR)
    throw std::system_error(last_error_code(), std::system_category(),
                            "WSASendMsg");

  return static_cast<size_t>(numBytes);
#else
  send_iov.iov_base = buf;
  send_iov.iov_len = len;

  // On unconnected UDP sockets, sendmsg() requires a destination address
  // in msg_name. On connected sockets, this must be NULL.
  if (!connected) {
    send_msg.msg_name = &peer.sa;
    send_msg.msg_namelen = peer.len;
  }

  cmsghdr *cmsg = CMSG_FIRSTHDR(&send_msg);
  fill_ecn_cmsg(cmsg, peer.family(), ecn);

  ssize_t rc = sendmsg(socket, &send_msg, 0);

  if (rc < 0)
    throw std::system_error(errno, std::system_category(), "sendmsg");

  return static_cast<size_tp>(rc);
#endif
}
