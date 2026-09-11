#ifndef LIVING_ACTIVITY_ADMISSION_H
#define LIVING_ACTIVITY_ADMISSION_H
#include <cstddef>
#include <cstdint>

namespace LivingActivity {
    // A tested gate for the existing observer loop, not another scheduler.
    // It never discards an accepted write or an unacknowledged receipt.
    enum class ObservationWork { Wait, Probe, Decode, Flush, Load, Import, CachePressure, HistoryPressure };
    struct ObservationQueue {
        bool enabled = false, ioPending = false, due = false, schemaReady = false, loaded = false;
        // pending counts writes whose OWN retry deadline has arrived. due is
        // the background import/load clock; it must not postpone those writes.
        size_t cached = 0, pending = 0, incoming = 0, cacheLimit = 20000;
        // Due outcomes from the already bounded sixteen-operation admission
        // book. Their evidence/save slots were reserved before native effects.
        // Disabling NEW work must not strand those effects or actor-save holds.
        size_t nativeOutcomes = 0;
        uint64_t retained = 0, historyLimit = 200000;
    };
    inline ObservationWork NextObservationWork(const ObservationQueue& q) {
        if (q.ioPending) return ObservationWork::Wait;
        // Admission/history pressure stops new work, not reserved completion
        // receipts. No decoding, loading, import, or native dispatch is enabled.
        if (q.schemaReady && q.nativeOutcomes && q.nativeOutcomes<=16 && q.nativeOutcomes<=q.pending)
            return ObservationWork::Flush;
        if (!q.enabled) return ObservationWork::Wait;
        if (!q.schemaReady) return q.due ? ObservationWork::Probe : ObservationWork::Wait;
        // Flush already decoded rows before requesting more cache space. A full
        // pending batch must not deadlock behind its own admission capacity.
        if (q.pending) {
            if (q.retained >= q.historyLimit || q.pending > q.historyLimit - q.retained)
                return ObservationWork::HistoryPressure;
            return ObservationWork::Flush;
        }
        if (!q.due) return ObservationWork::Wait;
        if (q.retained >= q.historyLimit) return ObservationWork::HistoryPressure;
        if (q.cached >= q.cacheLimit) return ObservationWork::CachePressure;
        if (q.incoming) return ObservationWork::Decode;
        return q.loaded ? ObservationWork::Import : ObservationWork::Load;
    }
    inline unsigned NextImportFamily(unsigned family) { return (family + 1) % 4; }
}
#endif
