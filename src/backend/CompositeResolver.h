#pragma once

#include <memory>
#include <vector>

#include "ProcessResolver.h"

namespace qiftop::backend {

// Fans resolveFlow / resolveContainerForPid out to a chain of inner
// resolvers, returning the FIRST non-nullopt answer for each query.
// capabilities() is the dedup'd union across children.
//
// This is what the factory returns once attribution is composed of
// multiple data sources (sock_diag for PID, cgroup for container,
// later netns-scan for container-side PID). Each inner resolver stays
// single-purpose and unit-testable in isolation.
//
// Ownership: composite owns its children; they're destroyed in
// reverse-construction order.
class CompositeResolver final : public ProcessResolver {
public:
    void add(std::unique_ptr<ProcessResolver> child)
    {
        if (child) m_children.push_back(std::move(child));
    }

    bool initialize() override
    {
        bool anyReady = false;
        for (auto &c : m_children) anyReady |= c->initialize();
        return anyReady;
    }

    [[nodiscard]] QStringList capabilities() const override
    {
        QStringList out;
        for (const auto &c : m_children) {
            for (const auto &tok : c->capabilities()) {
                if (!out.contains(tok)) out.append(tok);
            }
        }
        return out;
    }

    [[nodiscard]] qint32 resolvePid(const Connection &flow) override
    {
        for (auto &c : m_children) {
            const qint32 pid = c->resolvePid(flow);
            if (pid > 0) return pid;
        }
        return 0;
    }

    [[nodiscard]] std::optional<ProcessInfo>
        enrichPid(qint32 pid) override
    {
        for (auto &c : m_children) {
            if (auto r = c->enrichPid(pid)) return r;
        }
        return std::nullopt;
    }

    [[nodiscard]] std::optional<ContainerInfo>
        resolveContainerForPid(qint32 pid) override
    {
        for (auto &c : m_children) {
            if (auto r = c->resolveContainerForPid(pid)) return r;
        }
        return std::nullopt;
    }

    [[nodiscard]] QList<ContainerInfo>
        resolveContainerChainForPid(qint32 pid) override
    {
        // First child that returns a non-empty chain wins. We avoid
        // the base-class default (which would fall back to single
        // resolveContainerForPid) so children advertising the
        // `container-chain` capability get to provide the real chain.
        for (auto &c : m_children) {
            auto chain = c->resolveContainerChainForPid(pid);
            if (!chain.isEmpty()) return chain;
        }
        return {};
    }

private:
    std::vector<std::unique_ptr<ProcessResolver>> m_children;
};

} // namespace qiftop::backend
