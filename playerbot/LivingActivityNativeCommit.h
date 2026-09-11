#ifndef LIVING_ACTIVITY_NATIVE_COMMIT_H
#define LIVING_ACTIVITY_NATIVE_COMMIT_H
#include "LivingActivity.h"
#include <memory>
class Database;

namespace LivingActivity {
    enum class NativeReceipt { Unavailable, Absent, Present, Conflict };
    enum class NativeSaveStatus {
        Captured, Queued, CheckingReceipt, Saving, ReadyToCommit, Committed, AlreadyCommitted,
        QueueUnavailable, ReceiptUnavailable, ReceiptConflict, NativeProofUnavailable,
        NativeProofRejected, JournalProofUnavailable, JournalProofRejected, TransactionFailed, CommitUncertain
    };
    inline const char* Name(NativeSaveStatus status) {
        switch (status) {
#define LIVING_SAVE_STATUS(value, name) case NativeSaveStatus::value: return name
        LIVING_SAVE_STATUS(Captured,"native_save_captured"); LIVING_SAVE_STATUS(Queued,"native_save_queued");
        LIVING_SAVE_STATUS(CheckingReceipt,"native_save_checking_prior_receipt");
        LIVING_SAVE_STATUS(Saving,"native_transaction_executing");
        LIVING_SAVE_STATUS(ReadyToCommit,"native_proofs_validated_uncommitted");
        LIVING_SAVE_STATUS(Committed,"native_save_committed");
        LIVING_SAVE_STATUS(AlreadyCommitted,"native_save_prior_commit_reconciled");
        LIVING_SAVE_STATUS(QueueUnavailable,"native_save_queue_unavailable");
        LIVING_SAVE_STATUS(ReceiptUnavailable,"native_save_receipt_query_unavailable");
        LIVING_SAVE_STATUS(ReceiptConflict,"native_save_receipt_conflict");
        LIVING_SAVE_STATUS(NativeProofUnavailable,"native_save_postcondition_query_unavailable");
        LIVING_SAVE_STATUS(NativeProofRejected,"native_save_postcondition_rejected");
        LIVING_SAVE_STATUS(JournalProofUnavailable,"native_save_journal_query_unavailable");
        LIVING_SAVE_STATUS(JournalProofRejected,"native_save_journal_cas_rejected");
        LIVING_SAVE_STATUS(TransactionFailed,"native_save_transaction_failed_before_proof");
        LIVING_SAVE_STATUS(CommitUncertain,"native_save_commit_ack_uncertain");
#undef LIVING_SAVE_STATUS
        }
        return "native_save_invalid_status";
    }
    // Pinned native QueryResult/Field contract, including its healthy sentinel.
    // Missing rows and a failed SELECT are deliberately different states.
    template<class NativeRows>
    NativeReceipt ProbeNativeReceipt(NativeRows* rows, const std::string& task, uint64_t revision) {
        if (!rows) return NativeReceipt::Unavailable;
        if (rows->GetFieldCount() != 2) return NativeReceipt::Conflict;
        bool healthy = false, matched = false;
        unsigned count = 0;
        do {
            if (++count > 2) return NativeReceipt::Conflict;
            const auto* fields = rows->Fetch();
            if (!fields) return NativeReceipt::Conflict;
            const auto id = fields[0].GetCppString(); const auto value = fields[1].GetUInt64();
            if (id.empty() && !value && !healthy) healthy = true;
            else if (id == task && value == revision && !matched) matched = true;
            else return NativeReceipt::Conflict;
        } while (rows->NextRow());
        if (!healthy) return NativeReceipt::Unavailable;
        return matched ? NativeReceipt::Present : NativeReceipt::Absent;
    }
    template<class Probe, class NativeTransaction>
    bool ExecuteRetainedNativeSave(Probe probe, NativeTransaction transaction) {
        const auto prior = probe();
        if (prior == NativeReceipt::Present) return true; // Lost acknowledgement; no replay.
        if (prior != NativeReceipt::Absent) return false;
        // The actual native transaction contains a final receipt assertion:
        // rejected CAS/journal writes roll back native inventory/money too.
        return transaction();
    }

    // Owns the actual native prepared/plain SQL operations, not copied Player or
    // Item objects, and uses the existing character DB delay queue. Retrying this
    // batch retries persistence only, NEVER the purchase/craft/gameplay action.
    // Callers must hold later actor writes until acknowledgement/reconciliation.
    class NativeSaveBatch {
    public:
        // nativeProofQuery is compiled adapter SQL derived from actual after-
        // state. It returns the same task/revision only if native rows match.
        // It is checked INSIDE the transaction, not inferred from journal text.
        static std::shared_ptr<NativeSaveBatch> Capture(Database& database, const WritePlan& journal,
            const std::string& nativeProofQuery);
        bool Queue(Database& database);
        bool InFlight() const;
        NativeSaveStatus Status() const;
        unsigned Attempts() const;
#ifdef LIVING_ISOLATED_NATIVE_TESTS
        // Available only in the separately marked test binary. Reject one
        // native proof and one journal proof inside the real SQL transaction.
        void InjectIsolatedProofFailures();
#endif
    private:
        struct State;
        explicit NativeSaveBatch(std::shared_ptr<State> state) : state(std::move(state)) {}
        std::shared_ptr<State> state;
    };
}
#endif
