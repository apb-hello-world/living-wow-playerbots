#include "botpch.h"
#include "Database/DatabaseImpl.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivity.h"
#include "LivingActivityAdmission.h"
#include "LivingActivityCodec.h"
#include "LivingActivityMailbox.h"
#include "LivingActivityAuthority.h"
#include "LivingActivityPermissions.h"
#include "LivingActivityScope.h"
#include "LivingActivityNativeContext.h"
#include "PlayerbotRendezvousManager.h"
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/uuid/name_generator.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/string_generator.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

using namespace LivingActivity;
namespace {
    uint32_t NativeSafety(Player* bot) {
        return ReadNativeSafety(*bot, MovementFlags(MOVEFLAG_FALLING | MOVEFLAG_FALLINGFAR));
    }
    uint64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    std::string NewId() { return boost::uuids::to_string(boost::uuids::random_generator()()); }
    std::string SourceId(const std::string& source, const std::string& key) {
        static const auto ns = boost::uuids::string_generator()("a6dfc222-b146-5f9a-9c85-21519e5e2436");
        // Source keys include the authoritative actor and native record. The
        // character database is the identity scope, including restored copies.
        return boost::uuids::to_string(boost::uuids::name_generator(ns)(source + ':' + key));
    }
    std::string Json(const boost::property_tree::ptree& value) {
        std::ostringstream out; boost::property_tree::write_json(out, value, false); return out.str();
    }
    // Only identifiers and typed source facts enter checkpoints. Do not import
    // arbitrary legacy payloads, user event descriptions or private dialogue.
    std::string ImportQuery(unsigned family, unsigned limit) {
        std::string select, where, order;
        if (family == 0) {
            select = "SELECT 'economy_goal' source,CAST(g.goal_id AS CHAR) source_key,g.character_guid actor,"
                "JSON_OBJECT('goal_id',g.goal_id,'goal_type',g.goal_type,'capability_ref',COALESCE(g.capability_ref,''),"
                "'legacy_phase',g.state,'legacy_expires_at',COALESCE(UNIX_TIMESTAMP(g.expires_at),0)) checkpoint "
                "FROM organic_economy_goal g JOIN characters c ON c.guid=g.character_guid "
                "JOIN tbcrealmd.account a ON a.id=c.account ";
            where = "g.state='active'"; order = "g.goal_id";
        } else if (family == 1) {
            select = "SELECT 'guild_delivery' source,CAST(g.delivery_id AS CHAR) source_key,g.carrier_guid actor,"
                "JSON_OBJECT('delivery_id',g.delivery_id,'guild_id',g.guild_id,'goal_id',g.goal_id,"
                "'item_guid',g.item_guid,'item_entry',g.item_entry,'quantity',g.quantity,'mail_id',g.mail_id,"
                "'legacy_phase',g.phase) checkpoint FROM guild_society_supply_delivery g "
                "JOIN characters c ON c.guid=g.carrier_guid JOIN tbcrealmd.account a ON a.id=c.account ";
            where = "g.phase NOT IN ('completed','cancelled','failed','deposited')"; order = "g.delivery_id";
        } else if (family == 2) {
            select = "SELECT 'commission' source,g.commission_id source_key,g.bot_guid actor,"
                "JSON_OBJECT('commission_id',g.commission_id,'recipe_spell_id',g.recipe_spell_id,"
                "'output_item_entry',g.output_item_entry,'quantity',g.quantity,'legacy_phase',g.state) checkpoint "
                "FROM organic_economy_commission g JOIN characters c ON c.guid=g.bot_guid "
                "JOIN tbcrealmd.account a ON a.id=c.account ";
            where = "g.state IN ('awaiting_materials','materials_received','traveling','crafting','ready')"; order = "g.commission_id";
        } else {
            select = "SELECT 'guild_event' source,CONCAT(g.event_id,':',r.character_guid) source_key,r.character_guid actor,"
                "JSON_OBJECT('event_id',g.event_id,'event_revision',g.revision,'accepted_revision',r.accepted_revision,"
                "'guild_id',g.guild_id,'target_id',g.target_id,'event_type',g.event_type,'scheduled_at',g.scheduled_at,"
                "'legacy_phase',g.state) checkpoint FROM guild_society_event g "
                "JOIN guild_society_rsvp r ON r.event_id=g.event_id JOIN characters c ON c.guid=r.character_guid "
                "JOIN tbcrealmd.account a ON a.id=c.account ";
            where = "r.response='accepted' AND g.state NOT IN ('completed','cancelled','failed','expired','draft')";
            order = "g.event_id,r.character_guid";
        }
        // Sentinel distinguishes a healthy empty result from an SQL failure.
        return "SELECT * FROM (SELECT * FROM (" + select + "WHERE " + where +
            " AND a.username LIKE 'RNDBOT%' ORDER BY " + order + ") candidates WHERE NOT EXISTS "
            "(SELECT 1 FROM living_activity_task t WHERE t.source=candidates.source AND t.source_key=candidates.source_key) LIMIT " +
            std::to_string(limit) + ") bounded UNION ALL SELECT '','',0,'{}'";
    }
}

struct LivingActivityCoordinator::State {
    ExecutionAuthority authority; // Only world-thread methods may access this book.
    const std::string boot = NewId();
    struct Binding { uint64_t actorEpoch = 0; PermissionPublisher publisher; };
    std::map<uint32_t, Binding> bindings;
    struct ActionObservation {
        uint32_t actor;
        uint64_t actorEpoch, mapEpoch;
        Effects effects;
        std::string action;
        bool worldThread = false;
        AuthorityCode check = AuthorityCode::NoOwner;
        std::string scope = "unscoped";
    };
    struct ActionCount { uint64_t count = 0; uint32_t exampleActor = 0; };
    BoundedMailbox<ActionObservation> actionInbox{2048};
    std::atomic<bool> observeEffects{false};
    std::atomic<bool> enforceEffects{false}; // No configuration can enable it before Stage 3 acceptance.
    std::atomic<uint64_t> publishedPolicyRevision{0};
    std::atomic<uint64_t> leaseBoundaries[3][2]{};
    std::thread::id worldThread;
    std::atomic<bool> worldThreadReady{false};
    std::map<std::string, ActionCount> actionCounts;
    uint64_t observedActions = 0, unknownActions = 0, actionCardinalityRejected = 0;
    uint64_t nativeViewsPublished = 0, staleActorObservations = 0;
#ifdef LIVING_ISOLATED_NATIVE_TESTS
    bool fixtureFinished = false;
    uint64_t fixtureNext = 0;
    unsigned admissionFixtureStep = 0;
    uint64_t admissionFixtureDeadline = 0;
    TaskRequest admissionFixtureRequest;
    boost::property_tree::ptree admissionFixtureChecks;
#endif
    Mode effective = Mode::Off;
    std::string desired = "off", blocker = "not_enabled", loadCursor;
    uint64_t policyRevision = 0, epoch = 0, nextPolicy = 0, nextWork = 0, nextLog = 0;
    unsigned batch = 32, loadBatch = 64, maxCache = 20000;
    bool ioPending = false, schemaReady = false, loaded = false;
    unsigned importFamily = 0;
    uint64_t acknowledged = 0, persistenceFailures = 0, invalidRecords = 0, transitionCount = 0;
    uint64_t maximumDispatchUs = 0, overBudgetUpdates = 0;
    struct Pending { Task task; WritePlan plan; std::string admissionReceipt; };
    struct Incoming {
        bool restored = false;
        unsigned family = 0;
        uint32_t actor = 0;
        std::string id, source, key, payload;
    };
    std::deque<Pending> pending;
    std::deque<Incoming> incoming;
    std::map<std::string, Task> cache;
    // Compact last-receipt identity, bounded by the same task cache. Request
    // content is compared with the cached task, not retained as duplicate SQL.
    std::map<std::string, std::string> admissionReceipts;
    uint64_t taskAdmissions = 0, savedGrants = 0;
    std::map<uint32_t, std::string> preferred;
    std::map<std::string, std::string> quarantined;

    void QuarantineIncoming(const std::string& reason) {
        if (incoming.empty()) return;
        const auto& row = incoming.front();
        const std::string id = row.restored ? row.id : SourceId(row.source, row.key);
        quarantined[id] = reason;
        if (row.restored) loadCursor = row.id;
        ++invalidRecords; blocker = reason;
        sLog.outError("Living activity record quarantined: task=%s reason=%s; native journal retained",
            id.c_str(), reason.c_str());
        // Leave the authoritative row untouched and visible in Admin. One bad
        // record must not prevent other bots from rebuilding valid obligations.
        incoming.pop_front();
    }

    void Policy(uint64_t now) {
        if (now < nextPolicy) return;
        nextPolicy = now + 60000;
        Mode next = Mode::Off; std::string why = "not_enabled"; uint64_t revision = 0;
        bool nextObserveEffects = false;
        try {
            std::ifstream input("/srv/living-wow/config/activities.json");
            if (input) {
                boost::property_tree::ptree p; boost::property_tree::read_json(input, p);
                desired = p.get<std::string>("mode", "off"); revision = p.get<uint64_t>("policyRevision", 0);
                if (p.get<unsigned>("schemaVersion", 0) != 1 || !revision) throw std::invalid_argument("version");
                // Diagnostics cannot accidentally become an executor through config.
                bool execution = false;
                for (const char* flag : {"activityOwnership", "serviceExecution", "resourceClaims", "operationJournal", "socialOutbox"})
                    execution |= p.get<bool>(std::string("features.") + flag, false);
                if (desired != "off" && desired != "observe" && desired != "active") throw std::invalid_argument("mode");
                if (desired == "active" || execution) why = "execution_cutover_not_accepted";
                else if (desired == "observe" && p.get<bool>("features.durableTasks", false)) {
                    batch = p.get<unsigned>("limits.persistenceBatch", 32);
                    loadBatch = p.get<unsigned>("limits.loadBatch", 64);
                    maxCache = p.get<unsigned>("limits.cachedTasks", 20000);
                    if (!batch || batch > 32 || !loadBatch || loadBatch > 64 || maxCache < 64 || maxCache > 20000)
                        throw std::invalid_argument("limits");
                    next = Mode::Observe; why.clear();
                    nextObserveEffects = p.get<bool>("features.effectObservation", false);
                }
            } else desired = "off";
        } catch (const std::exception&) { why = "invalid_activity_configuration"; }
        // Do not invalidate a committed journal acknowledgement on config reload.
        // Its receipts still matter, but no gameplay action is ever dispatched here.
        effective = next; policyRevision = revision;
        publishedPolicyRevision.store(revision, std::memory_order_release);
        observeEffects.store(nextObserveEffects, std::memory_order_release);
        if (effective == Mode::Off) blocker = why;
        else if (!schemaReady) blocker = "schema_verification_pending";
    }
    void Remember(const Task& task) {
        cache[task.id] = task;
        const auto existing = preferred.find(task.actor);
        if (existing == preferred.end() || Before(task, cache.at(existing->second))) preferred[task.actor] = task.id;
    }
    void Queue(Task task, uint64_t expected, const std::string& code) {
        if (cache.size() + quarantined.size() + pending.size() >= maxCache) { blocker = "task_cache_backpressure"; return; }
        auto plan = TaskWrite(task, expected, NewId(), code);
        pending.push_back({std::move(task), std::move(plan), ""});
    }
    void Flush(std::chrono::steady_clock::time_point deadline) {
        const unsigned maximum = std::min<unsigned>(batch, pending.size());
        if (!maximum || !CharacterDatabase.BeginTransaction()) return;
        unsigned count = 0;
        std::string query;
        for (; count < maximum;) {
            for (const auto& sql : pending[count].plan.statements) CharacterDatabase.Execute(sql.c_str());
            if (!query.empty()) query += " UNION ALL ";
            query += pending[count].plan.receiptQuery;
            ++count;
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
        // One ordered native DB transaction followed by its receipt query. No
        // synchronous DB query or extra worker on the world thread.
        if (!CharacterDatabase.CommitTransaction()) { CharacterDatabase.RollbackTransaction(); return; }
        ioPending = true;
        const auto token = epoch;
        if (!CharacterDatabase.AsyncQuery([this, count, token](QueryResult* result) {
            if (token != epoch) return;
            ioPending = false;
            std::set<std::pair<std::string, uint64_t>> receipts;
            if (result) do { auto* f = result->Fetch(); receipts.emplace(f[0].GetCppString(), f[1].GetUInt64()); }
                while (result->NextRow());
            unsigned accepted = 0;
            // Whole batch is atomic; require every receipt before acknowledging.
            for (unsigned i = 0; i < count; ++i)
                accepted += receipts.count({pending[i].plan.task, pending[i].plan.revision}) != 0;
            if (accepted != count) {
                ++persistenceFailures; blocker = "journal_receipt_not_verified"; nextWork = NowMs() + 5000; return;
            }
            for (unsigned i = 0; i < count; ++i) {
                const auto& acknowledgedWrite = pending.front();
                Remember(acknowledgedWrite.task);
                if (!acknowledgedWrite.admissionReceipt.empty())
                    admissionReceipts[acknowledgedWrite.task.id] = acknowledgedWrite.admissionReceipt;
                else admissionReceipts.erase(acknowledgedWrite.task.id);
                pending.pop_front(); ++acknowledged; ++transitionCount;
            }
            blocker.clear();
        }, query.c_str())) { ioPending = false; blocker = "journal_ack_queue_unavailable"; nextWork = NowMs() + 5000; }
    }
    void Probe() {
        ioPending = true;
        // Explicit required columns plus version; querying a version row alone
        // would accept a partially applied schema. The empty task table is valid.
        const std::string sql = "SELECT version,(SELECT COUNT(*) FROM information_schema.columns "
            "WHERE table_schema=DATABASE() AND ((table_name='living_activity_task' AND column_name IN "
            "('task_id','source','source_key','revision','owner_generation','checkpoint','last_receipt_id')) OR "
            "(table_name='living_activity_transition' AND column_name IN ('sequence_id','transition_id','request_hash')) OR "
            "(table_name='living_activity_operation' AND column_name IN ('operation_id','state')) OR "
            "(table_name='living_activity_claim' AND column_name IN ('claim_id','task_id')))),"
            "(SELECT COUNT(*) FROM living_activity_transition) FROM living_activity_schema WHERE version=1";
        if (!CharacterDatabase.AsyncQuery([this](QueryResult* result) {
            ioPending = false;
            schemaReady = result && result->Fetch()[0].GetUInt32() == 1 && result->Fetch()[1].GetUInt32() == 14;
            if (schemaReady) transitionCount = result->Fetch()[2].GetUInt64();
            blocker = schemaReady ? "startup_reconciliation" : "activity_schema_unavailable";
            nextWork = NowMs() + (schemaReady ? 1000 : 60000);
        }, sql.c_str())) { ioPending = false; nextWork = NowMs() + 60000; }
    }
    void Load() {
        const std::string projection = PersistedTaskProjection();
        const std::string sql = "SELECT * FROM (SELECT task_id," + projection + " payload FROM living_activity_task "
            "WHERE phase NOT IN ('completed','cancelled','failed') AND task_id>" + SqlValue(loadCursor) +
            " ORDER BY task_id LIMIT " + std::to_string(loadBatch) + ") records UNION ALL SELECT '','{}'";
        ioPending = true;
        if (!CharacterDatabase.AsyncQuery([this](QueryResult* result) {
            ioPending = false;
            if (!result) { blocker = "task_load_query_failed"; nextWork = NowMs() + 5000; return; }
            unsigned count = 0;
            do {
                auto* f = result->Fetch(); const std::string id = f[0].GetCppString(); if (id.empty()) continue;
                incoming.push_back({true, 0, 0, id, "", "", f[1].GetCppString()}); ++count;
            } while (result->NextRow());
            if (count < loadBatch) loaded = true;
        }, sql.c_str())) { ioPending = false; nextWork = NowMs() + 5000; }
    }
    void Import() {
        const unsigned family = importFamily;
        const auto query = ImportQuery(family, batch);
        ioPending = true;
        if (!CharacterDatabase.AsyncQuery([this, family](QueryResult* result) {
            ioPending = false;
            if (!result) { blocker = "legacy_import_query_failed"; nextWork = NowMs() + 60000; return; }
            unsigned count = 0;
            do {
                auto* f = result->Fetch(); std::string source = f[0].GetCppString(); if (source.empty()) continue;
                incoming.push_back({false, family, f[2].GetUInt32(), "", source,
                    f[1].GetCppString(), f[3].GetCppString()}); ++count;
            } while (result->NextRow());
            // One domain cannot monopolize admission. A full rotation pauses
            // one minute only when there is no backlog in the observed domain.
            importFamily = NextImportFamily(family);
            if (importFamily == 0 && count == 0) nextWork = NowMs() + 60000;
        }, query.c_str())) { ioPending = false; nextWork = NowMs() + 5000; }
    }
    void DecodeIncoming(std::chrono::steady_clock::time_point deadline) {
        do {
            if (cache.size() + quarantined.size() + pending.size() >= maxCache) {
                blocker = "task_cache_backpressure"; nextWork = NowMs() + 60000; return;
            }
            const auto& row = incoming.front(); Task task;
            if (row.restored) {
                std::string error;
                if (!DecodeTaskProjection(row.payload, task, error)) {
                    QuarantineIncoming(error); continue;
                }
                // Active tasks are never downgraded or executed by this observer.
                if (task.mode == Mode::Active) { Remember(task); blocker = "active_task_requires_executor"; }
                else {
                    auto resumed = AfterRestart(task, NowMs());
                    std::string code = "restart_revalidation";
                    if (task.source == "economy_goal") {
                        boost::property_tree::ptree p; std::istringstream in(task.checkpoint.data);
                        boost::property_tree::read_json(in, p);
                        const Kind kind = LegacyEconomyKind(p.get<std::string>("goal_type", ""));
                        if (kind != task.kind || task.priority != Priority::Progression || task.accepted) {
                            resumed.kind = kind; resumed.priority = Priority::Progression; resumed.accepted = false;
                            code = "observation_reclassified";
                        }
                    }
                    Queue(std::move(resumed), task.revision, code);
                }
                loadCursor = row.id;
            } else {
                task.source = row.source; task.sourceKey = row.key;
                task.id = task.root = SourceId(task.source, task.sourceKey);
                task.actor = task.context.actor = row.actor;
                task.kind = row.family == 0 ? Kind::CollectionReconciliation : row.family == 1 ? Kind::GuildDelivery :
                    row.family == 2 ? Kind::Commission : Kind::GuildEvent;
                if (row.family == 0) {
                    boost::property_tree::ptree p; std::istringstream in(row.payload);
                    boost::property_tree::read_json(in, p);
                    task.kind = LegacyEconomyKind(p.get<std::string>("goal_type", ""));
                }
                task.priority = row.family == 3 ? Priority::Scheduled :
                    row.family == 0 ? Priority::Progression : Priority::Delivery;
                // A planner goal alone is not a paid/accepted obligation. The
                // later reconciler imports exact purchases/commissions as such.
                task.accepted = row.family != 0;
                task.context.policyRevision = policyRevision;
                task.createdAtMs = task.updatedAtMs = NowMs();
                task.checkpoint.data = row.payload;
                task.checkpoint.blocker = "legacy_work_requires_native_reconciliation";
                Queue(std::move(task), 0, "legacy_observed");
            }
            incoming.pop_front();
        } while (!incoming.empty() && std::chrono::steady_clock::now() < deadline);
    }
};

LivingActivityCoordinator& LivingActivityCoordinator::instance() {
    static LivingActivityCoordinator singleton; return singleton;
}
#ifdef LIVING_ISOLATED_NATIVE_TESTS
#include "../tests/realm/ActivityBoundaryFixture.inc"
#include "../tests/realm/ActivityAdmissionFixture.inc"
#endif

LivingActivityCoordinator::LivingActivityCoordinator() : state(new State) {
#ifdef LIVING_ISOLATED_NATIVE_TESTS
    RequireIsolatedNativeFixtureEnvironment();
#endif
}
LivingActivityCoordinator::~LivingActivityCoordinator() = default;
void LivingActivityCoordinator::Update() {
    if (!state->worldThreadReady.load(std::memory_order_acquire)) {
        state->worldThread = std::this_thread::get_id();
        state->worldThreadReady.store(true, std::memory_order_release);
    }
    MANGOS_ASSERT(state->worldThread == std::this_thread::get_id());
    const auto started = std::chrono::steady_clock::now();
    struct Measure {
        State& state; std::chrono::steady_clock::time_point start;
        ~Measure() {
            const auto us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count());
            state.maximumDispatchUs = std::max(state.maximumDispatchUs, us);
            if (us > 2000) ++state.overBudgetUpdates;
        }
    } measure{*state, started};
    const uint64_t now = NowMs();
    state->Policy(now);
    for (const auto& observation : state->actionInbox.Drain(16)) {
        if (observation.actorEpoch) RefreshPermission(observation.actor, observation.actorEpoch);
        ++state->observedActions;
        if (!observation.effects.classified) ++state->unknownActions;
        const std::string key = std::to_string(observation.effects.mask) + ':' +
            std::to_string(static_cast<unsigned>(observation.effects.lane)) + ':' +
            (observation.worldThread ? "world:" : "map:") + observation.action + ':' +
            Name(observation.check) + ':' + observation.scope;
        const auto found = state->actionCounts.find(key);
        if (found == state->actionCounts.end() && state->actionCounts.size() >= 256) {
            ++state->actionCardinalityRejected; continue;
        }
        auto& count = state->actionCounts[key]; ++count.count; count.exampleActor = observation.actor;
    }
    if (now >= state->nextLog && state->desired != "off") {
        state->nextLog = now + 60000;
        sLog.outString("Living activity shadow: %s", StatusJson().c_str());
    }
    const auto deadline = started + std::chrono::milliseconds(2);
#ifdef LIVING_ISOLATED_NATIVE_TESTS
    RunIsolatedBoundaryFixture();
#endif
    if (std::chrono::steady_clock::now() >= deadline) return;
    ObservationQueue queues;
    queues.enabled = state->effective != Mode::Off; queues.ioPending = state->ioPending;
    queues.due = now >= state->nextWork; queues.schemaReady = state->schemaReady; queues.loaded = state->loaded;
    queues.cached = state->cache.size() + state->quarantined.size(); queues.pending = state->pending.size(); queues.incoming = state->incoming.size();
    queues.cacheLimit = state->maxCache; queues.retained = state->transitionCount;
    const auto work = NextObservationWork(queues);
    if (work == ObservationWork::Wait) return;
    if (work == ObservationWork::Decode) {
        try { state->DecodeIncoming(deadline); }
        catch (const std::exception&) { state->QuarantineIncoming("invalid_source_record"); state->nextWork = now + 1000; }
        return;
    }
    state->nextWork = now + 1000;
    switch (work) {
        case ObservationWork::Probe: state->Probe(); break;
        case ObservationWork::Flush: state->Flush(deadline); break;
        case ObservationWork::Load: state->Load(); break;
        case ObservationWork::Import: state->Import(); break;
        case ObservationWork::HistoryPressure: state->blocker = "transition_outbox_backpressure"; break;
        case ObservationWork::CachePressure: state->blocker = "task_cache_backpressure"; break;
        default: break;
    }
}
std::string LivingActivityCoordinator::StatusJson() const {
    boost::property_tree::ptree p;
    p.put("contract_version", 1); p.put("desired_mode", state->desired); p.put("effective_mode", Name(state->effective));
    p.put("policy_revision", state->policyRevision); p.put("blocker", state->blocker);
    p.put("cached_tasks", state->cache.size()); p.put("pending_writes", state->pending.size());
    p.put("receipt_count", state->acknowledged); p.put("persistence_failures", state->persistenceFailures);
    p.put("retained_transitions", state->transitionCount);
    p.put("pending_decode", state->incoming.size()); p.put("maximum_dispatch_us", state->maximumDispatchUs);
    p.put("over_budget_updates", state->overBudgetUpdates); p.put("next_import_family", state->importFamily);
    p.put("invalid_records", state->invalidRecords); p.put("gameplay_mutations", 0);
    p.put("observed_actions", state->observedActions); p.put("unknown_effect_actions", state->unknownActions);
    p.put("optional_action_observations_rejected", state->actionInbox.Rejected());
    p.put("action_cardinality_rejected", state->actionCardinalityRejected);
    p.put("native_views_published", state->nativeViewsPublished);
    p.put("stale_actor_observations", state->staleActorObservations);
    p.put("task_admission_writes", state->taskAdmissions); p.put("saved_task_grants", state->savedGrants);
    p.put("execution_enforcement", state->enforceEffects.load(std::memory_order_acquire));
#ifdef LIVING_ISOLATED_NATIVE_TESTS
    p.put("isolated_fixture_build", true);
    p.put("isolated_fixture_attempted", state->fixtureFinished);
#endif
    for (unsigned boundary = 0; boundary != 3; ++boundary) {
        const char* name = boundary == 0 ? "acquire" : boundary == 1 ? "renew" : "release";
        for (unsigned lane = 0; lane != 2; ++lane)
            p.put(std::string("lease_boundaries.") + name + (lane == 0 ? ".world" : ".map"),
                state->leaseBoundaries[boundary][lane].load(std::memory_order_relaxed));
    }
    boost::property_tree::ptree effects;
    for (const auto& entry : state->actionCounts) {
        boost::property_tree::ptree value; value.put("key", entry.first);
        value.put("count", entry.second.count); value.put("example_actor", entry.second.exampleActor);
        effects.push_back({"", value});
    }
    p.add_child("action_effects", effects);
    p.put("quarantined_records", state->quarantined.size());
    if (!state->quarantined.empty()) {
        p.put("quarantined_task", state->quarantined.begin()->first);
        p.put("quarantined_reason", state->quarantined.begin()->second);
    }
    p.put("snapshot_at_ms", NowMs()); return Json(p);
}
std::string LivingActivityCoordinator::ActorJson(uint32_t guid) const {
    boost::property_tree::ptree p;
    p.put("actor_guid", guid); p.put("effective_mode", Name(state->effective));
    p.put("execution_owner", PlayerbotRendezvousManager::PartyActivityOwnerName(
        PlayerbotRendezvousManager::instance().GetPartyActivityOwner(guid)));
    auto selected = state->preferred.find(guid);
    if (selected != state->preferred.end()) {
        const Task& task = state->cache.at(selected->second);
        p.put("shadow_task_id", task.id); p.put("task_revision", task.revision);
        p.put("root_task_id", task.root); p.put("task_type", Name(task.kind));
        p.put("owner_generation", task.ownerGeneration);
        p.put("phase", Name(task.phase)); p.put("step", task.checkpoint.step);
        p.put("blocker", task.checkpoint.blocker); p.put("active_elapsed_ms", task.checkpoint.activeElapsedMs);
        p.put("source", task.source); p.put("last_progress_at_ms", task.checkpoint.lastProgressAtMs);
        p.put("updated_at_ms", task.updatedAtMs); p.put("due_at_ms", task.dueAtMs); p.put("retry_at_ms", task.retryAtMs);
    }
    return Json(p);
}

void LivingActivityCoordinator::ObserveAction(PlayerbotAI& ai, const Effects& effects, const std::string& action) {
    PermitEffects(ai, effects, action);
}

bool LivingActivityCoordinator::PermitEffects(PlayerbotAI& ai, const Effects& effects, const std::string& action) {
    const bool enforce = state->enforceEffects.load(std::memory_order_acquire);
    if (!enforce && !state->observeEffects.load(std::memory_order_acquire)) return true;
    Player* bot = ai.GetBot();
    const uint32_t guid = bot ? bot->GetGUIDLow() : 0;
    const uint64_t actorEpoch = ai.GetActivityActorEpoch(), mapEpoch = ai.GetActivityMapEpoch();
    if (!guid || !actorEpoch || !mapEpoch) return !enforce;
    // Fixed engine action identifiers only. Never include Event text, commands,
    // player names, model output, credentials or arbitrarily qualified strings.
    std::string bounded = action.substr(0, action.find("::")); // Omit the qualifier, retain its fixed action family.
    if (bounded.empty() || bounded.size() > 64 || bounded.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 _-'") != std::string::npos)
        bounded = "dynamic_action_identifier";
    AuthorityCode check = AuthorityCode::StaleContext;
    if (const auto view = ai.activityPermissions.Inspect()) {
        const auto current = ReadNativeContext(*bot,
            state->publishedPolicyRevision.load(std::memory_order_acquire), state->boot);
        const uint64_t monotonic = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        check = ExecutionScope::Check(ai.activityPermissions, effects, current, monotonic, NativeSafety(bot));
    }
    state->actionInbox.TryPush({guid, actorEpoch, mapEpoch, effects, std::move(bounded),
        state->worldThreadReady.load(std::memory_order_acquire) && state->worldThread == std::this_thread::get_id(),
        check, ExecutionScope::Origin(guid)});
    return !enforce || check == AuthorityCode::Allowed;
}

void LivingActivityCoordinator::RefreshPermission(uint32_t guid, uint64_t actorEpoch) {
    MANGOS_ASSERT(state->worldThread == std::this_thread::get_id());
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(guid);
    auto* ai = bot ? bot->GetPlayerbotAI() : nullptr;
    if (!ai || ai->GetActivityActorEpoch() != actorEpoch) { ++state->staleActorObservations; return; }
    auto binding = state->bindings.find(guid);
    if (binding == state->bindings.end()) {
        if (state->bindings.size() >= 20000) return;
        binding = state->bindings.emplace(guid, State::Binding{}).first;
    }
    auto& entry = binding->second;
    if (entry.actorEpoch != actorEpoch) {
        entry.publisher.Revoke(); state->authority.Forget(guid);
        entry.actorEpoch = actorEpoch;
        ai->activityPermissions = entry.publisher.Reader();
    }
    const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
    if (!current.mapGeneration) { entry.publisher.Revoke(); state->authority.Forget(guid); return; }
    const uint32_t safety = NativeSafety(bot);
    const auto prior = ai->activityPermissions.Inspect();
    if (prior && prior->current == current && prior->safety == safety) return;
    const auto observed = state->authority.Observe(current, safety);
    if (observed.code == AuthorityCode::InvalidRequest || observed.code == AuthorityCode::Capacity) {
        entry.publisher.Revoke(); return;
    }
    entry.publisher.Publish(state->authority.Read(guid)); ++state->nativeViewsPublished;
    // Stage 3 observation only: no task is admitted/acquired, and this view
    // never grants or rejects the actual legacy native action. Cutover is gated.
}

void LivingActivityCoordinator::ObserveLeaseBoundary(uint32_t guid, LeaseBoundary boundary) {
    if (!state->observeEffects.load(std::memory_order_acquire) || !guid) return;
    const unsigned index = static_cast<unsigned>(boundary);
    if (index >= 3) return;
    const bool worldThread = OnWorldThread();
    ++state->leaseBoundaries[index][worldThread ? 0 : 1];
    const char* action = boundary == LeaseBoundary::Acquire ? "legacy lease acquire" :
        boundary == LeaseBoundary::Renew ? "legacy lease renew" : "legacy lease release";
    state->actionInbox.TryPush({guid, 0, 0, {0, Lane::Inspection, true}, action,
        worldThread});
}

bool LivingActivityCoordinator::CompatibilityContext(uint32_t guid, const std::string& source,
    const std::string& key, ActivityLease& identity) const {
    if (!OnWorldThread() || !guid || !IsToken(source) || !IsSourceKey(key)) return false;
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(guid);
    if (!bot || !bot->GetPlayerbotAI()) return false;
    const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
    if (!current.actorGeneration || !current.mapGeneration) return false;
    identity = {};
    identity.actor = guid;
    identity.rootTask = SourceId(source, key);
    identity.context = current;
    return true;
}

bool LivingActivityCoordinator::OnWorldThread() const {
    return state->worldThreadReady.load(std::memory_order_acquire) &&
        state->worldThread == std::this_thread::get_id();
}

AdmissionResult LivingActivityCoordinator::SubmitTask(const TaskRequest& request) {
    AdmissionResult result; result.task = request.task.id; result.revision = request.task.revision;
    auto reject = [&](AdmissionCode code, const std::string& reason = "") {
        result.code = code; result.blocker = reason.empty() ? Name(code) : reason; return result;
    };
    if (!OnWorldThread()) return reject(AdmissionCode::InvalidRequest, "world_thread_required");
    if (!state->enforceEffects.load(std::memory_order_acquire)) return reject(AdmissionCode::Disabled);
    if (!state->schemaReady || !state->loaded || !state->incoming.empty()) return reject(AdmissionCode::NotReady);
    const Task& task = request.task;
    if (task.id != SourceId(task.source, task.sourceKey))
        return reject(AdmissionCode::InvalidRequest, "source_identity_mismatch");
    if (!task.parent.empty() && ParentRevision(task) != request.rootRevision)
        return reject(AdmissionCode::InvalidRequest, "checkpoint_parent_revision_required");
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(task.actor);
    if (!bot || !bot->GetPlayerbotAI()) return reject(AdmissionCode::StaleContext, "actor_not_available");
    const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
    const auto saved = state->cache.find(task.id);
    WritePlan plan;
    try { plan = TaskWrite(task, request.expectedRevision, request.receipt, "task_admitted"); }
    catch (const std::exception&) { return reject(AdmissionCode::InvalidRequest); }
    for (const auto& queued : state->pending) {
        if (queued.admissionReceipt == request.receipt && queued.task.id != task.id)
            return reject(AdmissionCode::InvalidRequest, "receipt_identity_reused");
        if (!task.parent.empty() && queued.task.id == task.root) return reject(AdmissionCode::ConflictingWrite);
        if (queued.task.id != task.id) continue;
        // Duplicate calls share one pending write. Changing content with an old
        // operation ID is never allowed to mutate a queued transaction.
        if (queued.task.context == task.context && SameRequest(queued.plan, plan))
            return reject(AdmissionCode::Pending);
        return reject(AdmissionCode::ConflictingWrite);
    }
    const auto acknowledgedReceipt = state->admissionReceipts.find(task.id);
    if (saved != state->cache.end() && saved->second.revision == task.revision &&
        acknowledgedReceipt != state->admissionReceipts.end() && acknowledgedReceipt->second == request.receipt &&
        saved->second.context == task.context && task.context == current &&
        SameRequest(TaskWrite(saved->second, request.expectedRevision, request.receipt, "task_admitted"), plan))
        return reject(AdmissionCode::Saved);
    std::string reason;
    const auto parent = state->cache.find(task.root);
    const auto valid = ValidateTaskRequest(request, saved == state->cache.end() ? nullptr : &saved->second, current, reason,
        parent == state->cache.end() ? nullptr : &parent->second);
    if (valid != AdmissionCode::Pending) return reject(valid, reason);
    if (task.mode != Mode::Active) return reject(AdmissionCode::InvalidRequest, "managed_task_requires_active_mode");
    if (state->pending.size() >= state->batch ||
        (saved == state->cache.end() && state->cache.size() + state->pending.size() + state->quarantined.size() >= state->maxCache) ||
        state->transitionCount + state->pending.size() >= 200000)
        return reject(AdmissionCode::Backpressure);
    RefreshPermission(task.actor, bot->GetPlayerbotAI()->GetActivityActorEpoch());
    const auto owned = state->authority.Read(task.actor);
    // An operation in flight must produce/reconcile its native receipt before
    // a planner or domain can replace even this task's checkpoint.
    if (!owned.operation.empty()) return reject(AdmissionCode::ReconciliationRequired, "atomic_operation_pending");
    state->pending.push_back({task, std::move(plan), request.receipt});
    ++state->taskAdmissions;
    if (owned.lease.rootTask == task.id || owned.step.id == task.id) {
        state->authority.Release(owned.lease);
        state->bindings.at(task.actor).publisher.Publish(state->authority.Read(task.actor));
    }
    state->nextWork = 0; // Existing bounded receipt queue, no new worker/timer.
    return reject(AdmissionCode::Pending);
}

LivingActivityCoordinator::TaskGrant LivingActivityCoordinator::AcquireSavedTask(
    const std::string& id, uint64_t revision, uint32_t effects, uint64_t durationMs, const std::string& origin) {
    TaskGrant result;
    if (!OnWorldThread() || !state->enforceEffects.load(std::memory_order_acquire)) {
        result.blocker = "execution_disabled"; return result;
    }
    const auto saved = state->cache.find(id);
    if (!state->schemaReady || !state->loaded || saved == state->cache.end() || !IsToken(origin)) {
        result.blocker = "acknowledged_task_unavailable"; return result;
    }
    for (const auto& queued : state->pending) if (queued.task.root == id) {
        result.blocker = "task_write_pending"; return result;
    }
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(saved->second.actor);
    if (!bot || !bot->GetPlayerbotAI()) { result.blocker = "actor_not_available"; return result; }
    const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
    if (!SavedTaskExecutable(saved->second, revision, current, NowMs(), result.blocker)) return result;
    RefreshPermission(saved->second.actor, bot->GetPlayerbotAI()->GetActivityActorEpoch());
    const uint64_t monotonic = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    result.authority = state->authority.Acquire(saved->second, effects, monotonic, durationMs);
    if (!result.authority.Granted()) { result.blocker = Name(result.authority.code); return result; }
    result.task = saved->second; result.task.ownerGeneration = result.authority.lease.generation;
    result.action.task = result.task.id; result.action.rootTask = result.task.root;
    result.action.revision = result.task.revision; result.action.ownerGeneration = result.task.ownerGeneration;
    result.action.world = current; result.action.origin = origin; result.action.permittedEffects = effects;
    state->bindings.at(result.task.actor).publisher.Publish(state->authority.Read(result.task.actor));
    ++state->savedGrants;
    return result;
}

LivingActivityCoordinator::TaskGrant LivingActivityCoordinator::SelectSavedStep(const ActivityLease& lease,
    const std::string& id, uint64_t revision, uint32_t effects, const std::string& origin) {
    TaskGrant result;
    if (!OnWorldThread() || !state->enforceEffects.load(std::memory_order_acquire)) {
        result.blocker = "execution_disabled"; return result;
    }
    const auto root = state->cache.find(lease.rootTask);
    const auto selected = state->cache.find(id.empty() ? lease.rootTask : id);
    if (root == state->cache.end() || selected == state->cache.end() || !IsToken(origin)) {
        result.blocker = "acknowledged_task_unavailable"; return result;
    }
    for (const auto& queued : state->pending) if (queued.task.root == lease.rootTask) {
        result.blocker = "task_write_pending"; return result;
    }
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(lease.actor);
    if (!bot || !bot->GetPlayerbotAI()) { result.blocker = "actor_not_available"; return result; }
    const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
    if (id.empty() ? !SavedTaskExecutable(root->second, revision, current, NowMs(), result.blocker) :
        !SavedStepExecutable(selected->second, root->second, revision, current, NowMs(), result.blocker)) return result;
    RefreshPermission(lease.actor, bot->GetPlayerbotAI()->GetActivityActorEpoch());
    const uint64_t monotonic = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto held = state->authority.Inspect(lease.actor, monotonic);
    if (held.code != AuthorityCode::Allowed || !SameLease(held.lease, lease) ||
        !effects || (effects & ~state->authority.Read(lease.actor).effects)) {
        result.blocker = "current_root_lease_required"; return result;
    }
    result.authority.code = state->authority.SelectStep(lease, id.empty() ? nullptr : &selected->second);
    if (result.authority.code != AuthorityCode::Allowed) { result.blocker = Name(result.authority.code); return result; }
    result.authority.lease = lease; result.task = selected->second; result.task.ownerGeneration = lease.generation;
    result.action.task = result.task.id; result.action.rootTask = result.task.root;
    result.action.revision = result.task.revision; result.action.ownerGeneration = lease.generation;
    result.action.world = current; result.action.origin = origin; result.action.permittedEffects = effects;
    state->bindings.at(lease.actor).publisher.Publish(state->authority.Read(lease.actor));
    return result;
}

AuthorityResult LivingActivityCoordinator::ReleaseTaskLease(const ActivityLease& lease) {
    if (!OnWorldThread()) return {AuthorityCode::InvalidRequest, {}, {}};
    auto result = state->authority.Release(lease); // Pending native operations cannot be discarded.
    const auto binding = state->bindings.find(lease.actor);
    if (binding != state->bindings.end()) binding->second.publisher.Publish(state->authority.Read(lease.actor));
    return result;
}
