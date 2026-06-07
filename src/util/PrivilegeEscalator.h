#pragma once

#include <QObject>
#include <QStringList>

#include <functional>

namespace util {

// Identifiers for the desktop environments we know how to take advantage of
// when picking a privilege-escalation helper. Order is not significant.
enum class DesktopEnv {
    Unknown, KDE, GNOME, XFCE, MATE, Cinnamon, LXQt, Other
};

// Abstracts choosing and invoking a privilege-escalation helper (`pkexec`,
// `kdesu`, `gksudo`, …). The escalator inspects the running desktop session
// and proposes an ordered list of strategies — DE-specific helpers first,
// then progressively more generic ones — and runs them until one starts a
// child process successfully.
//
// The escalator never kills the parent application: helpers like pkexec
// refuse to authenticate if their caller has already exited (manifesting as
// "Refusing to render service to dead parents"). Callers should keep the
// current instance running and let the user close it once the privileged
// copy appears.
class PrivilegeEscalator : public QObject {
    Q_OBJECT

public:
    explicit PrivilegeEscalator(QObject *parent = nullptr);

    void setVerbose(bool v) { m_verbose = v; }

    [[nodiscard]] DesktopEnv  detectDesktop() const;
    [[nodiscard]] QStringList desktopTokens() const;

    // Lists the strategy IDs (`pkexec`, `kdesu`, …) that would be attempted
    // in order for the current environment, after filtering out helpers not
    // installed on the system. Useful for introspection / tests.
    [[nodiscard]] QStringList plannedStrategies() const;

    // Tries each strategy in order until one starts a privileged copy of
    // (program, args). Returns true and writes the chosen strategy id into
    // `usedStrategy` on success.
    bool relaunch(const QString &program,
                  const QStringList &args,
                  QString *usedStrategy = nullptr);

signals:
    void status(QString message);

private:
    struct Strategy {
        QString id;
        std::function<bool(PrivilegeEscalator *, const QString &, const QStringList &)> run;
    };

    [[nodiscard]] QList<Strategy> orderedStrategies() const;

    bool runPkexec  (const QString &program, const QStringList &args);
    bool runKdesu   (const QString &program, const QStringList &args);
    bool runGksu    (const QString &id, const QString &program, const QStringList &args);
    bool runTermSudo(const QString &program, const QStringList &args);

    bool m_verbose = false;
};

} // namespace util
