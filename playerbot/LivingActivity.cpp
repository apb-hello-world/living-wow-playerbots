#include "LivingActivity.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace LivingActivity
{
    bool SameLease(const ActivityLease& a, const ActivityLease& b) {
        return a.actor && a.generation && a.actor == b.actor && a.rootTask == b.rootTask &&
            a.generation == b.generation && a.context == b.context;
    }
    bool MayAcquireCompatibilityLease(const ActivityLease& held, const ActivityLease& caller,
        const ActivityLease& requested, bool unexpired) {
        if (!requested.actor || !IsUuid(requested.rootTask)) return false;
        return !unexpired || (held.actor == requested.actor && held.rootTask == requested.rootTask &&
            SameLease(held, caller));
    }
    namespace {
        constexpr const char* phases[] = {"queued", "preparing", "traveling", "executing",
            "verifying", "completed", "waiting_external", "paused", "deferred", "failed",
            "cancelled", "reconciling"};
        constexpr const char* kinds[] = {"human_request", "party_errand", "profession",
            "guild_delivery", "guild_event", "progression", "maintenance", "commission",
            "collection_reconciliation"};
        template<size_t N, typename T>
        bool Parse(const std::string& value, const char* const (&names)[N], T& result) {
            for (size_t i = 0; i < N; ++i)
                if (value == names[i]) { result = T(i); return true; }
            return false;
        }
        std::string Number(uint64_t value) { return std::to_string(value); }
    }
    const char* Name(Phase value) {
        const auto i = size_t(value); return i < std::size(phases) ? phases[i] : "invalid";
    }
    const char* Name(Kind value) {
        const auto i = size_t(value); return i < std::size(kinds) ? kinds[i] : "invalid";
    }
    const char* Name(Mode value) {
        return value == Mode::Off ? "off" : value == Mode::Observe ? "observe" :
            value == Mode::Active ? "active" : "invalid";
    }
    bool ParsePhase(const std::string& value, Phase& result) { return Parse(value, phases, result); }
    bool ParseKind(const std::string& value, Kind& result) { return Parse(value, kinds, result); }
    Kind LegacyEconomyKind(const std::string& type) {
        if (type == "profession_skill_up") return Kind::Profession;
        if (type == "equipment_upgrade") return Kind::Progression;
        if (type == "storage_pressure" || type == "maintain_supplies" || type == "list_surplus" ||
            type == "profession_advertisement") return Kind::Maintenance;
        return Kind::CollectionReconciliation;
    }
    bool IsUuid(const std::string& value) {
        if (value.size() != 36) return false;
        for (size_t i = 0; i < value.size(); ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
            else if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
        }
        return value != "00000000-0000-0000-0000-000000000000";
    }
    bool IsToken(const std::string& value, size_t limit, bool empty) {
        return (empty || !value.empty()) && value.size() <= limit &&
            std::all_of(value.begin(), value.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            });
    }
    bool IsSourceKey(const std::string& value) {
        return !value.empty() && value.size() <= 160 && std::all_of(value.begin(), value.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':' || c == '.';
        });
    }
    bool Terminal(Phase phase) {
        return phase == Phase::Completed || phase == Phase::Failed || phase == Phase::Cancelled;
    }
    bool ConsumesActiveTime(Phase phase) {
        return phase == Phase::Preparing || phase == Phase::Traveling ||
            phase == Phase::Executing || phase == Phase::Verifying;
    }
    uint64_t ActiveElapsed(uint64_t prior, Phase phase, uint64_t delta) {
        if (!ConsumesActiveTime(phase)) return prior;
        return delta > std::numeric_limits<uint64_t>::max() - prior ?
            std::numeric_limits<uint64_t>::max() : prior + delta;
    }
    bool WorldContext::operator==(const WorldContext& other) const {
        return std::tie(actor, policyRevision, map, instance, session, sessionRevision, boot,
                actorGeneration, mapGeneration) ==
            std::tie(other.actor, other.policyRevision, other.map, other.instance,
                other.session, other.sessionRevision, other.boot, other.actorGeneration, other.mapGeneration);
    }
    bool CanTransition(const Task& before, Phase after, const CompletionProof& proof) {
        if (Terminal(before.phase) || std::string(Name(after)) == "invalid") return false;
        if (after == Phase::Completed)
            return before.mode == Mode::Active && before.phase == Phase::Verifying &&
                proof.verified && !proof.unresolvedOperation && IsUuid(proof.operation);
        if (after == Phase::Cancelled || after == Phase::Failed)
            return !proof.unresolvedOperation;
        if (after == Phase::Paused || after == Phase::Deferred || after == Phase::WaitingExternal ||
            after == Phase::Reconciling) return true;
        if (after == before.phase) return true; // A bounded verified checkpoint.
        switch (before.phase) {
        case Phase::Queued: return after == Phase::Preparing;
        case Phase::Preparing: return after == Phase::Traveling || after == Phase::Executing;
        case Phase::Traveling: return after == Phase::Executing || after == Phase::Preparing;
        case Phase::Executing: return after == Phase::Verifying;
        case Phase::Verifying: return after == Phase::Preparing;
        case Phase::Paused: case Phase::Deferred: case Phase::WaitingExternal:
            return after == Phase::Reconciling;
        case Phase::Reconciling: return after == Phase::Queued || after == Phase::Preparing;
        default: return false;
        }
    }
    bool Validate(const Task& task, std::string& error) {
        if (!IsUuid(task.id) || !IsUuid(task.root) || (!task.parent.empty() && !IsUuid(task.parent)))
            error = "invalid_task_identity";
        else if (!task.actor || task.context.actor != task.actor || !task.revision ||
            task.revision == std::numeric_limits<uint64_t>::max()) error = "invalid_actor_revision";
        else if (!IsToken(task.source, 48) || task.sourceKey.empty() || task.sourceKey.size() > 160 ||
            !IsToken(task.checkpoint.step) || !IsToken(task.checkpoint.blocker, 64, true))
            error = "invalid_checkpoint_identity";
        else if (task.checkpoint.version != 1 || task.checkpoint.data.size() > 8192 ||
            task.context.session.size() > 120 || task.context.boot.size() > 36)
            error = "unsupported_checkpoint";
        else if (std::string(Name(task.phase)) == "invalid" || std::string(Name(task.kind)) == "invalid" ||
            (task.mode != Mode::Observe && task.mode != Mode::Active) ||
            (task.mode == Mode::Observe && task.phase == Phase::Completed)) error = "invalid_state";
        else if (uint8_t(task.priority) < 10 || uint8_t(task.priority) > 60 || uint8_t(task.priority) % 10)
            error = "invalid_priority";
        else if (task.parent == task.id || (task.parent.empty() && task.root != task.id))
            error = "invalid_dependency";
        else if (!task.createdAtMs || task.updatedAtMs < task.createdAtMs)
            error = "invalid_timestamp";
        else { error.clear(); return true; }
        return false;
    }
    Task AfterRestart(Task task, uint64_t nowMs) {
        if (Terminal(task.phase)) return task;
        task.phase = Phase::Reconciling;
        task.checkpoint.blocker = "restart_revalidation_required";
        task.updatedAtMs = std::max(nowMs, task.updatedAtMs);
        task.context.boot.clear(); // Not an executable context until revalidated.
        task.context.actorGeneration = task.context.mapGeneration = 0;
        if (task.ownerGeneration == std::numeric_limits<uint64_t>::max() ||
            task.revision >= std::numeric_limits<uint64_t>::max() - 1)
            throw std::overflow_error("Task generation requires operator reconciliation");
        ++task.ownerGeneration;
        ++task.revision;
        return task;
    }
    bool Fresh(const Task& task, const ActionContext& action, const WorldContext& current) {
        return task.mode == Mode::Active && !Terminal(task.phase) && !current.boot.empty() &&
            task.id == action.task && task.root == action.rootTask && task.revision == action.revision &&
            task.ownerGeneration == action.ownerGeneration && task.context == current && action.world == current;
    }
    bool Before(const Task& a, const Task& b) {
        // Aging/due order cannot cross human or safety bands. No randomness on ties.
        const auto due = [](uint64_t t) { return t ? t : std::numeric_limits<uint64_t>::max(); };
        return std::make_tuple(a.priority, !a.accepted, due(a.dueAtMs), a.createdAtMs, a.id) <
            std::make_tuple(b.priority, !b.accepted, due(b.dueAtMs), b.createdAtMs, b.id);
    }
    std::string SqlValue(const std::string& value) {
        // Hex literals do not depend on SQL_MODE, character escaping or a
        // connection's NO_BACKSLASH_ESCAPES setting. Empty hex is valid in MariaDB.
        static constexpr char digits[] = "0123456789abcdef";
        std::string result = "X'";
        result.reserve(value.size() * 2 + 3);
        for (unsigned char c : value) { result += digits[c >> 4]; result += digits[c & 15]; }
        return result + "'";
    }
    static WritePlan MakeTaskWrite(const Task& task, uint64_t expected, const std::string& receipt,
        const std::string& code, const std::string& extra) {
        std::string error;
        if (!Validate(task, error) || task.revision != expected + 1 || !IsUuid(receipt) || !IsToken(code))
            throw std::invalid_argument("Invalid task write: " + error);
        // Column/value order is also the receipt fingerprint. Include immutable
        // source/actor/root fields so reusing a request ID with new data fails.
        const std::vector<std::pair<std::string, std::string>> fields = {
            {"task_id", SqlValue(task.id)}, {"source", SqlValue(task.source)},
            {"source_key", SqlValue(task.sourceKey)}, {"actor_guid", Number(task.actor)},
            {"root_task_id", SqlValue(task.root)}, {"parent_task_id", SqlValue(task.parent)},
            {"kind", SqlValue(Name(task.kind))}, {"mode", SqlValue(Name(task.mode))},
            {"phase", SqlValue(Name(task.phase))}, {"priority", Number(uint8_t(task.priority))},
            {"accepted", task.accepted ? "1" : "0"}, {"revision", Number(task.revision)},
            {"owner_generation", Number(task.ownerGeneration)},
            {"session_id", SqlValue(task.context.session)}, {"session_revision", Number(task.context.sessionRevision)},
            {"policy_revision", Number(task.context.policyRevision)}, {"map_id", Number(task.context.map)},
            {"instance_id", Number(task.context.instance)}, {"checkpoint_version", Number(task.checkpoint.version)},
            {"step", SqlValue(task.checkpoint.step)}, {"checkpoint", SqlValue(task.checkpoint.data)},
            {"blocker", SqlValue(task.checkpoint.blocker)}, {"active_elapsed_ms", Number(task.checkpoint.activeElapsedMs)},
            {"last_progress_at_ms", Number(task.checkpoint.lastProgressAtMs)}, {"due_at_ms", Number(task.dueAtMs)},
            {"retry_at_ms", Number(task.retryAtMs)}, {"created_at_ms", Number(task.createdAtMs)},
            {"updated_at_ms", Number(task.updatedAtMs)}, {"last_receipt_id", SqlValue(receipt)}
        };
        std::string columns, values, updates, fingerprint = code + ":" + Number(expected) + ':' + SqlValue(extra);
        for (const auto& field : fields) {
            if (!columns.empty()) { columns += ','; values += ','; updates += ','; }
            columns += field.first; values += field.second;
            updates += field.first + '=' + field.second;
            fingerprint += '|' + field.first + '=' + field.second;
        }
        WritePlan plan;
        plan.task = task.id; plan.revision = task.revision;
        const auto id = SqlValue(task.id), rid = SqlValue(receipt);
        const auto hash = "SHA2(" + SqlValue(fingerprint) + ",256)";
        if (!expected && Terminal(task.phase)) throw std::invalid_argument("New tasks cannot be terminal");
        if (!expected)
            plan.statements.push_back("INSERT INTO living_activity_task (" + columns + ") VALUES (" + values +
                ") ON DUPLICATE KEY UPDATE task_id=task_id");
        else
            plan.statements.push_back("UPDATE living_activity_task SET " + updates + " WHERE task_id=" + id +
                " AND revision=" + Number(expected) + " AND actor_guid=" + Number(task.actor) +
                " AND source=" + SqlValue(task.source) + " AND source_key=" + SqlValue(task.sourceKey) +
                " AND root_task_id=" + SqlValue(task.root) + " AND parent_task_id=" + SqlValue(task.parent) +
                (code == "observation_reclassified" ? "" : " AND kind=" + SqlValue(Name(task.kind))) +
                " AND phase NOT IN ('completed','failed','cancelled')" +
                " AND NOT EXISTS (SELECT 1 FROM living_activity_transition WHERE transition_id=" + rid + ")");
        plan.statements.push_back("INSERT INTO living_activity_transition "
            "(transition_id,task_id,task_revision,actor_guid,phase,code,request_hash,occurred_at_ms) "
            "SELECT " + rid + ",task_id,revision,actor_guid,phase," + SqlValue(code) + ',' + hash + ',' +
            Number(task.updatedAtMs) + " FROM living_activity_task WHERE task_id=" + id + " AND revision=" +
            Number(task.revision) + " AND last_receipt_id=" + rid +
            " ON DUPLICATE KEY UPDATE transition_id=transition_id");
        plan.receiptQuery = "SELECT task_id,task_revision FROM living_activity_transition WHERE transition_id=" +
            rid + " AND request_hash=" + hash;
        return plan;
    }
    WritePlan TaskWrite(const Task& task, uint64_t expected, const std::string& receipt, const std::string& code) {
        if (task.phase == Phase::Executing || task.phase == Phase::Verifying)
            throw std::invalid_argument("Execution and verification require a native operation journal");
        auto plan = MakeTaskWrite(task, expected, receipt, code, "");
        if (expected && (task.phase == Phase::Preparing || task.phase == Phase::Traveling || Terminal(task.phase))) {
            // An old intent cannot be bypassed by changing the same root or a
            // sibling step back to ordinary preparation. Keep accepted work,
            // but require native reconciliation before any new execution.
            plan.statements.front() += " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o "
                "JOIN living_activity_task ot ON ot.task_id=o.task_id WHERE "
                "ot.root_task_id=living_activity_task.root_task_id AND o.state IN ('intent','reconciling'))";
        }
        if (code == "observation_reclassified") {
            if (!expected || task.mode != Mode::Observe || task.phase != Phase::Reconciling || task.source != "economy_goal")
                throw std::invalid_argument("Only unexecuted shadow imports may be reclassified");
            plan.statements.front() += " AND mode='observe' AND phase='reconciling' "
                "AND NOT EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id) "
                "AND NOT EXISTS (SELECT 1 FROM living_activity_claim c WHERE c.task_id=living_activity_task.task_id)";
        }
        if (expected && Terminal(task.phase)) {
            plan.statements.front() += " AND NOT EXISTS (SELECT 1 FROM living_activity_operation o "
                "WHERE o.task_id=living_activity_task.task_id AND o.state IN ('intent','reconciling'))";
            if (task.phase == Phase::Completed)
                plan.statements.front() += " AND mode='active' AND phase='verifying' AND EXISTS "
                    "(SELECT 1 FROM living_activity_operation o WHERE o.task_id=living_activity_task.task_id "
                    "AND o.state='verified') AND EXISTS (SELECT 1 FROM living_activity_transition r "
                    "WHERE r.transition_id=living_activity_task.last_receipt_id AND r.code='operation_verified')";
            // A terminal workflow cannot leave protected quantities behind.
            // Acquired goods remain native possessions when a claim is released.
            plan.statements.front() += " AND NOT EXISTS (SELECT 1 FROM living_activity_claim c "
                "WHERE c.task_id=living_activity_task.task_id AND c.state IN ('held','in_transfer','reconciling'))";
            if (task.parent.empty())
                plan.statements.front() += " AND NOT EXISTS (SELECT 1 FROM living_activity_task child "
                    "WHERE child.parent_task_id=living_activity_task.task_id AND child.phase NOT IN ('completed','cancelled','failed'))";
        }
        if (expected && task.phase == Phase::Reconciling) {
            // An intent present at restart is uncertain, never permission to
            // repeat the native effect. Keep evidence/identity for reconciliation.
            plan.statements.insert(plan.statements.begin() + 1,
                "UPDATE living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id "
                "SET o.state='reconciling',o.updated_at_ms=" + Number(task.updatedAtMs) +
                " WHERE t.task_id=" + SqlValue(task.id) + " AND t.last_receipt_id=" + SqlValue(receipt) +
                " AND o.state='intent' AND EXISTS (" + plan.receiptQuery + ")");
        }
        return plan;
    }
    WritePlan OperationIntentWrite(const Task& task, uint64_t expected, const std::string& op,
        const std::string& kind, const std::string& before) {
        if (!expected || task.phase != Phase::Executing || task.mode != Mode::Active ||
            !IsToken(kind, 48) || before.size() > 8192) throw std::invalid_argument("Invalid operation intent");
        auto plan = MakeTaskWrite(task, expected, op, "operation_intent", kind + ':' + before);
        plan.statements.front() += " AND mode='active' AND phase IN ('preparing','traveling') "
            "AND NOT EXISTS (SELECT 1 FROM living_activity_operation o JOIN living_activity_task ot ON ot.task_id=o.task_id "
            "WHERE ot.actor_guid=living_activity_task.actor_guid AND o.state IN ('intent','reconciling'))";
        plan.statements.push_back("INSERT INTO living_activity_operation (operation_id,task_id,task_revision,kind,"
            "request_hash,state,native_reference,before_state,after_state,evidence_code,created_at_ms,updated_at_ms) "
            "SELECT " + SqlValue(op) + ",t.task_id,t.revision," + SqlValue(kind) + ",r.request_hash,'intent',''," +
            SqlValue(before) + ",'{}',''," + Number(task.updatedAtMs) + ',' + Number(task.updatedAtMs) +
            " FROM living_activity_task t JOIN living_activity_transition r ON r.transition_id=t.last_receipt_id "
            "WHERE t.task_id=" + SqlValue(task.id) + " AND t.last_receipt_id=" + SqlValue(op) +
            " AND t.revision=" + Number(task.revision) + " AND EXISTS (" + plan.receiptQuery +
            ") ON DUPLICATE KEY UPDATE operation_id=operation_id");
        // Acknowledgement confirms persistence only. The executor must also
        // revalidate native context and inspect operation state before execution.
        plan.receiptQuery += " AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.operation_id=" +
            SqlValue(op) + " AND o.task_id=" + SqlValue(task.id) + " AND o.task_revision=" + Number(task.revision) +
            " AND o.kind=" + SqlValue(kind) + " AND o.request_hash=living_activity_transition.request_hash)";
        return plan;
    }
    WritePlan OperationOutcomeWrite(const Task& task, uint64_t expected, const OperationResult& outcome,
        const std::string& receipt, const std::string& after) {
        const bool verified = outcome.state == OperationState::Verified;
        const bool uncertain = outcome.state == OperationState::Reconciling;
        const char* state = verified ? "verified" : uncertain ? "reconciling" : "rejected";
        if (!expected || task.mode != Mode::Active || task.phase != (uncertain ? Phase::Reconciling : Phase::Verifying) ||
            !IsUuid(outcome.id) || outcome.task != task.id || !outcome.taskRevision ||
            !IsToken(outcome.kind, 48) || !IsToken(outcome.evidence) || after.size() > 8192 ||
            outcome.nativeReference.size() > 160 || (verified && outcome.nativeReference.empty()) ||
            (!verified && !uncertain && outcome.state != OperationState::Rejected))
            throw std::invalid_argument("Invalid native operation result");
        const std::string operationWhere = "o.operation_id=" + SqlValue(outcome.id) +
            " AND o.task_id=living_activity_task.task_id AND o.task_revision=" + Number(outcome.taskRevision) +
            " AND o.kind=" + SqlValue(outcome.kind) + " AND o.state IN ('intent','reconciling')";
        auto plan = MakeTaskWrite(task, expected, receipt,
            verified ? "operation_verified" : uncertain ? "operation_uncertain" : "operation_rejected",
            SqlValue(outcome.id) + ':' + Number(outcome.taskRevision) + ':' + SqlValue(outcome.kind) + ':' +
            SqlValue(outcome.nativeReference) + ':' + SqlValue(outcome.evidence) + ':' + SqlValue(after));
        plan.statements.front() += " AND mode='active' AND phase IN ('executing','reconciling') "
            "AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE " + operationWhere + ')';
        // Guard the dependent mutation with the SAME request fingerprint as
        // the task transition. A reused receipt may still be its last receipt
        // after a CAS rejection; checking only that ID would let changed retry
        // contents rewrite uncertain evidence even though acknowledgement fails.
        plan.statements.push_back("UPDATE living_activity_operation o JOIN living_activity_task t ON t.task_id=o.task_id "
            "SET o.state=" + SqlValue(state) + ",o.native_reference=" +
            SqlValue(outcome.nativeReference) + ",o.after_state=" + SqlValue(after) + ",o.evidence_code=" +
            SqlValue(outcome.evidence) + ",o.updated_at_ms=" + Number(task.updatedAtMs) +
            " WHERE o.operation_id=" + SqlValue(outcome.id) + " AND t.last_receipt_id=" + SqlValue(receipt) +
            " AND t.revision=" + Number(task.revision) + " AND o.state IN ('intent','reconciling') "
            "AND EXISTS (" + plan.receiptQuery + ")");
        plan.receiptQuery += " AND EXISTS (SELECT 1 FROM living_activity_operation o WHERE o.operation_id=" +
            SqlValue(outcome.id) + " AND o.task_id=" + SqlValue(task.id) + " AND o.task_revision=" +
            Number(outcome.taskRevision) + " AND o.kind=" + SqlValue(outcome.kind) + " AND o.state=" +
            SqlValue(state) + " AND o.native_reference=" + SqlValue(outcome.nativeReference) +
            " AND o.evidence_code=" + SqlValue(outcome.evidence) + " AND o.after_state=" + SqlValue(after) + ')';
        return plan;
    }
    std::string PersistedTaskProjection() {
        // MariaDB recognizes JSON_VALID-constrained TEXT as a JSON value when
        // nesting it in JSON_OBJECT. Explicit text conversion preserves the
        // checkpoint's exact bytes and numeric types through the row envelope.
        return "JSON_OBJECT('id',task_id,'actor',actor_guid,'source',source,'source_key',source_key,"
            "'root',root_task_id,'parent',parent_task_id,'kind',kind,'mode',mode,'phase',phase,'priority',priority,"
            "'accepted',accepted,'revision',revision,'generation',owner_generation,'session',session_id,"
            "'session_revision',session_revision,'policy_revision',policy_revision,'map',map_id,'instance',instance_id,"
            "'checkpoint_version',checkpoint_version,'step',step,'checkpoint',CONCAT('',checkpoint),'blocker',blocker,"
            "'active_ms',active_elapsed_ms,'progress_at',last_progress_at_ms,'due_at',due_at_ms,'retry_at',retry_at_ms,"
            "'created_at',created_at_ms,'updated_at',updated_at_ms)";
    }
    bool ReceiptMatches(const WritePlan& plan, const std::string& task, uint64_t revision) {
        return task == plan.task && revision == plan.revision && revision > 0;
    }
}
