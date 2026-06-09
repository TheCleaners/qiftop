#include "ProcessResolverFactory.h"

#include "CompositeResolver.h"
#include "null/NullProcessResolver.h"
#include "util/Logging.h"

// Linux platform headers come in only if the platform backend was selected
// AND at least one of the three attribution features was compiled in.
// Step 2 brings SockDiagResolver (process attribution), step 3 brings
// CgroupClassifier (container attribution); they compose via
// CompositeResolver. Step 4 adds NetnsScanner on top.
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
#  ifdef QIFTOP_ENABLE_CONTAINER_ATTRIBUTION
#    include "linux/CgroupClassifier.h"
#  endif
#endif

namespace qiftop::backend {

std::unique_ptr<ProcessResolver>
createProcessResolver(const ProcessResolverConfig &cfg)
{
#ifdef QIFTOP_HAS_LINUX_ATTRIBUTION
    auto composite = std::make_unique<CompositeResolver>();
#  ifdef QIFTOP_ENABLE_PROCESS_ATTRIBUTION
    if (cfg.processAttribution) {
        auto r = std::make_unique<linuximpl::SockDiagResolver>();
        if (r->initialize()) {
            qCInfo(lcVerbose) << "ProcessResolverFactory: SockDiagResolver added";
            composite->add(std::move(r));
        } else {
            qCInfo(lcVerbose) << "ProcessResolverFactory: SockDiagResolver probe failed";
        }
    }
#  endif
#  ifdef QIFTOP_ENABLE_CONTAINER_ATTRIBUTION
    if (cfg.containerAttribution) {
        auto r = std::make_unique<linuximpl::CgroupClassifier>();
        if (r->initialize()) {
            qCInfo(lcVerbose) << "ProcessResolverFactory: CgroupClassifier added";
            composite->add(std::move(r));
        } else {
            qCInfo(lcVerbose) << "ProcessResolverFactory: CgroupClassifier probe failed";
        }
    }
#  endif
    if (!composite->capabilities().isEmpty()) {
        qCInfo(lcVerbose) << "ProcessResolverFactory: composite caps="
                          << composite->capabilities();
        return composite;
    }
    qCInfo(lcVerbose) << "ProcessResolverFactory: every layer disabled or probe-failed; "
                         "degrading to Null";
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
