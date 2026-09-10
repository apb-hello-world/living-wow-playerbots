// Actual journal SQL, exercised against a separate MariaDB fixture database.
// Not linked into the game. No test controls enter the production executable.
#include "LivingActivity.h"
#include "LivingActivityCodec.h"
#include <mysql.h>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <boost/property_tree/json_parser.hpp>

using namespace LivingActivity;
static const std::string Id = "637bd562-36d2-5b01-bc01-e2d831c49f38";
static const std::string Receipt = "ff2efbdf-f0ec-4539-b840-299847970c00";
static const std::string Receipt2 = "ff2efbdf-f0ec-4539-b840-299847970c01";
class Connection {
public:
    MYSQL* db = mysql_init(nullptr);
    Connection() {
        const char* marker = std::getenv("LIVING_WOW_TEST_ENVIRONMENT");
        const char* database = std::getenv("LIVING_ACTIVITY_TEST_DATABASE");
        if (!marker || std::string(marker) != "isolated-migration" || !database ||
            std::string(database).find("migration_activity_test_") != 0)
            throw std::runtime_error("Dedicated isolated fixture database required");
        const char* secret = std::getenv("LIVING_ACTIVITY_TEST_PASSWORD_FILE");
        if (!secret) throw std::runtime_error("Private test credential path required");
        std::ifstream input(secret); std::string password; std::getline(input, password);
        if (!input || password.empty()) throw std::runtime_error("Test credential unavailable");
        if (!mysql_real_connect(db, "database", "root", password.c_str(), database, 3306, nullptr, 0))
            throw std::runtime_error(mysql_error(db));
        if (Scalar("SELECT marker FROM isolated_test_identity LIMIT 1") != "living-activity-journal-v1")
            throw std::runtime_error("Database fixture identity mismatch");
    }
    ~Connection() { mysql_close(db); }
    bool Execute(const std::string& sql) { return mysql_query(db, sql.c_str()) == 0; }
    std::string Scalar(const std::string& sql) {
        if (!Execute(sql)) throw std::runtime_error(mysql_error(db));
        MYSQL_RES* result = mysql_store_result(db);
        if (!result) return "";
        auto row = mysql_fetch_row(result);
        std::string value = row && row[0] ? row[0] : "";
        mysql_free_result(result); return value;
    }
    bool ReceiptPresent(const WritePlan& plan) {
        if (!Execute(plan.receiptQuery)) return false;
        MYSQL_RES* result = mysql_store_result(db);
        if (!result) return false;
        auto row = mysql_fetch_row(result);
        bool present = row && row[0] && row[1] && ReceiptMatches(plan, row[0], std::stoull(row[1]));
        mysql_free_result(result); return present;
    }
    bool Write(const WritePlan& plan, bool injectFailure = false) {
        assert(Execute("START TRANSACTION"));
        bool success = true;
        for (const auto& statement : plan.statements) success = Execute(statement) && success;
        if (injectFailure) success = Execute("INSERT INTO definitely_missing_fixture_table VALUES (1)") && success;
        assert(Execute(success ? "COMMIT" : "ROLLBACK"));
        // Like the native adapter, the caller verifies a receipt even if a
        // transaction wrapper would have returned true after an SQL failure.
        return ReceiptPresent(plan);
    }
};
int main() {
    Connection db;
    Task task; task.id = task.root = Id; task.actor = task.context.actor = 497;
    task.source = "economy_goal"; task.sourceKey = "42";
    task.createdAtMs = task.updatedAtMs = 1000;
    task.checkpoint.data = R"({"recipe":2881,"untrusted":"quote' slash\\"})";
    auto create = TaskWrite(task, 0, Receipt, "legacy_observed");
    assert(!db.Write(create, true));
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_task") == "0");
    assert(db.Write(create)); assert(db.Write(create));
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_task") == "1");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition") == "1");
    auto changed = task; changed.checkpoint.data = "{}";
    assert(!db.Write(TaskWrite(changed, 0, Receipt, "legacy_observed"))); // Same ID, different request.
    assert(db.Scalar("SELECT checkpoint FROM living_activity_task") == task.checkpoint.data);
    // Regression from the copied-realm restart: JSON_VALID TEXT nests as an
    // object in MariaDB unless the actual runtime projection converts to text.
    const auto payload = db.Scalar("SELECT " + PersistedTaskProjection() + " FROM living_activity_task");
    boost::property_tree::ptree envelope; std::istringstream input(payload);
    boost::property_tree::read_json(input, envelope);
    assert(envelope.get<std::string>("checkpoint") == task.checkpoint.data);
    assert(envelope.get<uint64_t>("revision") == task.revision);
    assert(envelope.get<uint32_t>("actor") == task.actor);
    Task decoded; std::string decodeError;
    assert(DecodeTaskProjection(payload, decoded, decodeError));
    assert(decoded.id == task.id && decoded.context.actor == task.actor && decoded.checkpoint.data == task.checkpoint.data);
    assert(db.Execute("UPDATE living_activity_task SET checkpoint_version=99"));
    assert(!DecodeTaskProjection(db.Scalar("SELECT " + PersistedTaskProjection() + " FROM living_activity_task"), decoded, decodeError));
    assert(decodeError == "unsupported_checkpoint");
    assert(db.Execute("UPDATE living_activity_task SET checkpoint_version=1"));
    auto recovered = AfterRestart(task, 600000);
    auto update = TaskWrite(recovered, task.revision, Receipt2, "restart_revalidation");
    assert(!db.Write(update, true));
    assert(db.Scalar("SELECT revision FROM living_activity_task") == "1");
    assert(db.Write(update)); assert(db.Write(update)); // Lost acknowledgement, safe retry.
    auto stale = task; ++stale.revision; stale.checkpoint.blocker = "stale_proposal";
    assert(!db.Write(TaskWrite(stale, 1, "ff2efbdf-f0ec-4539-b840-299847970c02", "stale")));
    assert(db.Scalar("SELECT revision FROM living_activity_task") == "2");
    assert(db.Scalar("SELECT active_elapsed_ms FROM living_activity_task") == "0");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_transition") == "2");
    // Reconnect means no reliance on an in-memory acknowledgement cache.
    { Connection restarted; assert(restarted.ReceiptPresent(update)); }
    assert(!db.Execute("UPDATE living_activity_task SET phase='completed'"));
    assert(!db.Execute("UPDATE living_activity_task SET checkpoint='not JSON'"));
    // Fixture metadata only: no game items/money are created by this test.
    auto prepared = recovered; prepared.mode = Mode::Active; prepared.phase = Phase::Preparing; ++prepared.revision;
    assert(db.Write(TaskWrite(prepared, recovered.revision, "ff2efbdf-f0ec-4539-b840-299847970c03", "fixture_activation")));
    auto executing = prepared; executing.phase = Phase::Executing; ++executing.revision;
    const std::string operation = "ff2efbdf-f0ec-4539-b840-299847970c04";
    auto intent = OperationIntentWrite(executing, prepared.revision, operation, "vendor_purchase", "{\"copper\":100}");
    assert(!db.Write(intent, true));
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_operation") == "0");
    assert(db.Write(intent)); assert(db.Write(intent));
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_operation") == "1");
    assert(db.Execute("UPDATE living_activity_operation SET kind='mismatched_fixture'"));
    assert(!db.ReceiptPresent(intent)); // A task receipt without its exact operation is insufficient.
    assert(db.Execute("UPDATE living_activity_operation SET kind='vendor_purchase'"));
    assert(db.ReceiptPresent(intent));
    auto bypass = executing; ++bypass.revision; bypass.phase = Phase::Preparing;
    assert(!db.Write(TaskWrite(bypass, executing.revision, "ff2efbdf-f0ec-4539-b840-299847970c11", "task_admitted")));
    assert(db.Scalar("SELECT phase FROM living_activity_task") == "executing");
    // A finite sibling cannot acquire a new route around an unresolved paid step.
    auto sibling = prepared; sibling.id = "637bd562-36d2-5b01-bc01-e2d831c49f40";
    sibling.parent = sibling.root = task.id; sibling.sourceKey = "42:bank_child";
    sibling.phase = Phase::Queued; sibling.revision = 1;
    assert(db.Write(TaskWrite(sibling, 0, "ff2efbdf-f0ec-4539-b840-299847970c12", "fixture_child")));
    auto blockedChild = sibling; ++blockedChild.revision; blockedChild.phase = Phase::Preparing;
    assert(!db.Write(TaskWrite(blockedChild, 1, "ff2efbdf-f0ec-4539-b840-299847970c13", "task_admitted")));
    assert(!db.Write(OperationIntentWrite(executing, prepared.revision, operation, "vendor_purchase", "{\"copper\":999}")));
    auto uncertainTask = executing; ++uncertainTask.revision; uncertainTask.phase = Phase::Reconciling;
    uncertainTask.checkpoint.blocker = "fixture_receipt_not_available";
    OperationResult uncertain; uncertain.id = operation; uncertain.task = task.id; uncertain.taskRevision = executing.revision;
    uncertain.kind = "vendor_purchase"; uncertain.state = OperationState::Reconciling;
    uncertain.nativeReference = "fixture-mail:81"; uncertain.evidence = "fixture_receipt_not_available";
    const std::string uncertainState = "{\"mail\":81,\"known_quantity\":1}";
    auto uncertainty = OperationOutcomeWrite(uncertainTask, executing.revision, uncertain,
        "ff2efbdf-f0ec-4539-b840-299847970c16", uncertainState);
    assert(!db.Write(uncertainty, true)); assert(db.Write(uncertainty)); assert(db.Write(uncertainty));
    assert(db.Scalar("SELECT native_reference FROM living_activity_operation") == "fixture-mail:81");
    assert(db.Scalar("SELECT after_state FROM living_activity_operation") == uncertainState);
    // A reused receipt with changed contents must not edit an uncertain row,
    // even if the final acknowledgement correctly rejects the changed hash.
    auto rewritten = uncertain;
    rewritten.nativeReference = "fixture-mail:999";
    rewritten.evidence = "fixture_changed_receipt";
    assert(!db.Write(OperationOutcomeWrite(uncertainTask, executing.revision, rewritten,
        "ff2efbdf-f0ec-4539-b840-299847970c16", "{\"mail\":999}")));
    assert(db.Scalar("SELECT native_reference FROM living_activity_operation") == "fixture-mail:81");
    assert(db.Scalar("SELECT after_state FROM living_activity_operation") == uncertainState);
    assert(db.Scalar("SELECT evidence_code FROM living_activity_operation") == uncertain.evidence);
    assert(db.ReceiptPresent(uncertainty));
    auto forged = uncertainTask; forged.phase = Phase::Verifying;
    rewritten.state = OperationState::Verified;
    assert(!db.Write(OperationOutcomeWrite(forged, executing.revision, rewritten,
        "ff2efbdf-f0ec-4539-b840-299847970c16", "{\"mail\":999}")));
    assert(db.Scalar("SELECT state FROM living_activity_operation") == "reconciling");
    assert(db.Scalar("SELECT native_reference FROM living_activity_operation") == "fixture-mail:81");
    assert(db.Scalar("SELECT after_state FROM living_activity_operation") == uncertainState);
    assert(db.ReceiptPresent(uncertainty));
    // The acknowledgement covers the observed evidence, not just its label.
    assert(db.Execute("UPDATE living_activity_operation SET after_state='{}'"));
    assert(!db.ReceiptPresent(uncertainty));
    assert(db.Execute("UPDATE living_activity_operation SET after_state=" + SqlValue(uncertainState)));
    assert(db.ReceiptPresent(uncertainty));
    auto restartedTask = AfterRestart(uncertainTask, 700000);
    assert(db.Write(TaskWrite(restartedTask, uncertainTask.revision, "ff2efbdf-f0ec-4539-b840-299847970c05", "restart_revalidation")));
    assert(db.Scalar("SELECT state FROM living_activity_operation") == "reconciling");
    assert(db.Scalar("SELECT native_reference FROM living_activity_operation") == "fixture-mail:81");
    assert(db.Scalar("SELECT after_state FROM living_activity_operation") == uncertainState);
    bypass = restartedTask; ++bypass.revision; bypass.phase = Phase::Preparing;
    assert(!db.Write(TaskWrite(bypass, restartedTask.revision, "ff2efbdf-f0ec-4539-b840-299847970c14", "task_admitted")));
    assert(db.Write(intent)); // Original receipt exists, but it is NOT permission to execute again.
    assert(db.Scalar("SELECT state FROM living_activity_operation") == "reconciling");
    auto finished = restartedTask; finished.phase = Phase::Verifying; ++finished.revision;
    OperationResult proof; proof.id = operation; proof.task = task.id; proof.taskRevision = executing.revision;
    proof.kind = "vendor_purchase"; proof.state = OperationState::Verified;
    proof.nativeReference = "fixture-receipt:42"; proof.evidence = "fixture_native_effect";
    auto outcome = OperationOutcomeWrite(finished, restartedTask.revision, proof,
        "ff2efbdf-f0ec-4539-b840-299847970c06", "{\"copper\":90}");
    assert(!db.Write(outcome, true)); assert(db.Write(outcome)); assert(db.Write(outcome));
    assert(db.Scalar("SELECT state FROM living_activity_operation") == "verified");
    auto completed = finished; completed.phase = Phase::Completed; ++completed.revision;
    assert(!db.Write(TaskWrite(completed, finished.revision, "ff2efbdf-f0ec-4539-b840-299847970c07", "fixture_completed")));
    sibling.phase = Phase::Cancelled; ++sibling.revision;
    assert(db.Write(TaskWrite(sibling, 1, "ff2efbdf-f0ec-4539-b840-299847970c15", "fixture_child_cancelled")));
    assert(db.Write(TaskWrite(completed, finished.revision, "ff2efbdf-f0ec-4539-b840-299847970c07", "fixture_completed")));
    assert(db.Scalar("SELECT phase FROM living_activity_task WHERE task_id=" + SqlValue(Id)) == "completed");
    auto mistaken = task;
    mistaken.id = mistaken.root = "637bd562-36d2-5b01-bc01-e2d831c49f39";
    mistaken.sourceKey = "99"; mistaken.kind = Kind::Profession;
    assert(db.Write(TaskWrite(mistaken, 0, "ff2efbdf-f0ec-4539-b840-299847970c08", "legacy_observed")));
    auto corrected = AfterRestart(mistaken, 800000); corrected.kind = Kind::Progression;
    assert(!db.Write(TaskWrite(corrected, 1, "ff2efbdf-f0ec-4539-b840-299847970c09", "restart_revalidation")));
    assert(db.Write(TaskWrite(corrected, 1, "ff2efbdf-f0ec-4539-b840-299847970c09", "observation_reclassified")));
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_task WHERE kind='progression'") == "1");
    // A prior successful step cannot prove a later rejected operation completed.
    Task multi = prepared; multi.id = multi.root = "637bd562-36d2-5b01-bc01-e2d831c49f41";
    multi.sourceKey = "multi_step"; multi.phase = Phase::Queued; multi.revision = 1;
    assert(db.Write(TaskWrite(multi, 0, "ff2efbdf-f0ec-4539-b840-299847970c20", "fixture_created")));
    multi.phase = Phase::Preparing; ++multi.revision;
    assert(db.Write(TaskWrite(multi, 1, "ff2efbdf-f0ec-4539-b840-299847970c21", "fixture_prepared")));
    multi.phase = Phase::Executing; ++multi.revision;
    const std::string firstOp = "ff2efbdf-f0ec-4539-b840-299847970c22";
    assert(db.Write(OperationIntentWrite(multi, 2, firstOp, "fixture_step", "{}")));
    // Another root for the same actor cannot start a second operation either.
    Task competing = multi; competing.id = competing.root = "637bd562-36d2-5b01-bc01-e2d831c49f42";
    competing.sourceKey = "competing_step"; competing.phase = Phase::Queued; competing.revision = 1;
    assert(db.Write(TaskWrite(competing, 0, "ff2efbdf-f0ec-4539-b840-299847970c23", "fixture_created")));
    competing.phase = Phase::Preparing; ++competing.revision;
    assert(db.Write(TaskWrite(competing, 1, "ff2efbdf-f0ec-4539-b840-299847970c24", "fixture_prepared")));
    competing.phase = Phase::Executing; ++competing.revision;
    assert(!db.Write(OperationIntentWrite(competing, 2, "ff2efbdf-f0ec-4539-b840-299847970c25", "fixture_step", "{}")));
    proof.id = firstOp; proof.task = multi.id; proof.taskRevision = 3; proof.kind = "fixture_step";
    multi.phase = Phase::Verifying; ++multi.revision;
    assert(db.Write(OperationOutcomeWrite(multi, 3, proof, "ff2efbdf-f0ec-4539-b840-299847970c26", "{}")));
    multi.phase = Phase::Preparing; ++multi.revision;
    assert(db.Write(TaskWrite(multi, 4, "ff2efbdf-f0ec-4539-b840-299847970c27", "fixture_next_step")));
    multi.phase = Phase::Executing; ++multi.revision;
    proof.id = "ff2efbdf-f0ec-4539-b840-299847970c28"; proof.taskRevision = 6;
    assert(db.Write(OperationIntentWrite(multi, 5, proof.id, "fixture_step", "{}")));
    proof.state = OperationState::Rejected; proof.nativeReference.clear(); proof.evidence = "fixture_native_rejected";
    multi.phase = Phase::Verifying; ++multi.revision;
    assert(db.Write(OperationOutcomeWrite(multi, 6, proof, "ff2efbdf-f0ec-4539-b840-299847970c29", "{}")));
    multi.phase = Phase::Completed; ++multi.revision;
    assert(!db.Write(TaskWrite(multi, 7, "ff2efbdf-f0ec-4539-b840-299847970c30", "fixture_completed")));
    std::cout << "PASS: real MariaDB atomic task/outbox and intent/outcome journals, duplicate/stale requests, transaction rollback, uncertain restart, immutable receipts, shadow completion guard (fixture metadata, NOT native gameplay proof)\n";
}
