// Actual journal SQL, exercised against a separate MariaDB fixture database.
// Not linked into the game. No test controls enter the production executable.
#include "LivingActivity.h"
#include "LivingActivityCodec.h"
#include "LivingActivityResources.h"
#include "LivingActivityClaimConsumption.h"
#include "LivingActivityOperations.h"
#include "LivingActivityTransfer.h"
#include "LivingActivityClaimCodec.h"
#include "LivingActivityReceipts.h"
#include "LivingPurchaseBudget.h"
#include "LivingProfessionEvidence.h"
#include "LivingProfessionSettlement.h"
#include "LivingProfessionEconomy.h"
#include "fixtures/CraftEvidence.h"
#include <mysql.h>
#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <boost/property_tree/json_parser.hpp>
#include <atomic>
#include <thread>

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
    PurchaseSpend Budget(uint32_t actor,uint64_t now,const std::string& operation="") {
        if (!Execute(PurchaseSpendQuery(actor,now,operation))) throw std::runtime_error(mysql_error(db));
        MYSQL_RES* result=mysql_store_result(db); assert(result && mysql_num_fields(result)==4);
        const auto row=mysql_fetch_row(result); assert(row);
        PurchaseSpend spend;
        DecodePurchaseSpend({row[0] ? row[0] : "",row[1] ? row[1] : "",row[2] ? row[2] : "",row[3] ? row[3] : ""},spend);
        mysql_free_result(result); return spend;
    }
    std::vector<ProfessionHistoryRow> History(const Task& task) {
        if (!Execute(ProfessionHistoryQuery(task))) throw std::runtime_error(mysql_error(db));
        MYSQL_RES* result=mysql_store_result(db);assert(result && mysql_num_fields(result)==12);
        std::vector<ProfessionHistoryRow> rows;
        while (const auto row=mysql_fetch_row(result)) {
            ProfessionHistoryRow fields;for (size_t i=0;i<fields.size();++i) fields[i]=row[i] ? row[i] : "";
            rows.push_back(std::move(fields));
        }
        mysql_free_result(result);return rows;
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
#include "fixtures/ProfessionSettlementDatabase.inc"
#include "fixtures/ProfessionResumeDatabase.inc"
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
    // Reservation metadata only. These snapshots do NOT grant items or money.
    Task reserve = prepared; reserve.id = reserve.root = "637bd562-36d2-5b01-bc01-e2d831c49f43";
    reserve.actor = reserve.context.actor = 610; reserve.sourceKey = "reservation_a";
    reserve.phase = Phase::Queued; reserve.revision = 1;
    assert(db.Write(TaskWrite(reserve,0,"ff2efbdf-f0ec-4539-b840-299847970d00","fixture_created")));
    Task competitor = reserve; competitor.id = competitor.root = "637bd562-36d2-5b01-bc01-e2d831c49f44";
    competitor.sourceKey = "reservation_b";
    assert(db.Write(TaskWrite(competitor,0,"ff2efbdf-f0ec-4539-b840-299847970d01","fixture_created")));
    ResourceClaim claim; claim.id = "ff2efbdf-f0ec-4539-b840-299847970d02";
    claim.task = reserve.id; claim.actor = reserve.actor; claim.itemGuid = 900;
    claim.itemEntry = 2934; claim.quantity = 6; claim.location = "bags"; claim.state = "held";
    NativeResourceBalance stock{610,900,2934,10,0,"bags"};
    reserve.phase = Phase::Preparing; ++reserve.revision;
    const auto reservation = ResourceReservationWrite(reserve,1,"ff2efbdf-f0ec-4539-b840-299847970d03",{{claim,0}},{stock});
    assert(!db.Write(reservation,true));
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim") == "0");
    assert(db.Write(reservation)); assert(db.Write(reservation));
    ResourceClaim loadedClaim; std::string claimError;
    assert(DecodeClaimProjection(db.Scalar("SELECT " + PersistedClaimProjection() + " FROM living_activity_claim c WHERE c.claim_id=" +
        SqlValue(claim.id)),loadedClaim,claimError));
    assert(SameResourceClaim(loadedClaim,claim));
    ResourceClaimBook restoredClaims;
    assert(restoredClaims.RestoreBatch({loadedClaim}) == ClaimInstall::Installed);
    assert(restoredClaims.FinishRestore());
    assert(restoredClaims.Protection().UnreservedItem(610,900,2934,10) == 4);
    assert(db.Scalar("SELECT quantity FROM living_activity_claim WHERE claim_id=" + SqlValue(claim.id)) == "6");
    auto tamperedClaim = claim; tamperedClaim.quantity = 8;
    assert(!db.Write(ResourceReservationWrite(reserve,1,"ff2efbdf-f0ec-4539-b840-299847970d03",{{tamperedClaim,0}},{stock})));
    assert(db.Scalar("SELECT quantity FROM living_activity_claim WHERE claim_id=" + SqlValue(claim.id)) == "6");
    auto extraClaim = claim; extraClaim.id = "ff2efbdf-f0ec-4539-b840-299847970d04"; extraClaim.quantity = 2;
    assert(!db.Write(ResourceReservationWrite(reserve,1,"ff2efbdf-f0ec-4539-b840-299847970d03",{{claim,0},{extraClaim,0}},{stock})));
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim") == "1");
    auto competingClaim = claim; competingClaim.id = "ff2efbdf-f0ec-4539-b840-299847970d05";
    competingClaim.task = competitor.id; competingClaim.quantity = 5;
    competitor.phase = Phase::Preparing; ++competitor.revision;
    assert(!db.Write(ResourceReservationWrite(competitor,1,"ff2efbdf-f0ec-4539-b840-299847970d06",{{competingClaim,0}},{stock})));
    assert(db.Scalar("SELECT revision FROM living_activity_task WHERE task_id=" + SqlValue(competitor.id)) == "1");
    competingClaim.quantity = 4;
    assert(db.Write(ResourceReservationWrite(competitor,1,"ff2efbdf-f0ec-4539-b840-299847970d07",{{competingClaim,0}},{stock})));
    assert(db.Scalar("SELECT SUM(quantity) FROM living_activity_claim WHERE state='held'") == "10");
    ResourceClaim cash = claim; cash.id = "ff2efbdf-f0ec-4539-b840-299847970d08";
    cash.itemGuid = cash.itemEntry = 0; cash.quantity = 0; cash.copper = 600; cash.location = "money";
    NativeResourceBalance wallet{610,0,0,0,1000,"money"};
    ++reserve.revision;
    assert(db.Write(ResourceReservationWrite(reserve,2,"ff2efbdf-f0ec-4539-b840-299847970d09",{{cash,0}},{wallet})));
    auto otherCash = cash; otherCash.id = "ff2efbdf-f0ec-4539-b840-299847970d10";
    otherCash.task = competitor.id; otherCash.copper = 500;
    ++competitor.revision;
    assert(!db.Write(ResourceReservationWrite(competitor,2,"ff2efbdf-f0ec-4539-b840-299847970d11",{{otherCash,0}},{wallet})));
    otherCash.copper = 400;
    assert(db.Write(ResourceReservationWrite(competitor,2,"ff2efbdf-f0ec-4539-b840-299847970d12",{{otherCash,0}},{wallet})));
    auto releasedClaim = claim; ++releasedClaim.revision; releasedClaim.state = "released";
    auto releasedCash = cash; ++releasedCash.revision; releasedCash.state = "released";
    auto cancelledReserve = reserve; ++cancelledReserve.revision; cancelledReserve.phase = Phase::Cancelled;
    assert(!db.Write(TaskWrite(cancelledReserve,3,"ff2efbdf-f0ec-4539-b840-299847970d13","fixture_cancelled")));
    ++reserve.revision;
    const auto release = ResourceReservationWrite(reserve,3,"ff2efbdf-f0ec-4539-b840-299847970d14",{{releasedClaim,1},{releasedCash,1}},{});
    assert(!db.Write(release,true));
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id=" + SqlValue(claim.id)) == "held");
    assert(db.Write(release)); assert(db.Write(release));
    assert(db.Scalar("SELECT SUM(copper) FROM living_activity_claim WHERE state='held'") == "400");
    assert(db.Scalar("SELECT SUM(quantity) FROM living_activity_claim WHERE state='held'") == "4");
    cancelledReserve = reserve; ++cancelledReserve.revision; cancelledReserve.phase = Phase::Cancelled;
    assert(db.Write(TaskWrite(cancelledReserve,4,"ff2efbdf-f0ec-4539-b840-299847970d15","fixture_cancelled")));
    assert(db.ReceiptPresent(release)); // Native quantities are untouched by a release.
    bool transferRejected = false;
    auto illicit = competingClaim; ++illicit.revision; illicit.state = "in_transfer"; illicit.location = "mail"; illicit.nativeReference = 120;
    ++competitor.revision;
    try { ResourceReservationWrite(competitor,3,"ff2efbdf-f0ec-4539-b840-299847970d16",{{illicit,1}},{}); }
    catch (const std::invalid_argument&) { transferRejected = true; }
    assert(transferRejected);
    // Fail closed if native stock shrank before a new reservation. Never
    // compensate by rewriting the already-held quantities of another job.
    stock.quantity = 3; auto newClaim = competingClaim; newClaim.id = extraClaim.id; newClaim.quantity = 1;
    assert(!db.Write(ResourceReservationWrite(competitor,3,"ff2efbdf-f0ec-4539-b840-299847970d17",{{newClaim,0}},{stock})));
    assert(db.Scalar("SELECT quantity FROM living_activity_claim WHERE claim_id=" + SqlValue(competingClaim.id)) == "4");
    // Two concurrent root jobs for one actor cannot each reserve six out of
    // the same ten items. The common actor row lock precedes the stock check.
    Task raceA = reserve; raceA.id = raceA.root = "637bd562-36d2-5b01-bc01-e2d831c49f45";
    raceA.actor = raceA.context.actor = 611; raceA.sourceKey = "reservation_race_a";
    raceA.phase = Phase::Queued; raceA.revision = 1;
    Task raceB = raceA; raceB.id = raceB.root = "637bd562-36d2-5b01-bc01-e2d831c49f46"; raceB.sourceKey = "reservation_race_b";
    assert(db.Write(TaskWrite(raceA,0,"ff2efbdf-f0ec-4539-b840-299847970d20","fixture_created")));
    assert(db.Write(TaskWrite(raceB,0,"ff2efbdf-f0ec-4539-b840-299847970d21","fixture_created")));
    raceA.phase = raceB.phase = Phase::Preparing; ++raceA.revision; ++raceB.revision;
    auto raceClaimA = claim; raceClaimA.id = "ff2efbdf-f0ec-4539-b840-299847970d22";
    raceClaimA.actor = 611; raceClaimA.task = raceA.id; raceClaimA.itemGuid = 901;
    auto raceClaimB = raceClaimA; raceClaimB.id = "ff2efbdf-f0ec-4539-b840-299847970d23"; raceClaimB.task = raceB.id;
    NativeResourceBalance raceStock{611,901,2934,10,0,"bags"};
    const auto racePlanA = ResourceReservationWrite(raceA,1,"ff2efbdf-f0ec-4539-b840-299847970d24",{{raceClaimA,0}},{raceStock});
    const auto racePlanB = ResourceReservationWrite(raceB,1,"ff2efbdf-f0ec-4539-b840-299847970d25",{{raceClaimB,0}},{raceStock});
    std::atomic<unsigned> ready{0}, wins{0}; std::atomic<bool> go{false};
    auto compete = [&](const WritePlan& plan) {
        Connection connection; ++ready;
        while (!go.load()) std::this_thread::yield();
        if (connection.Write(plan)) ++wins;
    };
    std::thread first(compete,std::cref(racePlanA)), second(compete,std::cref(racePlanB));
    while (ready.load() != 2) std::this_thread::yield();
    go.store(true); first.join(); second.join();
    assert(wins.load() == 1);
    assert(db.Scalar("SELECT SUM(quantity) FROM living_activity_claim WHERE item_guid=901 AND state='held'") == "6");
    // A stale CAS in a COMMITTED batch does not invalidate another actor's
    // exact receipt. Exercise the same settlement helper used by the realm.
    auto bad = TaskWrite(changed,0,Receipt,"legacy_observed");
    Task independent = raceA; independent.actor = independent.context.actor = 612;
    independent.id = independent.root = "637bd562-36d2-5b01-bc01-e2d831c49f47";
    independent.sourceKey = "receipt_isolation"; independent.phase = Phase::Queued; independent.revision = 1;
    auto good = TaskWrite(independent,0,"ff2efbdf-f0ec-4539-b840-299847970d26","fixture_created");
    assert(db.Execute("START TRANSACTION"));
    for (const auto& write : {bad,good}) for (const auto& sql : write.statements) assert(db.Execute(sql));
    assert(db.Execute("COMMIT"));
    assert(!db.ReceiptPresent(bad) && db.ReceiptPresent(good));
    struct PendingWrite { WritePlan plan; ReceiptRetry retry; std::string afterState; };
    std::deque<PendingWrite> batch{{bad,{},"retained_uncertain_native_result"},{good,{},"{}"}};
    ReceiptSet receipts;
    for (const auto& write : batch) if (db.ReceiptPresent(write.plan)) receipts.emplace(write.plan.task,write.plan.revision);
    unsigned saves = 0;
    assert(SettleReceiptBatch(batch,2,true,receipts,1000,[&](const PendingWrite& write){
        assert(write.plan.task == independent.id); ++saves;
    }) == 1);
    assert(saves == 1 && batch.size() == 1 && batch.front().afterState == "retained_uncertain_native_result");
    assert(PrepareReceiptBatch(batch,32,5999) == 0 && PrepareReceiptBatch(batch,32,6000) == 1);
    assert(!db.Write(bad) && db.ReceiptPresent(good));
    // Consumption is journal metadata proof, not a fabricated native purchase.
    // Only claims already named in the saved intent may settle with its result.
    Task consuming=independent; consuming.id=consuming.root="637bd562-36d2-5b01-bc01-e2d831c49f60";
    consuming.actor=consuming.context.actor=700; consuming.sourceKey="claimed_consumption";
    assert(db.Write(TaskWrite(consuming,0,"ff2efbdf-f0ec-4539-b840-299847970e10","fixture_created")));
    ResourceClaim spend=cash; spend.id="ff2efbdf-f0ec-4539-b840-299847970e01";
    spend.actor=700; spend.task=consuming.id; spend.copper=30;
    consuming.phase=Phase::Preparing; ++consuming.revision;
    assert(db.Write(ResourceReservationWrite(consuming,1,"ff2efbdf-f0ec-4539-b840-299847970e11",
        {{spend,0}},{{700,0,0,0,1000,"money"}})));
    consuming.phase=Phase::Executing; ++consuming.revision;
    const std::string consumeId="ff2efbdf-f0ec-4539-b840-299847970e12";
    OperationRequest spending; spending.transition.task=consuming; spending.transition.expectedRevision=2;
    spending.transition.receipt=consumeId; spending.effects=Mask(Effect::Money); spending.kind="fixture_spend";
    spending.beforeState="{\"money\":1000}"; spending.consumption={{spend,30}};
    assert(db.Write(OperationRequestWrite(spending)));
    OperationResult consumed; consumed.id=consumeId; consumed.task=consuming.id; consumed.taskRevision=3;
    consumed.kind="fixture_spend"; consumed.state=OperationState::Verified;
    consumed.nativeReference="fixture_metadata:spend"; consumed.evidence="fixture_only_result";
    consuming.phase=Phase::Verifying; ++consuming.revision;
    const std::string consumeReceipt="ff2efbdf-f0ec-4539-b840-299847970e13";
    const auto settled=ConsumedOperationWrite(consuming,3,consumed,consumeReceipt,"{\"money\":970}",{{spend,30}});
    const auto changedUse=ConsumedOperationWrite(consuming,3,consumed,consumeReceipt,"{\"money\":980}",{{spend,20}});
    assert(!db.Write(changedUse.journal)); // Does not match saved intent.
    assert(!db.Write(settled.journal,true)); // Inject rollback after the writes.
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(spend.id)) == "held");
    assert(db.Scalar("SELECT state FROM living_activity_operation WHERE operation_id="+SqlValue(consumeId)) == "intent");
    assert(db.Write(settled.journal)); assert(db.Write(settled.journal));
    assert(!db.Write(changedUse.journal)); // Cannot rewrite an acknowledged result either.
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(spend.id)) == "consumed");
    assert(db.Scalar("SELECT revision FROM living_activity_claim WHERE claim_id="+SqlValue(spend.id)) == "2");
    assert(db.Scalar("SELECT JSON_EXTRACT(after_state,'$.native.money') FROM living_activity_operation WHERE operation_id="+SqlValue(consumeId)) == "970");
    consuming.phase=Phase::Completed; ++consuming.revision;
    assert(db.Write(TaskWrite(consuming,4,"ff2efbdf-f0ec-4539-b840-299847970e14","fixture_completed")));
    // One purchase receipt consumes its input claim and acquires exact output
    // identities atomically. These are fixture journal rows, not game items.
    Task purchase=consuming; purchase.id=purchase.root="637bd562-36d2-5b01-bc01-e2d831c49f81";
    purchase.actor=purchase.context.actor=701; purchase.sourceKey="claimed_item_gain";
    purchase.phase=Phase::Queued; purchase.revision=1;
    assert(db.Write(TaskWrite(purchase,0,"ff2efbdf-f0ec-4539-b840-299847970f10","fixture_created")));
    auto purchaseMoney=spend; purchaseMoney.id="ff2efbdf-f0ec-4539-b840-299847970f11";
    purchaseMoney.actor=701; purchaseMoney.task=purchase.id;
    purchase.phase=Phase::Preparing; ++purchase.revision;
    assert(db.Write(ResourceReservationWrite(purchase,1,"ff2efbdf-f0ec-4539-b840-299847970f12",
        {{purchaseMoney,0}},{{701,0,0,0,100,"money"}})));
    purchase.phase=Phase::Executing; ++purchase.revision;
    const std::string purchaseId="ff2efbdf-f0ec-4539-b840-299847970f13";
    OperationRequest buying; buying.transition.task=purchase; buying.transition.expectedRevision=2;
    buying.transition.receipt=purchaseId; buying.effects=Mask(Effect::Money)|Mask(Effect::Inventory);
    buying.persistence=NativePersistence::Inventory; buying.kind="vendor_purchase";
    buying.beforeState="{\"money\":100}"; buying.consumption={{purchaseMoney,30}}; buying.itemGain={3371,3};
    assert(db.Write(OperationRequestWrite(buying)));
    OperationResult bought=consumed; bought.id=purchaseId; bought.task=purchase.id; bought.kind="vendor_purchase";
    bought.nativeReference="fixture_metadata:item_gain";
    purchase.phase=Phase::Verifying; ++purchase.revision;
    const std::string purchaseReceipt="ff2efbdf-f0ec-4539-b840-299847970f14";
    const std::vector<VerifiedItemGain> outputs={
        {{701,950,3371,19,0,23},{701,950,3371,20,0,23},1},
        {{701,951,3371,0,9510,0},{701,951,3371,2,9510,0},2}};
    const auto received=AcquiredOperationWrite(purchase,3,bought,purchaseReceipt,"{\"money\":70}",buying.consumption,buying.itemGain,outputs);
    auto changedOutputs=outputs; ++changedOutputs[1].after.count; ++changedOutputs[1].added;
    const auto wrongQuantity=AcquiredOperationWrite(purchase,3,bought,purchaseReceipt,"{\"money\":70}",buying.consumption,{3371,4},changedOutputs);
    assert(!db.Write(wrongQuantity.journal)); // Exact saved desired quantity, not retrospective intent.
    assert(!db.Write(received.journal,true));
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(purchaseMoney.id))=="held");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE item_guid IN (950,951)")=="0");
    assert(db.Scalar("SELECT state FROM living_activity_operation WHERE operation_id="+SqlValue(purchaseId))=="intent");
    assert(db.Write(received.journal)); assert(db.Write(received.journal));
    assert(!db.Write(wrongQuantity.journal));
    assert(db.Scalar("SELECT state FROM living_activity_claim WHERE claim_id="+SqlValue(purchaseMoney.id))=="consumed");
    assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE item_guid IN (950,951) AND state='held'")=="2");
    assert(db.Scalar("SELECT SUM(quantity) FROM living_activity_claim WHERE item_guid IN (950,951) AND state='held'")=="3");
    assert(db.Scalar("SELECT quantity FROM living_activity_claim WHERE claim_id="+SqlValue(ItemGainClaimId(purchaseId,950)))=="1");
    assert(db.Scalar("SELECT JSON_EXTRACT(after_state,'$.native.result.money') FROM living_activity_operation WHERE operation_id="+SqlValue(purchaseId))=="70");
    { Connection restarted; assert(restarted.ReceiptPresent(received.journal)); }
    // Native output ownership remains held until a later verified craft or
    // explicit release; task completion cannot silently discard those goods.
    ++purchase.revision; purchase.phase=Phase::Completed;
    assert(!db.Write(TaskWrite(purchase,4,"ff2efbdf-f0ec-4539-b840-299847970f15","fixture_completed")));
    // Use the actual common ledger query, not a parallel arithmetic fixture.
    // These auction rows exist ONLY in this marked dedicated test database.
    assert(db.Execute("CREATE TABLE organic_economy_auction_history (buyer_guid INT UNSIGNED,outcome VARCHAR(20),"
        "unit_price_copper INT UNSIGNED,quantity INT UNSIGNED,occurred_at TIMESTAMP)"));
    constexpr uint64_t budgetNow=172800000;
    assert(db.Execute("INSERT INTO organic_economy_auction_history VALUES (701,'sold',10,2,FROM_UNIXTIME(172000)),"
        "(701,'bid',5,2,FROM_UNIXTIME(170000)),(701,'posted',999,1,FROM_UNIXTIME(172000)),"
        "(700,'sold',999,1,FROM_UNIXTIME(172000)),(701,'sold',999,1,FROM_UNIXTIME(1000))"));
    assert(db.Execute("UPDATE living_activity_operation SET updated_at_ms=172000000 WHERE operation_id="+SqlValue(purchaseId)));
    auto budget=db.Budget(701,budgetNow);
    assert(budget.complete && budget.auctionCountHour==2 && budget.spentDay==60 && budget.committed==0);
    // A verified purchase may not be hidden by passing its operation ID again.
    assert(db.Budget(701,budgetNow,purchaseId).spentDay==60);
    assert(db.Execute("UPDATE living_activity_operation SET state='intent',updated_at_ms=1000 WHERE operation_id="+SqlValue(purchaseId)));
    budget=db.Budget(701,budgetNow);
    assert(budget.complete && budget.spentDay==30 && budget.committed==30); // Old unresolved intent never expires.
    assert(db.Budget(701,budgetNow,purchaseId).committed==0); // Current fresh intent, not a second charge.
    assert(db.Execute("UPDATE living_activity_operation SET state='reconciling' WHERE operation_id="+SqlValue(purchaseId)));
    assert(db.Budget(701,budgetNow,purchaseId).committed==30); // Uncertainty is never excluded.
    assert(db.Execute("START TRANSACTION"));
    assert(db.Execute("UPDATE living_activity_operation SET before_state='{}' WHERE operation_id="+SqlValue(purchaseId)));
    assert(!db.Budget(701,budgetNow).complete); // Malformed proof does not become zero spend.
    assert(db.Execute("ROLLBACK"));
    assert(db.Execute("UPDATE living_activity_operation SET state='verified',updated_at_ms=172000000 WHERE operation_id="+SqlValue(purchaseId)));
    { Connection restarted; assert(restarted.Budget(701,budgetNow).spentDay==60); }
    // Exercise the actual revision-bound history query. These rows deliberately
    // contain no real craft proof, so even a 'verified' label cannot pass the
    // same physical decoder used by the game.
    Task historyTask=consuming;historyTask.id=historyTask.root="637bd562-36d2-5b01-bc01-e2d831c49f91";
    historyTask.actor=historyTask.context.actor=702;historyTask.source="profession_job";
    historyTask.sourceKey="history_query_fixture";historyTask.kind=Kind::Profession;
    historyTask.phase=Phase::Queued;historyTask.revision=1;
    ProfessionJob historyJob;historyJob.recipe=2329;historyJob.skill=171;historyJob.initialSkill=1;historyJob.targetSkill=2;
    historyJob.outputEntry=2454;historyJob.outputQuantity=1;historyJob.attemptLimit=1;historyJob.reagents={{765,1},{2449,1},{3371,1}};
    historyTask.checkpoint.data=EncodeProfessionJob(historyJob);
    assert(db.Write(TaskWrite(historyTask,0,"ff2efbdf-f0ec-4539-b840-299847971010","fixture_created")));
    ProfessionHistoryCursor history;std::string historyBlocker;
    assert(history.Begin(historyTask,db.History(historyTask),historyBlocker));
    assert(history.Result().complete && !history.Result().unresolvedOperation && history.Result().attempts.empty());
    auto missing=historyTask;++missing.revision;
    assert(db.History(missing).empty());missing=historyTask;++missing.actor;
    assert(db.History(missing).empty());
    historyTask.phase=Phase::Preparing;++historyTask.revision;
    assert(db.Write(TaskWrite(historyTask,1,"ff2efbdf-f0ec-4539-b840-299847971011","fixture_preparing")));
    historyTask.phase=Phase::Executing;++historyTask.revision;
    const std::string historyOp="ff2efbdf-f0ec-4539-b840-299847971012";
    assert(db.Write(OperationIntentWrite(historyTask,2,historyOp,"profession_craft","{}")));
    assert(history.Begin(historyTask,db.History(historyTask),historyBlocker) && history.Advance(historyBlocker));
    assert(history.Result().complete && history.Result().unresolvedOperation && history.Result().attempts.empty());
    OperationResult fakeCraft;fakeCraft.id=historyOp;fakeCraft.task=historyTask.id;fakeCraft.taskRevision=3;
    fakeCraft.kind="profession_craft";fakeCraft.state=OperationState::Verified;
    fakeCraft.nativeReference="spell:2329:operation:"+historyOp;fakeCraft.evidence="native_craft_consumption_output_and_skill_observed";
    historyTask.phase=Phase::Verifying;++historyTask.revision;
    assert(db.Write(OperationOutcomeWrite(historyTask,3,fakeCraft,"ff2efbdf-f0ec-4539-b840-299847971013","{}")));
    assert(history.Begin(historyTask,db.History(historyTask),historyBlocker) && !history.Advance(historyBlocker));
    assert(!history.Result().complete && history.Result().attempts.empty());
    // Another accepted root's uncertain purchase blocks this actor, not just
    // operations with the same recipe or task ID. No physical item is edited.
    assert(db.Execute("START TRANSACTION"));
    assert(db.Execute("UPDATE living_activity_task SET actor_guid=702 WHERE task_id="+SqlValue(purchase.id)));
    assert(db.Execute("UPDATE living_activity_operation SET state='reconciling' WHERE operation_id="+SqlValue(purchaseId)));
    assert(db.History(historyTask).front()[2]=="1");
    assert(db.Execute("ROLLBACK"));
    assert(db.History(historyTask).front()[2]=="0");
    // Overflow returns limit+1, never a silently truncated successful history.
    assert(db.Execute("START TRANSACTION"));
    assert(db.Execute("INSERT INTO living_activity_operation SELECT 'ff2efbdf-f0ec-4539-b840-299847971014',"
        "task_id,task_revision+1,kind,request_hash,state,native_reference,before_state,after_state,evidence_code,created_at_ms,updated_at_ms "
        "FROM living_activity_operation WHERE operation_id="+SqlValue(historyOp)));
    assert(db.History(historyTask).size()==2);
    assert(!history.Begin(historyTask,db.History(historyTask),historyBlocker) && historyBlocker=="profession_history_attempt_limit_exceeded");
    assert(db.Execute("ROLLBACK"));
    {Connection restarted;assert(restarted.History(historyTask)==db.History(historyTask));}
    {
        // Transfer journal metadata, not game-item fixtures. A native adapter
        // must separately prove the physical bank -> bags move in its save.
        const auto rid=[](unsigned n){return std::string("ff2efbdf-f0ec-4539-b840-29984797810")+std::to_string(n);};
        Task bank;bank.id=bank.root="637bd562-36d2-5b01-bc01-e2d831c49fe2";
        bank.actor=bank.context.actor=904;bank.source="bank_service";bank.sourceKey="native_transfer_fixture";
        bank.mode=Mode::Active;bank.phase=Phase::Queued;bank.kind=Kind::Maintenance;
        bank.createdAtMs=bank.updatedAtMs=1000;
        assert(db.Write(TaskWrite(bank,0,rid(0),"fixture_created")));
        bank.phase=Phase::Preparing;++bank.revision;
        assert(db.Write(TaskWrite(bank,1,rid(1),"fixture_preparing")));
        ResourceClaim item;item.id=rid(2);item.task=bank.id;item.actor=904;item.itemGuid=9876;
        item.itemEntry=765;item.quantity=5;item.location="bank";item.state="held";
        ++bank.revision;
        assert(db.Write(ResourceReservationWrite(bank,2,rid(3),{{item,0}},{{904,9876,765,5,0,"bank"}})));
        OperationRequest request;request.transition.task=bank;request.transition.expectedRevision=bank.revision;
        ++request.transition.task.revision;request.transition.task.phase=Phase::Executing;request.transition.receipt=rid(4);
        request.kind="bank_withdraw";request.effects=Mask(Effect::Inventory);request.persistence=NativePersistence::Inventory;
        request.itemTransfer=item;
        assert(db.Write(OperationRequestWrite(request)));
        bank=request.transition.task;++bank.revision;bank.phase=Phase::Verifying;
        OperationResult result;result.id=rid(4);result.task=bank.id;result.taskRevision=request.transition.task.revision;
        result.kind="bank_withdraw";result.state=OperationState::Verified;result.nativeReference="bank_item:9876";
        result.evidence="native_bank_stack_relocated";
        const auto moved=BankTransferWrite(bank,request.transition.task.revision,result,rid(5),"{}",item);
        assert(!db.Write(moved.journal,true));
        assert(db.Scalar("SELECT location FROM living_activity_claim WHERE claim_id="+SqlValue(item.id))=="bank");
        const auto exact=db.Scalar("SELECT before_state FROM living_activity_operation WHERE operation_id="+SqlValue(result.id));
        assert(db.Execute("UPDATE living_activity_operation SET before_state=JSON_SET(before_state,'$.native.transfer.quantity',6) WHERE operation_id="+SqlValue(result.id)));
        assert(!db.Write(moved.journal));
        assert(db.Execute("UPDATE living_activity_operation SET before_state="+SqlValue(exact)+" WHERE operation_id="+SqlValue(result.id)));
        assert(db.Write(moved.journal));assert(db.Write(moved.journal));
        assert(db.Scalar("SELECT CONCAT(location,':',state,':',quantity,':',revision) FROM living_activity_claim WHERE claim_id="+SqlValue(item.id))=="bags:held:5:2");
        assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE item_guid=9876")=="1");
    }
    {
        // Exact mail envelope binding and rollback at the real MariaDB journal
        // boundary. Physical mail/inventory proof is a separate realm fixture.
        const auto rid=[](unsigned n){return std::string("ff2efbdf-f0ec-4539-b840-29984797811")+std::to_string(n);};
        Task task;task.id=task.root="637bd562-36d2-5b01-bc01-e2d831c49fe3";
        task.actor=task.context.actor=905;task.source="mail_service";task.sourceKey="native_mail_transfer_fixture";
        task.mode=Mode::Active;task.phase=Phase::Queued;task.kind=Kind::Maintenance;task.createdAtMs=task.updatedAtMs=1000;
        assert(db.Write(TaskWrite(task,0,rid(0),"fixture_created")));
        task.phase=Phase::Preparing;++task.revision;assert(db.Write(TaskWrite(task,1,rid(1),"fixture_preparing")));
        ResourceClaim item;item.id=rid(2);item.task=task.id;item.actor=905;item.itemGuid=9877;
        item.itemEntry=765;item.quantity=5;item.location="mail";item.nativeReference=1234;item.state="held";
        ++task.revision;
        assert(db.Write(ResourceReservationWrite(task,2,rid(3),{{item,0}},{{905,9877,765,5,0,"mail",1234}})));
        OperationRequest request;request.transition.task=task;request.transition.expectedRevision=task.revision;
        ++request.transition.task.revision;request.transition.task.phase=Phase::Executing;request.transition.receipt=rid(4);
        request.kind="mail_collect";request.effects=Mask(Effect::Inventory);request.persistence=NativePersistence::Inventory;request.itemTransfer=item;
        assert(db.Write(OperationRequestWrite(request)));
        task=request.transition.task;++task.revision;task.phase=Phase::Verifying;
        OperationResult result;result.id=rid(4);result.task=task.id;result.taskRevision=request.transition.task.revision;
        result.kind="mail_collect";result.state=OperationState::Verified;result.nativeReference="mail:1234:item:9877";
        result.evidence="native_mail_attachment_collected";
        const auto moved=ItemTransferWrite(task,request.transition.task.revision,result,rid(5),"{}",item);
        assert(!db.Write(moved.journal,true));
        assert(db.Scalar("SELECT CONCAT(location,':',native_reference) FROM living_activity_claim WHERE claim_id="+SqlValue(item.id))=="mail:1234");
        const auto exact=db.Scalar("SELECT before_state FROM living_activity_operation WHERE operation_id="+SqlValue(result.id));
        assert(db.Execute("UPDATE living_activity_operation SET before_state=JSON_SET(before_state,'$.native.transfer.mail',1235) WHERE operation_id="+SqlValue(result.id)));
        assert(!db.Write(moved.journal));
        assert(db.Execute("UPDATE living_activity_operation SET before_state="+SqlValue(exact)+" WHERE operation_id="+SqlValue(result.id)));
        assert(db.Write(moved.journal) && db.Write(moved.journal));
        assert(db.Scalar("SELECT CONCAT(location,':',native_reference,':',state,':',quantity,':',revision) FROM living_activity_claim WHERE claim_id="+SqlValue(item.id))=="bags:0:held:5:2");
        assert(db.Scalar("SELECT COUNT(*) FROM living_activity_claim WHERE item_guid=9877")=="1");
    }
    ProfessionSettlementDatabase(db);
    ProfessionResumeDatabase(db);
    // Execute the exact production projection/expiry SQL, including old-schema
    // compatibility and a newer speculative row with the same recipe label.
    assert(db.Execute("CREATE TABLE characters(guid INT PRIMARY KEY,race INT)"));
    assert(db.Execute("CREATE TABLE organic_economy_profile(character_guid INT PRIMARY KEY,career_participant INT,intended_profession_one INT,intended_profession_two INT,profession_plan_version INT)"));
    assert(db.Execute("CREATE TABLE organic_economy_goal(goal_id BIGINT PRIMARY KEY,character_guid INT,capability_ref VARCHAR(120),goal_type VARCHAR(40),state VARCHAR(20),created_at DATETIME,expires_at DATETIME,authoritative_payload LONGTEXT)"));
    assert(db.Execute("INSERT INTO characters VALUES(899,5)"));
    assert(db.Execute("INSERT INTO organic_economy_profile VALUES(899,1,171,182,1)"));
    assert(db.Execute("INSERT INTO organic_economy_goal VALUES(701,899,'profession:899:2329','profession_skill_up','active',NOW(),DATE_SUB(NOW(),INTERVAL 1 DAY),'{}'),(702,899,'profession:899:2329','profession_skill_up','active',NOW(),DATE_ADD(NOW(),INTERVAL 1 HOUR),'{}')"));
    auto owner=historyTask;owner.id=owner.root="637bd562-36d2-5b01-bc01-e2d831c49ff1";
    owner.actor=owner.context.actor=899;owner.source="profession_job";owner.sourceKey="economy_goal:701";
    owner.mode=Mode::Active;owner.phase=Phase::Preparing;owner.accepted=true;owner.revision=1;
    assert(db.Write(TaskWrite(owner,0,"ff2efbdf-f0ec-4539-b840-299847970c91","legacy_handoff_fixture")));
    auto selected=[&](bool enabled,unsigned column) {
        assert(db.Execute(EconomyProfessionProfilesQuery(enabled)));
        auto* rows=mysql_store_result(db.db);assert(rows && mysql_num_fields(rows)==14 && mysql_num_rows(rows)==1);
        auto row=mysql_fetch_row(rows);const std::string value=row[column] ? row[column] : "";mysql_free_result(rows);return value;
    };
    assert(selected(false,11)=="702" && selected(false,12).empty());
    assert(selected(true,11)=="701" && selected(true,12)==owner.id && selected(true,13)=="preparing");
    assert(db.Execute(EconomyProfessionExpiryQuery(899,true)));
    assert(db.Scalar("SELECT state FROM organic_economy_goal WHERE goal_id=701")=="active");
    assert(db.Scalar("SELECT state FROM organic_economy_goal WHERE goal_id=702")=="expired");
    assert(db.Execute(EconomyProfessionExpiryQuery(900,true)));
    assert(selected(true,11)=="701");
    assert(db.Execute("UPDATE living_activity_task SET phase='completed' WHERE task_id='"+owner.id+"'"));
    assert(selected(true,13)=="completed"); // Pending legacy acknowledgement still finds its native owner.
    assert(db.Execute("UPDATE organic_economy_goal SET state='completed' WHERE goal_id=701"));
    assert(selected(true,11)=="0");
    std::cout << "PASS: real MariaDB task/outbox, consumed/acquired claims, shared vendor/AH budget, bounded profession history, skill-job settlement and exact-row legacy handoff; atomic rollback, stale/changed retry rejection, conservation, uncertain holds and receipt isolation (fixture metadata, NOT native gameplay proof)\n";
}
