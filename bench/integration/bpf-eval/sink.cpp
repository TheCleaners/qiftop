// bpf-eval sink: a minimal epoll-based TCP/UDP server that accepts many
// concurrent connections, drains (and optionally echoes) bytes so generated
// flows complete, and optionally logs each accepted connection as a
// cross-check on the generator's authoritative ground truth.
//
// Phase 0 of the conntrack-vs-BPF capture measurement harness. Plain sockets,
// no Qt, no special deps — always builds. Runs in the "remote peer" netns so
// generated flows have a genuinely non-local remote (loopback would make every
// flow's direction ambiguous; see DESIGN.md §3).

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

int makeListener(int family, uint16_t port)
{
    const int fd = ::socket(family, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (family == AF_INET) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) { ::close(fd); return -1; }
    } else {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_any;
        a.sin6_port = htons(port);
        int v6only = 1; // keep v4 + v6 listeners distinct
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) { ::close(fd); return -1; }
    }
    if (::listen(fd, 1024) < 0) { ::close(fd); return -1; }
    return fd;
}

int makeUdp(int family, uint16_t port)
{
    const int fd = ::socket(family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (family == AF_INET) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(port);
        if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) { ::close(fd); return -1; }
    } else {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_any;
        a.sin6_port = htons(port);
        int v6only = 1;
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
        if (::bind(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) { ::close(fd); return -1; }
    }
    return fd;
}

void epollAdd(int ep, int fd, uint32_t ev)
{
    epoll_event e{};
    e.events = ev;
    e.data.fd = fd;
    ::epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e);
}

} // namespace

int main(int argc, char **argv)
{
    uint16_t port  = 18080;
    bool     echo  = true;
    int      durationSecs = 0; // 0 = until SIGTERM/SIGINT
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--port") port = static_cast<uint16_t>(std::stoi(next()));
        else if (a == "--no-echo") echo = false;
        else if (a == "--duration") durationSecs = std::stoi(next());
        else if (a == "--help") {
            std::printf("usage: qiftop-bpfeval-sink [--port N] [--no-echo] [--duration S]\n");
            return 0;
        }
    }

    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT,  onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    const int ep = ::epoll_create1(0);
    if (ep < 0) { std::perror("epoll_create1"); return 1; }

    std::vector<int> listeners;
    for (int fam : {AF_INET, AF_INET6}) {
        const int l = makeListener(fam, port);
        if (l >= 0) { listeners.push_back(l); epollAdd(ep, l, EPOLLIN); }
    }
    std::vector<int> udps;
    for (int fam : {AF_INET, AF_INET6}) {
        const int u = makeUdp(fam, port);
        if (u >= 0) { udps.push_back(u); epollAdd(ep, u, EPOLLIN); }
    }
    if (listeners.empty() && udps.empty()) {
        std::fprintf(stderr, "sink: could not bind any socket on port %u\n", port);
        return 1;
    }
    std::fprintf(stderr, "sink: ready on port %u (tcp listeners=%zu udp=%zu echo=%d)\n",
                 port, listeners.size(), udps.size(), echo ? 1 : 0);

    auto isListener = [&](int fd) {
        for (int l : listeners) if (l == fd) return true;
        return false;
    };
    auto isUdp = [&](int fd) {
        for (int u : udps) if (u == fd) return true;
        return false;
    };

    std::vector<char> buf(64 * 1024);
    std::vector<epoll_event> evs(256);
    const time_t deadline = durationSecs > 0 ? ::time(nullptr) + durationSecs : 0;

    while (!g_stop) {
        if (deadline && ::time(nullptr) >= deadline) break;
        const int n = ::epoll_wait(ep, evs.data(), static_cast<int>(evs.size()), 500);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; ++i) {
            const int fd = evs[i].data.fd;
            if (isListener(fd)) {
                for (;;) {
                    const int c = ::accept4(fd, nullptr, nullptr, SOCK_NONBLOCK);
                    if (c < 0) break;
                    epollAdd(ep, c, EPOLLIN);
                }
            } else if (isUdp(fd)) {
                for (;;) {
                    sockaddr_storage from{};
                    socklen_t fl = sizeof(from);
                    const ssize_t r = ::recvfrom(fd, buf.data(), buf.size(), 0,
                                                 reinterpret_cast<sockaddr *>(&from), &fl);
                    if (r <= 0) break;
                    if (echo)
                        ::sendto(fd, buf.data(), static_cast<size_t>(r), 0,
                                 reinterpret_cast<sockaddr *>(&from), fl);
                }
            } else {
                // Connected TCP socket: drain (and echo) until EAGAIN / EOF.
                for (;;) {
                    const ssize_t r = ::recv(fd, buf.data(), buf.size(), 0);
                    if (r > 0) {
                        if (echo) {
                            ssize_t off = 0;
                            while (off < r) {
                                const ssize_t w = ::send(fd, buf.data() + off,
                                                         static_cast<size_t>(r - off), MSG_NOSIGNAL);
                                if (w <= 0) break;
                                off += w;
                            }
                        }
                        continue;
                    }
                    if (r == 0 || (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                        ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                        ::close(fd);
                    }
                    break;
                }
            }
        }
    }

    for (int l : listeners) ::close(l);
    for (int u : udps) ::close(u);
    ::close(ep);
    std::fprintf(stderr, "sink: stopped\n");
    return 0;
}
