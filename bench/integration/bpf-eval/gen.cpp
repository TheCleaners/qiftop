// bpf-eval ground-truth traffic generator.
//
// Spawns many worker PROCESSES (→ many PIDs, for the many-PID / PID-reuse
// scenarios) that open known flows to the sink and record, for EVERY flow, an
// authoritative ground-truth record — independent of any capture path. This
// log is the gold standard the scorer measures conntrack/pcap/eBPF against.
//
// Direction is definitional here: the worker always calls connect(), so it is
// the initiator ⇒ the flow is OUTBOUND from this host. The remote is the sink
// (run in a separate "peer" netns by the orchestrator, so it is a genuinely
// non-local address — loopback would make direction ambiguous; see
// DESIGN.md §3).
//
// Plain sockets, no Qt, no special deps.

#include <arpa/inet.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

int64_t monoMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// /proc/self/stat field 22 (starttime in clock ticks since boot). Robust to a
// comm containing spaces/parens by scanning to the LAST ')'.
uint64_t selfStartTime()
{
    FILE *f = std::fopen("/proc/self/stat", "re");
    if (!f) return 0;
    std::string s;
    int c;
    while ((c = std::fgetc(f)) != EOF) s.push_back(static_cast<char>(c));
    std::fclose(f);
    const auto rp = s.rfind(')');
    if (rp == std::string::npos) return 0;
    // After ") ", fields 3.. are space-separated; starttime is field 22, i.e.
    // the 20th token after the comm.
    std::vector<std::string> toks;
    std::string cur;
    for (size_t i = rp + 2; i < s.size(); ++i) {
        const char ch = s[i];
        if (ch == ' ') { if (!cur.empty()) { toks.push_back(cur); cur.clear(); } }
        else cur.push_back(ch);
    }
    if (!cur.empty()) toks.push_back(cur);
    // toks[0] is field 3 (state). starttime is field 22 → index 19.
    if (toks.size() > 19) return std::strtoull(toks[19].c_str(), nullptr, 10);
    return 0;
}

struct Target {
    int         family = AF_INET;
    sockaddr_storage addr{};
    socklen_t   addrlen = 0;
    std::string remoteIp;
    uint16_t    remotePort = 0;
};

bool resolveTarget(const std::string &host, uint16_t port, int family, Target &out)
{
    addrinfo hints{};
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    const std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res)
        return false;
    out.family  = res->ai_family;
    out.addrlen = res->ai_addrlen;
    std::memcpy(&out.addr, res->ai_addr, res->ai_addrlen);
    char ip[INET6_ADDRSTRLEN] = {0};
    if (res->ai_family == AF_INET)
        ::inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(res->ai_addr)->sin_addr, ip, sizeof(ip));
    else
        ::inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6 *>(res->ai_addr)->sin6_addr, ip, sizeof(ip));
    out.remoteIp   = ip;
    out.remotePort = port;
    ::freeaddrinfo(res);
    return true;
}

void localEndpoint(int fd, std::string &ip, uint16_t &port)
{
    sockaddr_storage ss{};
    socklen_t sl = sizeof(ss);
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&ss), &sl) != 0) return;
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        auto *a = reinterpret_cast<sockaddr_in *>(&ss);
        ::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
        port = ntohs(a->sin_port);
    } else {
        auto *a = reinterpret_cast<sockaddr_in6 *>(&ss);
        ::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
        port = ntohs(a->sin6_port);
    }
    ip = buf;
}

// One flow's authoritative record, appended as NDJSON.
struct FlowRecord {
    int64_t  flowId;
    bool     udp;
    int      family;
    std::string localIp; uint16_t localPort;
    std::string remoteIp; uint16_t remotePort;
    int      pid;
    uint64_t startTime;
    uint64_t bytesL2R, bytesR2L;
    int64_t  tOpen, tClose;
};

void writeRecord(FILE *out, const FlowRecord &r)
{
    std::fprintf(out,
        "{\"flow_id\":%lld,\"proto\":\"%s\",\"family\":%d,"
        "\"local_ip\":\"%s\",\"local_port\":%u,"
        "\"remote_ip\":\"%s\",\"remote_port\":%u,"
        "\"pid\":%d,\"starttime\":%llu,\"initiator\":\"outbound\","
        "\"bytes_l2r\":%llu,\"bytes_r2l\":%llu,"
        "\"t_open_ms\":%lld,\"t_close_ms\":%lld}\n",
        static_cast<long long>(r.flowId), r.udp ? "udp" : "tcp", r.family,
        r.localIp.c_str(), r.localPort, r.remoteIp.c_str(), r.remotePort,
        r.pid, static_cast<unsigned long long>(r.startTime),
        static_cast<unsigned long long>(r.bytesL2R),
        static_cast<unsigned long long>(r.bytesR2L),
        static_cast<long long>(r.tOpen), static_cast<long long>(r.tClose));
}

// Open one flow, transfer `bytes`, optionally drain echo, hold `holdMs`, close.
// Returns false on connect failure (logged as a not-captured-by-design event
// only if it actually opened). Fills the record.
bool runFlow(const Target &tgt, bool udp, uint64_t bytes, int holdMs,
             int pid, uint64_t startTime, int64_t flowId, FlowRecord &rec)
{
    const int type = udp ? SOCK_DGRAM : SOCK_STREAM;
    const int fd = ::socket(tgt.family, type, 0);
    if (fd < 0) return false;

    const int64_t tOpen = monoMs();
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&tgt.addr), tgt.addrlen) != 0) {
        ::close(fd);
        return false;
    }

    std::string lip; uint16_t lport = 0;
    localEndpoint(fd, lip, lport);

    std::vector<char> chunk(std::size_t{16} * 1024, 'x');
    uint64_t sent = 0;
    while (sent < bytes) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(chunk.size(), bytes - sent));
        const ssize_t w = ::send(fd, chunk.data(), want, MSG_NOSIGNAL);
        if (w <= 0) break;
        sent += static_cast<uint64_t>(w);
    }

    // Drain a little echo back so byte_r2l is non-zero (TCP echo sink); bounded.
    uint64_t recvd = 0;
    if (!udp) {
        ::shutdown(fd, SHUT_WR);
        std::vector<char> rb(std::size_t{16} * 1024);
        for (;;) {
            const ssize_t r = ::recv(fd, rb.data(), rb.size(), 0);
            if (r <= 0) break;
            recvd += static_cast<uint64_t>(r);
        }
    }

    if (holdMs > 0)
        ::usleep(static_cast<useconds_t>(holdMs) * 1000);

    ::close(fd);
    const int64_t tClose = monoMs();

    rec = FlowRecord{flowId, udp, tgt.family, lip, lport, tgt.remoteIp,
                     tgt.remotePort, pid, startTime, sent, recvd, tOpen, tClose};
    return true;
}

[[noreturn]] void usage()
{
    std::printf(
        "usage: qiftop-bpfeval-gen --sink-host H --sink-port P --out-dir DIR\n"
        "  [--workers N] [--flows-per-worker M] [--proto tcp|udp|mix]\n"
        "  [--family 4|6|mix] [--bytes B] [--mode concurrent|churn]\n"
        "  [--hold-ms MS]       (concurrent: keep flows open this long)\n"
        "  [--lifetime-ms MS]   (churn: per-flow lifetime; <1000 = sub-poll)\n"
        "  [--duration-ms MS]   (churn: keep churning for this long)\n");
    std::exit(2);
}

} // namespace

int main(int argc, char **argv)
{
    std::string host, outDir, proto = "tcp", family = "4", mode = "concurrent";
    uint16_t port = 18080;
    int workers = 1, flowsPerWorker = 100, bytes = 4096;
    int holdMs = 2000, lifetimeMs = 200, durationMs = 5000;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto nx = [&]() -> std::string { if (i + 1 >= argc) usage(); return argv[++i]; };
        if      (a == "--sink-host") host = nx();
        else if (a == "--sink-port") port = static_cast<uint16_t>(std::stoi(nx()));
        else if (a == "--out-dir") outDir = nx();
        else if (a == "--workers") workers = std::stoi(nx());
        else if (a == "--flows-per-worker") flowsPerWorker = std::stoi(nx());
        else if (a == "--proto") proto = nx();
        else if (a == "--family") family = nx();
        else if (a == "--bytes") bytes = std::stoi(nx());
        else if (a == "--mode") mode = nx();
        else if (a == "--hold-ms") holdMs = std::stoi(nx());
        else if (a == "--lifetime-ms") lifetimeMs = std::stoi(nx());
        else if (a == "--duration-ms") durationMs = std::stoi(nx());
        else if (a == "--help") usage();
    }
    if (host.empty() || outDir.empty()) usage();

    std::signal(SIGCHLD, SIG_DFL);

    std::vector<pid_t> kids;
    for (int w = 0; w < workers; ++w) {
        const pid_t pid = ::fork();
        if (pid < 0) { std::perror("fork"); break; }
        if (pid == 0) {
            // --- worker ---
            const int mypid = ::getpid();
            const uint64_t st = selfStartTime();
            const std::string path = outDir + "/gt." + std::to_string(mypid) + ".ndjson";
            FILE *out = std::fopen(path.c_str(), "we");
            if (!out) { std::perror("gt open"); ::_exit(1); }

            auto pick = [&](int idx) -> std::pair<bool,int> {
                const bool udp = (proto == "udp") ? true
                               : (proto == "mix") ? (idx % 3 == 0) : false;
                int fam = (family == "6") ? AF_INET6
                        : (family == "mix") ? ((idx % 2) ? AF_INET6 : AF_INET)
                        : AF_INET;
                return {udp, fam};
            };

            int64_t flowId = static_cast<int64_t>(mypid) * 1000000;

            if (mode == "concurrent") {
                // Open M flows, hold them all open, then they close on scope.
                // We run them sequentially-open but hold each via holdMs at the
                // end; to get true concurrency we fork per-flow would be heavy,
                // so emulate by opening all sockets then sleeping then closing.
                struct Live { int fd; FlowRecord rec; };
                std::vector<Live> live;
                live.reserve(flowsPerWorker);
                for (int m = 0; m < flowsPerWorker; ++m) {
                    auto [udp, fam] = pick(m);
                    Target tgt;
                    if (!resolveTarget(host, port, fam, tgt)) continue;
                    const int type = udp ? SOCK_DGRAM : SOCK_STREAM;
                    const int fd = ::socket(tgt.family, type, 0);
                    if (fd < 0) continue;
                    const int64_t tOpen = monoMs();
                    if (::connect(fd, reinterpret_cast<sockaddr *>(&tgt.addr), tgt.addrlen) != 0) {
                        ::close(fd); continue;
                    }
                    std::string lip; uint16_t lport = 0;
                    localEndpoint(fd, lip, lport);
                    std::vector<char> chunk(std::min<uint64_t>(bytes, std::size_t{16} * 1024), 'x');
                    uint64_t sent = 0;
                    while (sent < static_cast<uint64_t>(bytes)) {
                        const size_t want = std::min(chunk.size(), static_cast<size_t>(bytes) - sent);
                        const ssize_t wr = ::send(fd, chunk.data(), want, MSG_NOSIGNAL);
                        if (wr <= 0) break;
                        sent += static_cast<uint64_t>(wr);
                    }
                    FlowRecord rec{flowId++, udp, tgt.family, lip, lport, tgt.remoteIp,
                                   tgt.remotePort, mypid, st, sent, 0, tOpen, 0};
                    live.push_back({fd, rec});
                }
                if (holdMs > 0) ::usleep(static_cast<useconds_t>(holdMs) * 1000);
                for (auto &l : live) {
                    ::close(l.fd);
                    l.rec.tClose = monoMs();
                    writeRecord(out, l.rec);
                }
            } else { // churn
                const int64_t deadline = monoMs() + durationMs;
                int idx = 0;
                while (monoMs() < deadline) {
                    auto [udp, fam] = pick(idx);
                    Target tgt;
                    if (resolveTarget(host, port, fam, tgt)) {
                        FlowRecord rec;
                        if (runFlow(tgt, udp, static_cast<uint64_t>(bytes), lifetimeMs,
                                    mypid, st, flowId++, rec))
                            writeRecord(out, rec);
                    }
                    ++idx;
                }
            }
            std::fclose(out);
            ::_exit(0);
        }
        kids.push_back(pid);
    }

    for (pid_t k : kids) { int status = 0; ::waitpid(k, &status, 0); }
    std::fprintf(stderr, "gen: %zu workers done\n", kids.size());
    return 0;
}
