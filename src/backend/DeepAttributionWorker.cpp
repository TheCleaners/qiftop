#include "backend/DeepAttributionWorker.h"

// Translation unit exists so AUTOMOC generates and compiles the moc for the
// abstract DeepAttributionWorker QObject (it has Q_OBJECT + signals but is
// otherwise header-only). Without a .cpp of the same basename, its
// staticMetaObject / vtable would be undefined at link time.
