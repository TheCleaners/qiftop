#include "ProcessResolverFactory.h"

#include "null/NullProcessResolver.h"
#include "util/Logging.h"

// Linux platform headers come in only if the platform backend was selected
// AND at least one of the three attribution features was compiled in.
// Step 2 brings the SockDiagResolver for host-netns process attribution;
// step 3 (cgroup) and step 4 (netns) compose on top.
#if defined(BACKEND_LINUX) && \
    (defined(QIFTOP_ENABLE_PROCESS_ATTRIBUTION) ||   \
     defined(QIFTOP_ENABLE_CONTAINER_ATTRIBUTION) || \
     defined(QIFTOP_ENABLE_NETNS_SCAN))
#  define QIFTOP_HAS_LINUX_ATTRIBUTION 1
#endif

#ifdef QIFTOP_HAS_LINUX_ATTRIBUTION
#  ifdef QIFTOP_ENABLE_PROCESS_ATTRIBUTION
#    include "linux/SockDiagResolver.h"
#  endif
#endif

namespace qiftop::backend {

std::unique_ptr<ProcessResolver>
createProcessResolver(const ProcessResolverConfig &cfg)
{
#ifdef QIFTOP_HAS_LINUX_ATTRIBUTION
#  ifdef QIFTOP_ENABLE_PROCESS_ATTRIBUTION
    if (cfg.processAttribution) {
        auto r = std::make_unique<linuximpl::SockDiagResolver>();
        if (r->initialize()) {
            qCInfo(lcVerbose) << "ProcessResolverFactory: SockDiagResolver active"
                              << "caps=" << r->capabilities();
            // STEP 3-4 TODO: wrap in CompositeResolver that also queries the
            // CgroupClassifier and (optionally) NetnsScanner. For step 2 we
            // return the host-netns sock_diag resolver alone.
            return r;
        }
        qCInfo(lcVerbose) << "ProcessResolverFactory: SockDiagResolver probe failed; "
                             "degrading to Null";
    }
#  endif
    (void)cfg;
#else
    (void)cfg;
    qCInfo(lcVerbose) << "ProcessResolverFactory: no attribution backend "
                         "compiled in for this platform — using Null resolver";
#endif

    auto resolver = std::make_unique<NullProcessResolver>();
    resolver->initialize();
    return resolver;
}

} // namespace qiftop::backend
