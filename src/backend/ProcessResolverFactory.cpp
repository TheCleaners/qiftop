#include "ProcessResolverFactory.h"

#include "null/NullProcessResolver.h"
#include "util/Logging.h"

// Linux platform headers come in only if the platform backend was selected
// AND at least one of the three attribution features was compiled in.
// Step 1 ships the Null fallback only; SockDiag / Cgroup / Netns concrete
// resolvers land in steps 2-4 and will be wired into this same factory
// behind the corresponding #ifdef guards.
#if defined(BACKEND_LINUX) && \
    (defined(QIFTOP_ENABLE_PROCESS_ATTRIBUTION) ||   \
     defined(QIFTOP_ENABLE_CONTAINER_ATTRIBUTION) || \
     defined(QIFTOP_ENABLE_NETNS_SCAN))
#  define QIFTOP_HAS_LINUX_ATTRIBUTION 1
#endif

namespace qiftop::backend {

std::unique_ptr<ProcessResolver>
createProcessResolver(const ProcessResolverConfig &cfg)
{
#ifdef QIFTOP_HAS_LINUX_ATTRIBUTION
    // STEP 2-4 TODO: when SockDiagResolver / CgroupClassifier / NetnsScanner
    // land, this block will instantiate a CompositeResolver wiring them
    // up according to `cfg`. For step 1 we deliberately keep the surface
    // small and fall through to Null so the rest of the foundation can
    // be tested without dragging in netlink dependencies.
    (void)cfg;
    qCInfo(lcVerbose) << "ProcessResolverFactory: Linux attribution backend "
                         "compiled in; concrete impl arrives in step 2 — "
                         "using Null resolver for now";
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
