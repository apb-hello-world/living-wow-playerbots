#include "playerbot/playerbot.h"
#include "LivingActivityNativeCommit.h"
#include "Database/Database.h"
#include "Database/SqlOperations.h"
#include <atomic>
#include <stdexcept>
#ifdef LIVING_ISOLATED_NATIVE_TESTS
#include <cstdlib>
#include <fstream>
#endif

namespace LivingActivity {
    namespace {
        struct SaveTrace {
            std::atomic<NativeSaveStatus> status{NativeSaveStatus::Captured};
            std::atomic<unsigned> attempts{0};
#ifdef LIVING_ISOLATED_NATIVE_TESTS
            std::atomic<bool> failNativeProof{false}, failJournalProof{false};
#endif
        };
        NativeReceipt ReadReceipt(SqlConnection* connection, const std::string& query,
            const std::string& task, uint64_t revision) {
            auto rows = connection->Query(query.c_str());
            return ProbeNativeReceipt(rows.get(), task, revision);
        }
        class RequireReceipt final : public SqlOperation {
        public:
            RequireReceipt(std::string query, std::string task, uint64_t revision,
                std::shared_ptr<SaveTrace> trace, bool native)
                : query(std::move(query)), task(std::move(task)), revision(revision), trace(std::move(trace)), native(native) {}
            bool Execute(SqlConnection* connection) override {
                auto receipt = ReadReceipt(connection, query, task, revision);
#ifdef LIVING_ISOLATED_NATIVE_TESTS
                if ((native ? trace->failNativeProof : trace->failJournalProof).exchange(false))
                    receipt = NativeReceipt::Absent;
#endif
                if (receipt == NativeReceipt::Present) {
                    if (!native) trace->status.store(NativeSaveStatus::ReadyToCommit);
                    return true;
                }
                trace->status.store(native ?
                    (receipt == NativeReceipt::Unavailable ? NativeSaveStatus::NativeProofUnavailable : NativeSaveStatus::NativeProofRejected) :
                    (receipt == NativeReceipt::Unavailable ? NativeSaveStatus::JournalProofUnavailable : NativeSaveStatus::JournalProofRejected));
                return false;
            }
        private:
            std::string query, task;
            uint64_t revision;
            std::shared_ptr<SaveTrace> trace;
            bool native;
        };
    }
    struct NativeSaveBatch::State {
        std::unique_ptr<SqlTransaction> transaction;
        std::string query, task;
        uint64_t revision = 0;
        std::atomic<bool> queued{false};
        std::shared_ptr<SaveTrace> trace = std::make_shared<SaveTrace>();
        class Attempt final : public SqlOperation {
        public:
            explicit Attempt(std::shared_ptr<State> state) : state(std::move(state)) {}
            ~Attempt() override { state->queued.store(false, std::memory_order_release); }
            bool Execute(SqlConnection* connection) override {
                SqlConnection::Lock lock(connection);
                state->trace->status.store(NativeSaveStatus::CheckingReceipt);
                state->trace->attempts.fetch_add(1);
                return ExecuteRetainedNativeSave(
                    [&] {
                        const auto receipt = ReadReceipt(connection, state->query, state->task, state->revision);
                        if (receipt == NativeReceipt::Present) state->trace->status.store(NativeSaveStatus::AlreadyCommitted);
                        else if (receipt == NativeReceipt::Unavailable) state->trace->status.store(NativeSaveStatus::ReceiptUnavailable);
                        else if (receipt == NativeReceipt::Conflict) state->trace->status.store(NativeSaveStatus::ReceiptConflict);
                        return receipt;
                    },
                    [&] {
                        state->trace->status.store(NativeSaveStatus::Saving);
                        const bool committed = state->transaction->Execute(connection);
                        const auto status = state->trace->status.load();
                        if (committed) state->trace->status.store(NativeSaveStatus::Committed);
                        else if (status == NativeSaveStatus::ReadyToCommit) state->trace->status.store(NativeSaveStatus::CommitUncertain);
                        else if (status == NativeSaveStatus::Saving) state->trace->status.store(NativeSaveStatus::TransactionFailed);
                        return committed;
                    });
            }
        private:
            std::shared_ptr<State> state;
        };
    };
    std::shared_ptr<NativeSaveBatch> NativeSaveBatch::Capture(Database& database, const WritePlan& journal,
        const std::string& nativeProofQuery) {
        if (!database.HasOpenTransaction() || !IsUuid(journal.task) || !journal.revision ||
            journal.statements.empty() || journal.statements.size() > 64 ||
            journal.receiptQuery.empty() || journal.receiptQuery.size() > 30000 ||
            nativeProofQuery.empty() || nativeProofQuery.size() > 30000)
            throw std::invalid_argument("invalid_native_save_journal");
        for (const auto& sql : journal.statements)
            if (sql.empty() || sql.size() >= MAX_QUERY_LEN) throw std::invalid_argument("invalid_native_save_statement");
        auto state = std::make_shared<State>();
        state->task = journal.task; state->revision = journal.revision;
        state->query = journal.receiptQuery + " UNION ALL SELECT '',0";
        auto assertion = std::make_unique<RequireReceipt>(state->query, state->task, state->revision, state->trace, false);
        auto nativeAssertion = std::make_unique<RequireReceipt>(nativeProofQuery + " UNION ALL SELECT '',0",
            state->task, state->revision, state->trace, true);
        for (const auto& sql : journal.statements)
            if (!database.Execute(sql.c_str())) throw std::runtime_error("native_save_journal_queue_failed");
        state->transaction = database.TakeTransaction();
        if (!state->transaction) throw std::runtime_error("native_save_transaction_missing");
        state->transaction->DelayExecute(nativeAssertion.release());
        state->transaction->DelayExecute(assertion.release());
        return std::shared_ptr<NativeSaveBatch>(new NativeSaveBatch(std::move(state)));
    }
    bool NativeSaveBatch::Queue(Database& database) {
        bool expected = false;
        if (!state->queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;
        std::unique_ptr<SqlOperation> attempt;
        try {
            attempt = std::make_unique<State::Attempt>(state);
            state->trace->status.store(NativeSaveStatus::Queued);
            if (database.QueueOperation(attempt)) return true;
        } catch (...) {
            if (attempt) attempt.reset();
            else state->queued.store(false, std::memory_order_release);
            throw;
        }
        // A rejected queue retains attempt; its destructor releases the flag
        // exactly once. Do not clear it twice across another producer's CAS.
        state->trace->status.store(NativeSaveStatus::QueueUnavailable);
        return false;
    }
    bool NativeSaveBatch::InFlight() const { return state->queued.load(std::memory_order_acquire); }
    NativeSaveStatus NativeSaveBatch::Status() const { return state->trace->status.load(); }
    unsigned NativeSaveBatch::Attempts() const { return state->trace->attempts.load(); }
#ifdef LIVING_ISOLATED_NATIVE_TESTS
    void NativeSaveBatch::InjectIsolatedProofFailures() {
        const auto* environment = std::getenv("LIVING_WOW_TEST_ENVIRONMENT");
        std::ifstream marker("/isolated/ENVIRONMENT"); std::string value; std::getline(marker,value);
        if (!environment || std::string(environment) != "isolated-migration" || value != "isolated-migration-v1" ||
            InFlight() || Attempts())
            throw std::runtime_error("isolated_native_save_fault_not_allowed");
        state->trace->failNativeProof.store(true); state->trace->failJournalProof.store(true);
    }
#endif
}
