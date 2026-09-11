#include "botpch.h"
#include "Database/DatabaseImpl.h"
#include "LivingActivityCoordinator.h"
#include "LivingActivity.h"
#include "LivingActivityAdmission.h"
#include "LivingActivityCodec.h"
#include "LivingActivityClaimCodec.h"
#include "LivingActivityReceipts.h"
#include "LivingActivityNativeCommit.h"
#include "LivingActivityMailbox.h"
#include "LivingActivityAuthority.h"
#include "LivingActivityPermissions.h"
#include "LivingActivityScope.h"
#include "LivingActivityNativeContext.h"
#include "LivingActivityCommitments.h"
#include "LivingProfessionNative.h"
#include "PlayerbotRendezvousManager.h"
#include "PlayerbotActionBroker.h"
#include "PlayerbotGuildSupplies.h"
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
    PartyProtection NativePartyProtection(Player& bot) {
        const auto* group = bot.GetGroup();
        if (!group) return PartyProtection::None;
        return ReadPartyProtection(group->GetMemberSlots(),
            [](uint32_t account) { return sPlayerbotAIConfig.IsInRandomAccountList(account); },
            [](ObjectGuid guid) {
                Player* member = sObjectMgr.GetPlayer(guid);
                return member && member->isRealPlayer();
            });
    }
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
    std::vector<NativeResourceBalance> NativeClaimBalances(Player& bot,const std::vector<ResourceClaim>& claims,bool admission=true) {
        std::map<uint32_t,NativeResourceBalance> owned;
        bool bags=false,bank=false,money=false;
        for (const auto& claim : claims) if (claim.state == "held") {
            bags |= claim.location == "bags"; bank |= claim.location == "bank"; money |= claim.location == "money";
        }
        const auto actor=bot.GetGUIDLow();
        if (money) {
            const uint32_t native=bot.GetMoney(),legacy=admission ? sPlayerbotActionBroker.ReservedCopper(actor) : 0;
            owned.emplace(0,NativeResourceBalance{actor,0,0,0,native-std::min(native,legacy),"money"});
        }
        for (const auto& service : {std::make_pair(bags,IterateItemsMask::ITERATE_ITEMS_IN_BAGS),
                                   std::make_pair(bank,IterateItemsMask::ITERATE_ITEMS_IN_BANK)}) if (service.first)
            for (Item* item : bot.GetPlayerbotAI()->InventoryParseItems("inventory",service.second)) {
                if (!item || (admission && (sPlayerbotActionBroker.IsItemReserved(item->GetGUIDLow()) ||
                    sGuildSupplies.ReservedEntry(actor,item->GetEntry())))) continue;
                const std::string location=service.second == IterateItemsMask::ITERATE_ITEMS_IN_BAGS ? "bags" : "bank";
                for (const auto& claim : claims) if (claim.state == "held" && claim.itemGuid == item->GetGUIDLow() &&
                    claim.itemEntry == item->GetEntry() && claim.location == location)
                    owned.emplace(item->GetGUIDLow(),NativeResourceBalance{actor,item->GetGUIDLow(),item->GetEntry(),item->GetCount(),0,location});
            }
        std::vector<NativeResourceBalance> result;
        for (const auto& row : owned) result.push_back(row.second);
        return result;
    }
    std::vector<NativeResourceBalance> NativeConsumptionBalances(Player& bot,const OperationRequest& request,bool admission=true) {
        std::vector<ResourceClaim> claims;
        for (const auto& use : request.consumption) claims.push_back(use.before);
        return NativeClaimBalances(bot,claims,admission);
    }
    std::vector<NativeItemStack> NativeGainStacks(Player& bot,const ItemGainSpec& spec) {
        std::vector<NativeItemStack> rows;
        if (spec.Empty()) return rows;
        if (!ValidItemGainSpec(spec) || !bot.GetPlayerbotAI()) throw std::invalid_argument("Invalid native gain actor/scope");
        for (Item* item : bot.GetPlayerbotAI()->InventoryParseItems("inventory",IterateItemsMask::ITERATE_ITEMS_IN_BAGS)) {
            if (!item || item->GetEntry() != spec.entry) continue;
            if (rows.size() >= 256 || item->GetOwnerGuid() != bot.GetObjectGuid())
                throw std::invalid_argument("Native gain inventory identity unavailable");
            rows.push_back({bot.GetGUIDLow(),item->GetGUIDLow(),item->GetEntry(),item->GetCount(),
                item->GetContainer() ? item->GetContainer()->GetGUIDLow() : 0,item->GetSlot()});
        }
        return rows;
    }
    std::string NativeGainProof(const std::vector<VerifiedItemGain>& gains) {
        std::string predicate;
        for (const auto& gain : gains) {
            const auto& item=gain.after;
            // Pinned native inventory schema. This is a predicate on the real
            // serializers' results, never replacement inventory-writing SQL.
            predicate+=" AND EXISTS (SELECT 1 FROM character_inventory v JOIN item_instance i ON i.guid=v.item WHERE v.guid="+
                std::to_string(item.actor)+" AND v.item="+std::to_string(item.guid)+" AND v.item_template="+std::to_string(item.entry)+
                " AND v.bag="+std::to_string(item.bagGuid)+" AND v.slot="+std::to_string(item.slot)+
                " AND i.owner_guid="+std::to_string(item.actor)+" AND i.itemEntry="+std::to_string(item.entry)+
                " AND i.count="+std::to_string(item.count)+')';
        }
        return predicate;
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
    std::set<uint32_t> compatibilityActors; // Enumeration index only; no second lease/owner state.
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
    std::shared_ptr<const std::set<uint32_t>> nativeSaveHolds = std::make_shared<const std::set<uint32_t>>();
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
    TaskRequest admissionFixtureRequest, admissionFixtureChild;
    OperationRequest operationFixtureRequest;
    ReservationRequest reservationFixtureRequest;
    unsigned operationFixtureCalls = 0;
    boost::property_tree::ptree admissionFixtureChecks;
    boost::property_tree::ptree nativeSaveFixtureChecks;
    unsigned nativeSaveFixtureAttempts = 0;
    bool nativeSaveFixtureLostAck = false;
    std::atomic<uint32_t> gameplayFixtureActor{0}, gameplayFixtureSpell{0};
    std::atomic<uint32_t> gameplayHealPackets{0}, gameplayHealAmount{0};
    std::atomic<uint32_t> gameplayCastFailure{0}, gameplayCastResult{0};
    uint32_t gameplayOriginalHealth = 0, gameplayOriginalMana = 0, gameplaySetupHealth = 0;
    uint32_t gameplayOriginalRage = 0, gameplayOriginalEnergy = 0;
    bool supportFixtureLookupStarted = false;
    uint32_t supportFixtureRequestedActor = 0;
    boost::property_tree::ptree supportFixtureCandidates;
    uint32_t gameplayOriginalDelay = 0;
    uint64_t gameplayOriginalMoney = 0, gameplayDeadline = 0;
    ObjectGuid gameplayOriginalSelection;
    WorldContext gameplayContext;
    ActivityLease gameplayLease;
    std::string gameplayAtomicFixture;
    uint32_t gameplayMinimumMana = 0;
    uint64_t gameplayNextSample = 0;
    boost::property_tree::ptree gameplayTrace;
    uint64_t combatFixtureDeadline = 0;
    uint64_t petFixtureDeadline = 0;
    uint64_t professionFixtureDeadline = 0;
    std::atomic<uint64_t> petFixtureCaster{0}, petFixtureTarget{0};
    std::atomic<uint32_t> petFixtureDamagePackets{0}, petFixtureDamage{0};
    boost::property_tree::ptree petFixtureCandidates;
#endif
    Mode effective = Mode::Off;
    std::string desired = "off", blocker = "not_enabled", loadCursor;
    uint64_t policyRevision = 0, epoch = 0, nextPolicy = 0, nextWork = 0, nextLog = 0;
    unsigned batch = 32, loadBatch = 64, maxCache = 20000;
    bool ioPending = false, schemaReady = false, loaded = false;
    std::atomic<bool> purchaseLedgerReady{false};
    struct BudgetRead {
        std::string task, operation, blocker="purchase_budget_queued";
        uint64_t revision=0, generation=0, receivedAt=0, dueAt=0, requestedAt=0;
        PurchaseSpend spend;
        bool pending=false;
    };
    std::map<uint32_t,BudgetRead> purchaseBudgets; // At most 64 requested reads.
    uint64_t budgetReads=0, budgetFailures=0;
    unsigned importFamily = 0;
    uint64_t acknowledged = 0, persistenceFailures = 0, invalidRecords = 0, transitionCount = 0;
    uint64_t maximumDispatchUs = 0, overBudgetUpdates = 0;
    struct Pending {
        Task task; WritePlan plan; std::string admissionReceipt;
        std::string operation;
        bool operationOutcome = false;
        ReceiptRetry retry{};
        std::string reservation;
        std::string gainReservation;
        std::vector<ClaimReceiptChange> claims;
        std::shared_ptr<NativeSaveBatch> nativeSave;
    };
    struct PendingOperation {
        OperationRequest request;
        bool ready = false, dispatched = false, uncertain = false, saveBlocked = false;
        OperationState outcome = OperationState::Reconciling;
        ActivityLease held;
    };
    // Bounded transient admission state. An entry is created before its intent
    // write; it is NEVER reconstructed as dispatchable from a persisted intent.
    std::map<std::string, PendingOperation> operations;
    bool operationDispatching = false;
    uint64_t nativeDispatches = 0, nativeOutcomes = 0, nativeVerifiedResults = 0;
    struct Incoming {
        bool restored = false;
        unsigned family = 0;
        uint32_t actor = 0;
        std::string id, source, key, payload;
    };
    std::deque<Pending> pending;
    bool preferReceiptRetry = false;
    std::deque<Incoming> incoming;
    std::map<std::string, Task> cache;
    ResourceClaimBook resources;
    struct IncomingClaim { std::string id, payload; uint32_t taskActor; std::string taskPhase; };
    std::deque<IncomingClaim> incomingClaims;
    std::string claimCursor, claimBlocker = "claim_restore_pending";
    bool claimsEnumerated = false, claimRestoreFailed = false;
    uint64_t invalidClaims = 0;
    // Compact last-receipt identity, bounded by the same task cache. Request
    // content is compared with the cached task, not retained as duplicate SQL.
    std::map<std::string, std::string> admissionReceipts;
    std::map<std::string,std::vector<std::string>> reservationClaimIds;
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
        for (const auto& write : pending) if (write.task.id == task.id) return; // Preserve the original failed write.
        if (cache.size() + quarantined.size() + pending.size() >= maxCache) { blocker = "task_cache_backpressure"; return; }
        auto plan = TaskWrite(task, expected, NewId(), code);
        pending.push_back({std::move(task), std::move(plan), ""});
    }
    void HoldNativeSave(uint32_t actor, bool hold) {
        auto next = std::make_shared<std::set<uint32_t>>(*std::atomic_load_explicit(&nativeSaveHolds, std::memory_order_acquire));
        if (hold) next->insert(actor); else next->erase(actor);
        std::shared_ptr<const std::set<uint32_t>> immutable = std::move(next);
        std::atomic_store_explicit(&nativeSaveHolds, std::move(immutable), std::memory_order_release);
    }
    void Flush(std::chrono::steady_clock::time_point deadline) {
        const auto now = NowMs();
        const auto native = std::find_if(pending.begin(),pending.end(),[&](const Pending& write) {
            return write.nativeSave && write.retry.dueAtMs <= now;
        });
        unsigned maximum;
        if (native != pending.end()) { std::rotate(pending.begin(),native,std::next(native)); maximum=1; }
        else maximum=PrepareReceiptBatch(pending,batch,now,preferReceiptRetry);
        if (!maximum) return;
        const bool nativeBatch = pending.front().nativeSave != nullptr;
        if (nativeBatch) {
            if (!pending.front().nativeSave->Queue(CharacterDatabase)) {
                pending.front().retry.Missed(now); blocker="native_save_queue_unavailable"; return;
            }
        } else {
            // A retained native body must NEVER fall through to journal-only retry.
            for (unsigned i=1;i<maximum;++i) if (pending[i].nativeSave) { maximum=i; break; }
            if (!CharacterDatabase.BeginTransaction()) return;
        }
        preferReceiptRetry = !pending.front().retry.failures; // Alternate fresh batches and due failed writes.
        unsigned count = 0;
        std::string query;
        for (; count < maximum;) {
            if (!nativeBatch)
                for (const auto& sql : pending[count].plan.statements) CharacterDatabase.Execute(sql.c_str());
            if (!query.empty()) query += " UNION ALL ";
            query += pending[count].plan.receiptQuery;
            ++count;
            if (std::chrono::steady_clock::now() >= deadline) break;
        }
        // One ordered native DB transaction followed by its receipt query. No
        // synchronous DB query or extra worker on the world thread.
        if (!nativeBatch && !CharacterDatabase.CommitTransaction()) { CharacterDatabase.RollbackTransaction(); return; }
        query += " UNION ALL SELECT '',0"; // Healthy empty acknowledgement differs from query failure.
#ifdef LIVING_ISOLATED_NATIVE_TESTS
        if (nativeBatch && pending.front().operation == operationFixtureRequest.transition.receipt &&
            operationFixtureRequest.kind == "isolated_wallet_consumption") {
            query += " UNION ALL SELECT 'isolated_native_wallet',money FROM characters WHERE guid=" +
                std::to_string(pending.front().task.actor);
            query += " UNION ALL SELECT 'isolated_claim_revision',revision FROM living_activity_claim WHERE claim_id=" +
                SqlValue(reservationFixtureRequest.changes.front().after.id);
            query += " UNION ALL SELECT 'isolated_native_operation',CASE WHEN state='intent' THEN 1 WHEN state='verified' THEN 2 ELSE 0 END "
                "FROM living_activity_operation WHERE operation_id=" + SqlValue(pending.front().operation);
        }
#endif
        ioPending = true;
        const auto token = epoch;
        if (!CharacterDatabase.AsyncQuery([this, count, token](QueryResult* result) {
            if (token != epoch) return;
            ioPending = false;
            ReceiptSet receipts;
            if (result) do { auto* f = result->Fetch(); receipts.emplace(f[0].GetCppString(), f[1].GetUInt64()); }
                while (result->NextRow());
            const bool healthy = receipts.count({"",0}) != 0;
#ifdef LIVING_ISOLATED_NATIVE_TESTS
            if (count == 1 && !pending.empty() && pending.front().nativeSave &&
                pending.front().operation == operationFixtureRequest.transition.receipt &&
                operationFixtureRequest.kind == "isolated_wallet_consumption") {
                const auto& write = pending.front();
                const auto status = write.nativeSave->Status();
                nativeSaveFixtureAttempts = write.nativeSave->Attempts();
                const bool committed = status == NativeSaveStatus::Committed || status == NativeSaveStatus::AlreadyCommitted;
                const bool rollback = status == NativeSaveStatus::NativeProofRejected || status == NativeSaveStatus::JournalProofRejected;
                boost::property_tree::ptree check;
                check.put("attempt",nativeSaveFixtureAttempts); check.put("status",Name(status));
                check.put("observed_at_ms",NowMs());
                const bool correct = healthy && (committed || rollback) &&
                    receipts.count({"isolated_native_wallet",gameplayOriginalMoney-(committed ? 1 : 0)}) &&
                    receipts.count({"isolated_claim_revision",committed ? 2 : 1}) &&
                    receipts.count({"isolated_native_operation",committed ? 2 : 1}) &&
                    (bool(receipts.count({write.plan.task,write.plan.revision})) == committed);
                check.put("native_money_claim_operation_and_receipt_agree",correct);
                nativeSaveFixtureChecks.push_back({"",check});
                if (status == NativeSaveStatus::Committed && !nativeSaveFixtureLostAck) {
                    // Lose only this test acknowledgement AFTER an actual native
                    // commit. The subsequent attempt must reconcile, not replay.
                    nativeSaveFixtureLostAck = true;
                    receipts.erase({write.plan.task,write.plan.revision});
                }
            }
#endif
            const auto accepted = SettleReceiptBatch(pending,count,healthy,receipts,NowMs(),[this](const Pending& acknowledgedWrite) {
                bool claimProjectionValid=true;
                if (!acknowledgedWrite.gainReservation.empty()) {
                    const auto installed=resources.CommitReservation(acknowledgedWrite.gainReservation);
                    if (installed != ClaimInstall::Installed && installed != ClaimInstall::Duplicate) {
                        resources.BlockProjection(); claimRestoreFailed=true; claimProjectionValid=false;
                        claimBlocker="acquired_claim_projection_mismatch"; ++invalidClaims;
                    }
                }
                if (!acknowledgedWrite.reservation.empty()) {
                    const auto installed = resources.CommitReservation(acknowledgedWrite.reservation);
                    if (installed != ClaimInstall::Installed && installed != ClaimInstall::Duplicate) {
                        resources.BlockProjection(); claimRestoreFailed = true; claimProjectionValid=false;
                        claimBlocker = "acknowledged_claim_projection_mismatch"; ++invalidClaims;
                    } else {
                        auto& ids = reservationClaimIds[acknowledgedWrite.task.id]; ids.clear();
                        for (const auto& change : acknowledgedWrite.claims) ids.push_back(change.after.id);
                    }
                } else {
                    reservationClaimIds.erase(acknowledgedWrite.task.id);
                    if (!acknowledgedWrite.claims.empty()) {
                        // New gained claims were installed from their pending
                        // hold above; settle the input claims from this receipt.
                        std::vector<ClaimReceiptChange> input;
                        for (const auto& change : acknowledgedWrite.claims)
                            if (acknowledgedWrite.gainReservation.empty() || change.expectedRevision) input.push_back(change);
                        const auto installed=input.empty() ? ClaimInstall::Duplicate : resources.InstallReceipt(input);
                        if (installed != ClaimInstall::Installed && installed != ClaimInstall::Duplicate) {
                            resources.BlockProjection(); claimRestoreFailed=true; claimProjectionValid=false;
                            claimBlocker="consumed_claim_projection_mismatch"; ++invalidClaims;
                        }
                    }
                }
                Remember(acknowledgedWrite.task);
                if (!acknowledgedWrite.admissionReceipt.empty())
                    admissionReceipts[acknowledgedWrite.task.id] = acknowledgedWrite.admissionReceipt;
                else admissionReceipts.erase(acknowledgedWrite.task.id);
                if (!acknowledgedWrite.operation.empty()) {
                    auto operation = operations.find(acknowledgedWrite.operation);
                    MANGOS_ASSERT(operation != operations.end());
                    if (!acknowledgedWrite.operationOutcome) operation->second.ready = true;
                    else {
                        const auto held = operation->second.held;
                        if (!claimProjectionValid) operation->second.saveBlocked=operation->second.uncertain=true;
                        if (!operation->second.saveBlocked) {
                            authority.FinishAtomic(held, operation->first);
                            authority.Release(held);
                            HoldNativeSave(held.actor,false);
                        }
                        const auto binding = bindings.find(held.actor);
                        if (binding != bindings.end()) binding->second.publisher.Publish(authority.Read(held.actor));
                        ++nativeOutcomes;
                        if (operation->second.outcome == OperationState::Verified) ++nativeVerifiedResults;
                        // Uncertainty remains visible and cannot be replayed.
                        // A domain reconciler must resolve the native references.
                        if (operation->second.uncertain) operation->second.ready = false;
                        else operations.erase(operation);
                    }
                }
                ++acknowledged; ++transitionCount;
            });
            if (accepted != count) {
                ++persistenceFailures;
                blocker = healthy ? "actor_journal_receipt_pending" : "journal_ack_query_failed";
                for (const auto& write : pending) if (write.nativeSave) {
                    blocker=Name(write.nativeSave->Status()); break;
                }
            } else blocker.clear();
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
            "(table_name='living_activity_claim' AND column_name IN ('claim_id','task_id','actor_guid','item_guid','item_entry',"
            "'quantity','copper','location','native_reference','state','revision')))),"
            "(SELECT COUNT(*) FROM living_activity_transition) FROM living_activity_schema WHERE version=1";
        if (!CharacterDatabase.AsyncQuery([this](QueryResult* result) {
            ioPending = false;
            schemaReady = result && result->Fetch()[0].GetUInt32() == 1 && result->Fetch()[1].GetUInt32() == 23;
            purchaseLedgerReady.store(schemaReady,std::memory_order_release);
            if (schemaReady) transitionCount = result->Fetch()[2].GetUInt64();
            blocker = schemaReady ? "startup_reconciliation" : "activity_schema_unavailable";
            nextWork = NowMs() + (schemaReady ? 1000 : 60000);
        }, sql.c_str())) { ioPending = false; nextWork = NowMs() + 60000; }
    }
    bool QueryPurchaseBudget(uint64_t now) {
        for (auto it=purchaseBudgets.begin();it!=purchaseBudgets.end();) {
            if (!it->second.pending && now>it->second.requestedAt+30000) it=purchaseBudgets.erase(it);
            else ++it;
        }
        for (auto& row : purchaseBudgets) {
            auto& read=row.second;
            if (read.pending || !read.dueAt || read.dueAt>now) continue;
            const auto generation=NativePurchaseEpoch().Read(row.first);
            if (!generation) { read.dueAt=now+1000; continue; }
            const auto actor=row.first;
            const auto revision=read.revision;
            const auto task=read.task, operation=read.operation;
            const auto query=PurchaseSpendQuery(actor,now,operation);
            read.pending=true; ioPending=true; ++budgetReads;
            if (!CharacterDatabase.AsyncQuery([this,actor,revision,task,operation,generation](QueryResult* result) {
                ioPending=false;
                const auto found=purchaseBudgets.find(actor);
                if (found==purchaseBudgets.end()) return;
                auto& read=found->second; read.pending=false;
                if (read.revision!=revision || read.task!=task || read.operation!=operation) return;
                read.spend={}; read.generation=generation; read.receivedAt=NowMs();
                bool valid=false;
                if (result && result->GetFieldCount()==4) {
                    auto* fields=result->Fetch();
                    valid=DecodePurchaseSpend({fields[0].GetCppString(),fields[1].GetCppString(),
                        fields[2].GetCppString(),fields[3].GetCppString()},read.spend);
                }
                if (generation!=NativePurchaseEpoch().Read(actor)) {
                    read.spend={}; read.blocker="purchase_budget_changed_during_read"; read.dueAt=NowMs()+1000;
                } else if (!valid) {
                    ++budgetFailures; read.blocker="purchase_budget_ledger_unavailable"; read.dueAt=NowMs()+5000;
                } else { read.blocker.clear(); read.dueAt=0; }
            },query.c_str())) {
                ioPending=false; read.pending=false; read.dueAt=now+5000;
                read.blocker="purchase_budget_queue_unavailable"; ++budgetFailures;
            }
            return true;
        }
        return false;
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
    void LoadClaims() {
        const auto sql = "SELECT * FROM (SELECT c.claim_id," + PersistedClaimProjection() +
            " payload,t.actor_guid task_actor,t.phase task_phase FROM living_activity_claim c "
            "JOIN living_activity_task t ON t.task_id=c.task_id "
            "WHERE c.state NOT IN ('released','consumed') AND c.claim_id>" + SqlValue(claimCursor) +
            " ORDER BY c.claim_id LIMIT " + std::to_string(loadBatch) +
            ") records UNION ALL SELECT '','{}',0,''";
        ioPending = true;
        if (!CharacterDatabase.AsyncQuery([this](QueryResult* result) {
            ioPending = false;
            if (!result) { claimBlocker = "claim_load_query_failed"; nextWork = NowMs()+5000; return; }
            unsigned count = 0;
            do {
                auto* f = result->Fetch(); const auto id = f[0].GetCppString();
                if (id.empty()) continue;
                incomingClaims.push_back({id,f[1].GetCppString(),f[2].GetUInt32(),f[3].GetCppString()}); ++count;
            } while (result->NextRow());
            claimsEnumerated = count < loadBatch;
            if (!count && !claimRestoreFailed) { resources.FinishRestore(); claimBlocker.clear(); }
        },sql.c_str())) { ioPending = false; claimBlocker = "claim_load_queue_failed"; nextWork = NowMs()+5000; }
    }
    void DecodeClaims(std::chrono::steady_clock::time_point deadline) {
        do {
            const auto& row = incomingClaims.front(); ResourceClaim claim; std::string reason;
            Phase ownerPhase = Phase::Reconciling;
            bool valid = DecodeClaimProjection(row.payload,claim,reason);
            if (valid && (claim.id != row.id || claim.actor != row.taskActor ||
                !ParsePhase(row.taskPhase,ownerPhase) || Terminal(ownerPhase))) {
                valid = false; reason = "claim_owner_requires_reconciliation";
            }
            if (valid) {
                const auto installed = resources.RestoreBatch({claim});
                if (installed != ClaimInstall::Installed && installed != ClaimInstall::Duplicate) {
                    valid = false; reason = installed == ClaimInstall::Capacity ? "claim_cache_capacity" : "claim_cache_restore_failed";
                }
            }
            if (!valid) {
                ++invalidClaims; claimRestoreFailed = true; claimBlocker = reason;
                // Retain the native row. Observation/gameplay can continue, but
                // this incomplete projection cannot authorize resource work.
                sLog.outError("Living activity claim restore blocked: claim=%s reason=%s",row.id.c_str(),reason.c_str());
            }
            claimCursor = row.id; incomingClaims.pop_front();
        } while (!incomingClaims.empty() && std::chrono::steady_clock::now() < deadline);
        if (incomingClaims.empty() && claimsEnumerated && !claimRestoreFailed) {
            resources.FinishRestore(); claimBlocker.clear();
        }
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
#include "../tests/realm/ActivityGameplayFixture.inc"
#include "../tests/realm/ActivityCombatFixture.inc"
#include "../tests/realm/ActivityPetFixture.inc"
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
    if (state->effective != Mode::Off && !state->ioPending && !state->incomingClaims.empty()) {
        state->DecodeClaims(deadline); return;
    }
    ObservationQueue queues;
    queues.enabled = state->effective != Mode::Off; queues.ioPending = state->ioPending;
    queues.due = now >= state->nextWork; queues.schemaReady = state->schemaReady; queues.loaded = state->loaded;
    queues.cached = state->cache.size() + state->quarantined.size() + state->pending.size();
    queues.pending = std::count_if(state->pending.begin(),state->pending.end(),[now](const State::Pending& p){return p.retry.dueAtMs <= now;});
    queues.incoming = state->incoming.size();
    queues.cacheLimit = state->maxCache; queues.retained = state->transitionCount;
    const auto work = NextObservationWork(queues);
    // Native receipt verification stays ahead of speculative budget reads.
    // Reads run only when requested, using the same bounded DB queue as tasks.
    if (state->effective!=Mode::Off && !state->ioPending && state->schemaReady && state->loaded &&
        state->claimsEnumerated && state->incoming.empty() && state->incomingClaims.empty() &&
        !queues.pending && state->QueryPurchaseBudget(now)) return;
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
        case ObservationWork::Import:
            if (!state->claimsEnumerated) state->LoadClaims();
            else state->Import();
            break;
        case ObservationWork::HistoryPressure: state->blocker = "transition_outbox_backpressure"; break;
        case ObservationWork::CachePressure: state->blocker = "task_cache_backpressure"; break;
        default: break;
    }
}
std::string LivingActivityCoordinator::StatusJson() const {
    boost::property_tree::ptree p;
    p.put("contract_version", 1); p.put("desired_mode", state->desired); p.put("effective_mode", Name(state->effective));
    p.put("boot",state->boot);
    p.put("policy_revision", state->policyRevision); p.put("blocker", state->blocker);
    p.put("cached_tasks", state->cache.size()); p.put("pending_writes", state->pending.size());
    p.put("receipt_count", state->acknowledged); p.put("persistence_failures", state->persistenceFailures);
    p.put("retained_transitions", state->transitionCount);
    p.put("pending_decode", state->incoming.size()); p.put("maximum_dispatch_us", state->maximumDispatchUs);
    p.put("over_budget_updates", state->overBudgetUpdates); p.put("next_import_family", state->importFamily);
    p.put("invalid_records", state->invalidRecords); p.put("gameplay_mutations", state->nativeVerifiedResults);
    p.put("native_dispatches", state->nativeDispatches); p.put("native_result_receipts", state->nativeOutcomes);
    p.put("verified_native_results", state->nativeVerifiedResults);
    p.put("pending_native_operations", state->operations.size());
    p.put("purchase_budget_reads",state->budgetReads); p.put("purchase_budget_failures",state->budgetFailures);
    p.put("purchase_budget_cache",state->purchaseBudgets.size());
    p.put("unacknowledged_writes",std::count_if(state->pending.begin(),state->pending.end(),
        [](const State::Pending& write){return write.retry.failures != 0;}));
    p.put("cached_resource_claims", state->resources.Size());
    p.put("pending_resource_reservations",state->resources.PendingCount());
    p.put("resource_claims_ready", state->resources.Protection().ready);
    p.put("pending_claim_decode", state->incomingClaims.size());
    p.put("invalid_resource_claims", state->invalidClaims);
    p.put("resource_claim_blocker", state->claimBlocker);
    p.put("observed_actions", state->observedActions); p.put("unknown_effect_actions", state->unknownActions);
    p.put("optional_action_observations_rejected", state->actionInbox.Rejected());
    p.put("action_cardinality_rejected", state->actionCardinalityRejected);
    p.put("native_views_published", state->nativeViewsPublished);
    p.put("stale_actor_observations", state->staleActorObservations);
    p.put("task_admission_writes", state->taskAdmissions); p.put("saved_task_grants", state->savedGrants);
    p.put("execution_enforcement", state->enforceEffects.load(std::memory_order_acquire));
    p.put("compatibility_lease_index",state->compatibilityActors.size());
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
    if (OnWorldThread()) {
        if (Player* bot = sRandomPlayerbotMgr.GetPlayerBot(guid)) {
            const auto roster = NativePartyProtection(*bot);
            p.put("party_commitment.roster", Name(roster));
            p.put("party_commitment.saved_executor_blocker", PartyAdmissionBlocker(roster, PartyAdmission::SavedExecutor, false));
            p.put("party_commitment.safe_service_window", sPlayerbotRendezvousManager.HasSafePartyServiceWindow(bot));
        }
    }
    const auto lease=state->authority.Read(guid);
    if(lease.lease.actor) {
        const uint64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        p.put("execution_lease.source",lease.compatibility?"legacy_compatibility":"managed_task");
        p.put("execution_lease.root_task_id",lease.lease.rootTask);
        p.put("execution_lease.owner_generation",lease.lease.generation);
        p.put("execution_lease.domain",lease.root.source);
        p.put("execution_lease.phase",lease.compatibility?lease.compatibilityPhase:Name(lease.root.phase));
        p.put("execution_lease.reason",lease.compatibility?lease.compatibilityReason:lease.root.checkpoint.blocker);
        p.put("execution_lease.state",Name(state->authority.Inspect(guid,now).code));
        p.put("execution_lease.remaining_ms",lease.expires>now?lease.expires-now:0);
        p.put("execution_lease.operation",lease.operation);
    }
    p.put("native_save_deferred", DefersNativeSave(guid));
    for (const auto& write : state->pending) if (write.task.actor == guid && (write.nativeSave || write.retry.failures)) {
        p.put("journal.task",write.task.id); p.put("journal.revision",write.task.revision);
        p.put("journal.blocker",write.nativeSave ? Name(write.nativeSave->Status()) : "exact_receipt_not_verified");
        p.put("journal.attempts",write.retry.failures);
        p.put("journal.retry_at_ms",write.retry.dueAtMs); p.put("journal.native_outcome_retained",write.operationOutcome);
        if (write.nativeSave) {
            p.put("journal.native_save_attempts",write.nativeSave->Attempts());
            p.put("journal.native_save_in_flight",write.nativeSave->InFlight());
        }
        break;
    }
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
    for (const auto& operation : state->operations) {
        const auto& pending = operation.second;
        if (pending.request.transition.task.actor != guid) continue;
        p.put("native_operation.id", operation.first);
        p.put("native_operation.task", pending.request.transition.task.id);
        p.put("native_operation.kind", pending.request.kind);
        p.put("native_operation.save_capture_blocked", pending.saveBlocked);
        p.put("native_operation.phase", pending.uncertain ? "reconciliation_required" : pending.dispatched ?
            "result_receipt_pending" : pending.ready ? "ready_for_native_validation" : "intent_receipt_pending");
    }
    return Json(p);
}

void LivingActivityCoordinator::ObserveAction(PlayerbotAI& ai, const Effects& effects, const std::string& action) {
    PermitEffects(ai, effects, action);
}

NativePermit LivingActivityCoordinator::NativeActionContext(PlayerbotAI& ai, Lane lane,
    uint32_t effects, uint32_t allowedSafety) const {
    NativePermit result;
    if (!state->enforceEffects.load(std::memory_order_acquire) && !state->observeEffects.load(std::memory_order_acquire)) return result;
    Player* bot = ai.GetBot();
    const auto view = ai.activityPermissions.Inspect();
    if (!bot || !view || !effects || (effects & ~AllEffects) || lane == Lane::Managed || lane == Lane::Inspection) return result;
    const auto current = ReadNativeContext(*bot, state->publishedPolicyRevision.load(std::memory_order_acquire), state->boot);
    if (!(current == view->current) || !current.actorGeneration || !current.mapGeneration) return result;
    result.world = current; result.lane = lane; result.effects = effects; result.allowedSafety = allowedSafety;
    // Eligibility is deliberately NOT asserted here. The caller's native
    // operation validator must set validated only after inspecting its target.
    return result;
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
    const auto current = ReadNativeContext(*bot, std::max<uint64_t>(1,state->policyRevision), state->boot);
    if (!current.mapGeneration) { entry.publisher.Revoke(); state->authority.Forget(guid); return; }
    const uint32_t safety = NativeSafety(bot);
    const auto prior = ai->activityPermissions.Inspect();
    if (prior && prior->current == current && prior->safety == safety) return;
    const auto observed = state->authority.Observe(current, safety);
    if(!state->authority.Read(guid).compatibility) state->compatibilityActors.erase(guid);
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
    const auto current = ReadNativeContext(*bot, std::max<uint64_t>(1,state->policyRevision), state->boot);
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

ResourceReader LivingActivityCoordinator::ResourceReservations() const { return state->resources.Reader(); }
bool LivingActivityCoordinator::PurchaseLedgerReady() const {
    return state->purchaseLedgerReady.load(std::memory_order_acquire);
}
bool LivingActivityCoordinator::ReadPurchaseBudget(uint32_t actor,const std::string& task,uint64_t revision,
    const std::string& operation,PurchaseSpend& spend,std::string& blocker) {
    spend={};
    auto reject=[&](const char* why){blocker=why; return false;};
    if (!OnWorldThread() || !PurchaseLedgerReady() || state->effective==Mode::Off)
        return reject("purchase_budget_coordinator_unavailable");
    const auto saved=state->cache.find(task);
    if (!actor || !IsUuid(operation) || saved==state->cache.end() || saved->second.actor!=actor ||
        saved->second.revision!=revision || Terminal(saved->second.phase))
        return reject("purchase_budget_task_changed");
    const auto now=NowMs(), generation=NativePurchaseEpoch().Read(actor);
    if (!generation) return reject("purchase_budget_native_mutation_pending");
    auto found=state->purchaseBudgets.find(actor);
    if (found==state->purchaseBudgets.end()) {
        if (state->purchaseBudgets.size()>=64) {
            for (auto it=state->purchaseBudgets.begin();it!=state->purchaseBudgets.end();) {
                if (!it->second.pending && now>it->second.receivedAt+30000) it=state->purchaseBudgets.erase(it);
                else ++it;
            }
            if (state->purchaseBudgets.size()>=64) return reject("purchase_budget_queue_full");
        }
        found=state->purchaseBudgets.emplace(actor,State::BudgetRead{}).first;
    }
    auto& read=found->second;
    if (read.pending) return reject("purchase_budget_read_pending");
    if (read.task!=task || read.revision!=revision || read.operation!=operation || read.generation!=generation ||
        (read.receivedAt && (now<read.receivedAt || now-read.receivedAt>15000))) {
        read={}; read.task=task; read.revision=revision; read.operation=operation;
        read.generation=generation; read.dueAt=now;
    }
    read.requestedAt=now;
    if (!read.spend.complete) { blocker=read.blocker; return false; }
    spend=read.spend; blocker.clear(); return true;
}

bool LivingActivityCoordinator::EffectEnforcementEnabled() const {
    return state->enforceEffects.load(std::memory_order_acquire);
}

bool LivingActivityCoordinator::DefersNativeSave(uint32_t actor) const {
    const auto held = std::atomic_load_explicit(&state->nativeSaveHolds, std::memory_order_acquire);
    return actor && held && held->count(actor);
}

Acquisition LivingActivityCoordinator::AcquireCompatibilityLease(uint32_t actor,const std::string& owner,
    const std::string& phase,uint32_t ttlSeconds,const std::string& reason,const std::string& jobKey,ActivityLease& handle) {
    if(!OnWorldThread()) return {AcquisitionState::Invalidated,"world_thread_required"};
    if(!IsSourceKey(jobKey) || !IsToken(phase) || !IsToken(reason,128,true) || !ttlSeconds || ttlSeconds>600)
        return {AcquisitionState::Invalidated,"invalid_compatibility_request"};
    Kind kind;Priority priority;
    if(owner=="player_command") {kind=Kind::HumanRequest;priority=Priority::Human;}
    else if(owner=="guild_supply") {kind=Kind::GuildDelivery;priority=Priority::Delivery;}
    else if(owner=="economy_service") {kind=Kind::Maintenance;priority=Priority::Delivery;}
    else return {AcquisitionState::Invalidated,"unsupported_compatibility_owner"};
    Player* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
    if(!bot || !bot->GetPlayerbotAI()) return {AcquisitionState::Invalidated,"native_context_unavailable"};
    RefreshPermission(actor,bot->GetPlayerbotAI()->GetActivityActorEpoch());
    const auto admission = owner == "player_command" ? PartyAdmission::ValidatedHumanCompatibility : PartyAdmission::ServiceCompatibility;
    const char* partyBlocker = PartyAdmissionBlocker(NativePartyProtection(*bot), admission,
        sPlayerbotRendezvousManager.HasSafePartyServiceWindow(bot));
    if (*partyBlocker) return {AcquisitionState::Waiting, partyBlocker};
    const auto before=state->authority.Read(actor);
    const uint64_t monotonic=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    Task task;
    task.id=task.root=SourceId(owner,jobKey);task.source=owner;task.sourceKey=jobKey;
    task.actor=actor;task.context=before.current;task.kind=kind;task.priority=priority;
    task.mode=Mode::Active;task.phase=Phase::Traveling;task.checkpoint.step="legacy_movement";
    task.createdAtMs=task.updatedAtMs=NowMs();
    ActivityLease identity{actor,task.id,0,task.context};
    if(before.lease.actor && before.expires>monotonic && before.compatibility && before.root.source==owner &&
        !MayAcquireCompatibilityLease(before.lease,handle,identity,true))
        return {AcquisitionState::Waiting,"another_committed_activity"};
    if(before.compatibility && before.root.id==task.id && before.root.context==task.context)
        task=before.root; // Renewal does not recreate the accepted legacy identity.
    const auto result=state->authority.AcquireCompatibility(task,monotonic,uint64_t(ttlSeconds)*1000);
    if(!result.Granted()) return AcquisitionFrom(result.code);
    state->authority.DescribeCompatibility(result.lease,phase,reason);
    handle=result.lease;state->compatibilityActors.insert(actor);
    state->bindings.at(actor).publisher.Publish(state->authority.Read(actor));
    return AcquisitionFrom(result.code);
}

bool LivingActivityCoordinator::RenewCompatibilityLease(const ActivityLease& handle,const std::string& phase,
    uint32_t ttlSeconds,const std::string& reason) {
    if(!OnWorldThread()) return false;
    Player* bot=sRandomPlayerbotMgr.GetPlayerBot(handle.actor);
    if(!bot || !bot->GetPlayerbotAI()) return false;
    RefreshPermission(handle.actor,bot->GetPlayerbotAI()->GetActivityActorEpoch());
    const auto before=state->authority.Read(handle.actor);
    const uint64_t now=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if(!before.compatibility || !SameLease(before.lease,handle) || before.expires<=now) return false;
    auto renewed=handle;
    const auto result=AcquireCompatibilityLease(handle.actor,before.root.source,phase,ttlSeconds,reason,before.root.sourceKey,renewed);
    // Expired/new-context callbacks may never replace a caller's immutable handle.
    if(result.Permitted() && !SameLease(renewed,handle)) {ReleaseCompatibilityLease(renewed);return false;}
    return result.Permitted();
}

bool LivingActivityCoordinator::ReleaseCompatibilityLease(const ActivityLease& handle) {
    if(!OnWorldThread()) return false;
    const auto before=state->authority.Read(handle.actor);
    if(!before.compatibility || !SameLease(before.lease,handle)) return false;
    return ReleaseTaskLease(handle).code==AuthorityCode::Released;
}

std::optional<LivingActivityCoordinator::CompatibilityLease> LivingActivityCoordinator::ReadCompatibilityLease(uint32_t actor) const {
    // Native callers already hold the actor lifecycle. Read only its published
    // immutable view here; no map worker reads the world's mutable lease book.
    Player* bot=sRandomPlayerbotMgr.GetPlayerBot(actor);
    if(!bot || !bot->GetPlayerbotAI()) return {};
    const auto view=bot->GetPlayerbotAI()->activityPermissions.Inspect();
    if(!view || !view->compatibility || !view->lease.actor ||
        view->current.actorGeneration!=bot->GetPlayerbotAI()->GetActivityActorEpoch() ||
        view->current.mapGeneration!=bot->GetPlayerbotAI()->GetActivityMapEpoch()) return {};
    return CompatibilityLease{view->lease,view->root.source,view->compatibilityPhase,view->compatibilityReason,view->expires};
}

std::vector<LivingActivityCoordinator::CompatibilityLease> LivingActivityCoordinator::CompatibilityLeases() const {
    std::vector<CompatibilityLease> result;
    if(!OnWorldThread()) return result;
    for(uint32_t actor:state->compatibilityActors) {
        const auto view=state->authority.Read(actor);
        if(view.compatibility && view.lease.actor)
            result.push_back({view.lease,view.root.source,view.compatibilityPhase,view.compatibilityReason,view.expires});
    }
    return result;
}

AdmissionResult LivingActivityCoordinator::SubmitResourceReservation(const ReservationRequest& original,
    NativeReservationAdapter& adapter) {
    if (original.changes.empty() || original.changes.size() > 16) {
        AdmissionResult result; result.blocker="invalid_reservation_batch"; return result;
    }
    ReservationRequest request=original;
    std::sort(request.changes.begin(),request.changes.end(),[](const auto& a,const auto& b){return a.after.id < b.after.id;});
    const auto& next=request.transition.task;
    AdmissionResult result; result.task=next.id; result.revision=next.revision;
    auto reject=[&](AdmissionCode code,const std::string& reason="") {
        result.code=code; result.blocker=reason.empty() ? Name(code) : reason; return result;
    };
    if (!OnWorldThread() || !state->enforceEffects.load(std::memory_order_acquire)) return reject(AdmissionCode::Disabled);
    if (!state->schemaReady || !state->loaded || !state->resources.Protection().ready || !state->incoming.empty())
        return reject(AdmissionCode::NotReady);
    if (state->operationDispatching) return reject(AdmissionCode::Backpressure);
    if (next.id != SourceId(next.source,next.sourceKey) || next.mode != Mode::Active || next.phase != Phase::Preparing ||
        request.changes.empty() || request.changes.size() > 16) return reject(AdmissionCode::InvalidRequest);
    Player* bot=sRandomPlayerbotMgr.GetPlayerBot(next.actor);
    if (!bot || !bot->GetPlayerbotAI()) return reject(AdmissionCode::StaleContext);
    const auto current=ReadNativeContext(*bot,state->policyRevision,state->boot);
    if (!(next.context == current)) return reject(AdmissionCode::StaleContext);
    auto sameTask=[&](const Task& task) {
        try { return SameRequest(TaskWrite(task,request.transition.expectedRevision,request.transition.receipt,"resources_reserved"),
            TaskWrite(next,request.transition.expectedRevision,request.transition.receipt,"resources_reserved")); }
        catch (const std::exception&) { return false; }
    };
    for (const auto& queued : state->pending) {
        if (queued.reservation == request.transition.receipt && queued.task.id == next.id && sameTask(queued.task) &&
            queued.claims.size() == request.changes.size()) {
            for (size_t i=0;i<request.changes.size();++i)
                if (queued.claims[i].expectedRevision != request.changes[i].expectedRevision ||
                    !SameResourceClaim(queued.claims[i].after,request.changes[i].after)) return reject(AdmissionCode::ConflictingWrite);
            return reject(AdmissionCode::Pending);
        }
        if (queued.task.actor == next.actor || queued.admissionReceipt == request.transition.receipt)
            return reject(AdmissionCode::ConflictingWrite);
    }
    const auto saved=state->cache.find(next.id), root=state->cache.find(next.root);
    const auto receipt=state->admissionReceipts.find(next.id);
    const auto ids=state->reservationClaimIds.find(next.id);
    if (saved != state->cache.end() && receipt != state->admissionReceipts.end() &&
        receipt->second == request.transition.receipt && ids != state->reservationClaimIds.end() &&
        ids->second.size() == request.changes.size() && sameTask(saved->second)) {
        for (size_t i=0;i<request.changes.size();++i) {
            const auto& change=request.changes[i]; const auto* claim=state->resources.Inspect(change.after.id);
            if (ids->second[i] != change.after.id || !claim || change.after.revision != change.expectedRevision+1 ||
                !SameResourceClaim(*claim,change.after)) return reject(AdmissionCode::ConflictingWrite);
        }
        return reject(AdmissionCode::Saved);
    }
    std::string blocker;
    auto valid=ValidateTaskRequest(request.transition,saved == state->cache.end() ? nullptr : &saved->second,current,blocker,
        root == state->cache.end() ? nullptr : &root->second);
    if (valid != AdmissionCode::Pending) return reject(valid,blocker);
    if (saved == state->cache.end()) return reject(AdmissionCode::NotReady,"saved_predecessor_required");
    if (state->pending.size() >= state->batch || state->transitionCount+state->pending.size() >= 200000)
        return reject(AdmissionCode::Backpressure);
    for (const auto& operation : state->operations) if (operation.second.request.transition.task.actor == next.actor)
        return reject(AdmissionCode::ReconciliationRequired,"actor_operation_unresolved");
    RefreshPermission(next.actor,bot->GetPlayerbotAI()->GetActivityActorEpoch());
    auto predecessor=saved->second; predecessor.ownerGeneration=request.authorization.ownerGeneration;
    const uint64_t monotonic=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    uint32_t required=0;
    for (const auto& change : request.changes) required |= change.after.copper ? Mask(Effect::Money) : Mask(Effect::Inventory);
    if ((required & ~request.authorization.permittedEffects) || (required & ~state->authority.Read(next.actor).effects))
        return reject(AdmissionCode::InvalidRequest,"reservation_effects_not_granted");
    if (state->authority.Authorize({0,Lane::Managed,true},current,monotonic,&predecessor,&request.authorization) != AuthorityCode::Allowed)
        return reject(AdmissionCode::StaleContext,"current_predecessor_lease_required");
    if (bot->GetTradeData()) return reject(AdmissionCode::InvalidRequest,"native_trade_in_progress");
    std::vector<NativeResourceBalance> balances;
    bool reservationStarted=false;
    try {
        if (!adapter.ValidatePurpose(*bot,request,blocker)) return reject(AdmissionCode::InvalidRequest,
            IsToken(blocker) ? blocker : "native_reservation_purpose_rejected");
        std::vector<ResourceClaim> claims;
        for (const auto& change : request.changes) claims.push_back(change.after);
        balances=NativeClaimBalances(*bot,claims);
        auto plan=ResourceReservationWrite(next,request.transition.expectedRevision,request.transition.receipt,request.changes,balances);
        State::Pending write{next,std::move(plan),request.transition.receipt};
        write.reservation=request.transition.receipt; write.claims=request.changes;
        reservationStarted=true;
        const auto reserved=state->resources.ReservePending(request.transition.receipt,request.changes,balances);
        if (reserved != ClaimInstall::Installed) return reject(reserved == ClaimInstall::Capacity ? AdmissionCode::Backpressure :
            AdmissionCode::InvalidRequest,"resources_unavailable_or_reserved");
        state->pending.push_back(std::move(write));
    } catch (const std::exception&) {
        // An allocation failure may interrupt indexing before the pending map
        // insertion. Never expose that partially updated projection as ready.
        if (reservationStarted) {
            state->resources.BlockProjection(); state->claimRestoreFailed=true;
            state->claimBlocker="reservation_admission_requires_reconciliation";
            return reject(AdmissionCode::ReconciliationRequired,state->claimBlocker);
        }
        return reject(AdmissionCode::InvalidRequest,"invalid_native_reservation");
    }
    ReleaseTaskLease(state->authority.Read(next.actor).lease);
    state->nextWork=0;
    return reject(AdmissionCode::Pending);
}

AdmissionResult LivingActivityCoordinator::SubmitTask(const TaskRequest& request) {
    AdmissionResult result; result.task = request.task.id; result.revision = request.task.revision;
    auto reject = [&](AdmissionCode code, const std::string& reason = "") {
        result.code = code; result.blocker = reason.empty() ? Name(code) : reason; return result;
    };
    if (!OnWorldThread()) return reject(AdmissionCode::InvalidRequest, "world_thread_required");
    if (!state->enforceEffects.load(std::memory_order_acquire)) return reject(AdmissionCode::Disabled);
    if (state->operationDispatching) return reject(AdmissionCode::Backpressure, "native_result_capacity_reserved");
    if (!state->schemaReady || !state->loaded || !state->incoming.empty()) return reject(AdmissionCode::NotReady);
    const Task& task = request.task;
    if (task.phase == Phase::Executing || task.phase == Phase::Verifying)
        return reject(AdmissionCode::ReconciliationRequired, "native_operation_journal_required");
    for (const auto& pending : state->operations)
        if (pending.second.request.transition.task.root == task.root)
            return reject(AdmissionCode::ReconciliationRequired, "native_operation_pending");
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
    // Check first admission only. Later cancellation/deferral/reconciliation
    // must remain possible if a recipe becomes obsolete or a subject changes.
    // Repeated requests share the saved receipt above, not a second job.
    if ((saved == state->cache.end() || (!saved->second.accepted && task.accepted)) &&
        !ValidateNativeProfessionTask(*bot, task, reason))
        return reject(AdmissionCode::InvalidRequest, reason);
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
    if ((effects & (Mask(Effect::Inventory)|Mask(Effect::Money))) && !state->resources.Protection().ready) {
        result.blocker="resource_protection_unavailable"; return result;
    }
    for (const auto& queued : state->pending) if (queued.task.root == id) {
        result.blocker = "task_write_pending"; return result;
    }
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(saved->second.actor);
    if (!bot || !bot->GetPlayerbotAI()) { result.blocker = "actor_not_available"; return result; }
    const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
    if (!SavedTaskExecutable(saved->second, revision, current, NowMs(), result.blocker)) return result;
    result.blocker = PartyAdmissionBlocker(NativePartyProtection(*bot), PartyAdmission::SavedExecutor, false);
    if (!result.blocker.empty()) return result;
    RefreshPermission(saved->second.actor, bot->GetPlayerbotAI()->GetActivityActorEpoch());
    const uint64_t monotonic = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    result.authority = state->authority.Acquire(saved->second, effects, monotonic, durationMs);
    if (!result.authority.Granted()) { result.blocker = Name(result.authority.code); return result; }
    state->compatibilityActors.erase(saved->second.actor);
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
    if(!state->authority.Read(lease.actor).compatibility) state->compatibilityActors.erase(lease.actor);
    const auto binding = state->bindings.find(lease.actor);
    if (binding != state->bindings.end()) binding->second.publisher.Publish(state->authority.Read(lease.actor));
    return result;
}

AdmissionResult LivingActivityCoordinator::SubmitOperationIntent(const OperationRequest& request,
    NativeOperationAdapter& adapter) {
    const auto& next = request.transition.task;
    AdmissionResult result; result.task = next.id; result.revision = next.revision;
    auto reject = [&](AdmissionCode code, const std::string& blocker = "") {
        result.code = code; result.blocker = blocker.empty() ? Name(code) : blocker; return result;
    };
    if (!OnWorldThread() || !state->enforceEffects.load(std::memory_order_acquire))
        return reject(AdmissionCode::Disabled);
    if (state->operationDispatching) return reject(AdmissionCode::Backpressure, "native_result_capacity_reserved");
    if (!state->schemaReady || !state->loaded || !state->incoming.empty()) return reject(AdmissionCode::NotReady);
    if (next.id != SourceId(next.source, next.sourceKey) || request.kind != adapter.OperationKind() ||
        request.effects != adapter.OperationEffects() || request.persistence != adapter.PersistencePolicy())
        return reject(AdmissionCode::InvalidRequest, "native_adapter_mismatch");
    if ((request.effects & (Mask(Effect::Money)|Mask(Effect::Inventory))) &&
        (!adapter.SupportsClaimedConsumption() || request.consumption.empty() || request.persistence == NativePersistence::JournalOnly))
        return reject(AdmissionCode::InvalidRequest,"resource_effect_adapter_not_supported");
    if (!request.itemGain.Empty() && (!adapter.SupportsItemGain() || !ValidItemGainSpec(request.itemGain)))
        return reject(AdmissionCode::InvalidRequest,"native_item_gain_adapter_not_supported");
    WritePlan plan;
    try { plan = OperationRequestWrite(request); }
    catch (const std::exception&) { return reject(AdmissionCode::InvalidRequest, "invalid_native_operation_intent"); }
    const auto prior = state->operations.find(request.transition.receipt);
    if (prior != state->operations.end()) {
        if (!SameRequest(plan, OperationRequestWrite(prior->second.request))) return reject(AdmissionCode::ConflictingWrite);
        if (prior->second.dispatched) return reject(AdmissionCode::ReconciliationRequired, "operation_already_dispatched");
        return reject(prior->second.ready ? AdmissionCode::Saved : AdmissionCode::Pending);
    }
    if (state->operations.size() >= 16 || state->pending.size() >= state->batch ||
        state->transitionCount + state->pending.size() + 2 > 200000) return reject(AdmissionCode::Backpressure);
    for (const auto& operation : state->operations)
        if (operation.second.request.transition.task.actor == next.actor)
            return reject(AdmissionCode::ReconciliationRequired, "actor_operation_unresolved");
    for (const auto& queued : state->pending)
        if (queued.task.root == next.root || queued.admissionReceipt == request.transition.receipt)
            return reject(AdmissionCode::ConflictingWrite);
    const auto saved = state->cache.find(next.id), root = state->cache.find(next.root);
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(next.actor);
    if (!bot || !bot->GetPlayerbotAI() || saved == state->cache.end()) return reject(AdmissionCode::NotReady);
    const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
    std::string blocker;
    if (!ValidateOperationRequest(request, saved->second, current, root == state->cache.end() ? nullptr : &root->second, NowMs(), blocker))
        return reject(AdmissionCode::InvalidRequest, blocker);
    RefreshPermission(next.actor, bot->GetPlayerbotAI()->GetActivityActorEpoch());
    auto predecessor = saved->second; predecessor.ownerGeneration = request.authorization.ownerGeneration;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    // Check scoped ownership without executing a resource effect before intent.
    if (state->authority.Authorize({0, Lane::Managed, true}, current, now, &predecessor, &request.authorization) != AuthorityCode::Allowed)
        return reject(AdmissionCode::StaleContext, "current_predecessor_lease_required");
    try {
        if (bot->GetTradeData() || !ValidateOperationResources(request,state->resources,NativeConsumptionBalances(*bot,request),blocker))
            return reject(AdmissionCode::InvalidRequest,bot->GetTradeData() ? "native_trade_in_progress" : blocker);
        if (!adapter.ValidateNative(*bot, request, blocker))
            return reject(AdmissionCode::InvalidRequest, IsToken(blocker) ? blocker : "native_prerequisite_unavailable");
    } catch (const std::exception&) { return reject(AdmissionCode::InvalidRequest, "native_validation_failed"); }
    state->operations.emplace(request.transition.receipt, State::PendingOperation{request});
    if (request.kind=="vendor_purchase") NativePurchaseEpoch().Changed(request.transition.task.actor);
    state->pending.push_back({next, std::move(plan), "", request.transition.receipt, false});
    const auto held = state->authority.Read(next.actor).lease;
    ReleaseTaskLease(held); // Intent is durable work, not a retained execution grant.
    state->nextWork = 0;
    return reject(AdmissionCode::Pending);
}

DispatchResult LivingActivityCoordinator::DispatchSavedOperation(const std::string& id,
    const TaskGrant& grant, NativeOperationAdapter& adapter) {
    DispatchResult result;
    auto reject = [&](AdmissionCode code, const std::string& blocker) {
        result.admission.code = code; result.admission.blocker = blocker; return result;
    };
    if (!OnWorldThread() || !state->enforceEffects.load(std::memory_order_acquire))
        return reject(AdmissionCode::Disabled, "execution_disabled");
    const auto found = state->operations.find(id);
    if (found == state->operations.end()) return reject(AdmissionCode::ReconciliationRequired, "no_fresh_intent_admission");
    auto& pending = found->second; const auto& request = pending.request;
    const auto& intended = request.transition.task;
    result.admission.task = intended.id; result.admission.revision = intended.revision;
    if (pending.dispatched) return reject(AdmissionCode::ReconciliationRequired, "operation_already_dispatched");
    if (!pending.ready) return reject(AdmissionCode::Pending, "intent_receipt_pending");
    // Reserve the existing result queue before any nonrepeatable effect. The
    // world dispatch is synchronous and rejects reentrant task admissions.
    if (state->operationDispatching || state->ioPending || state->pending.size() >= state->batch || !state->incoming.empty() ||
        state->transitionCount + state->pending.size() >= 200000)
        return reject(AdmissionCode::Backpressure, "native_result_capacity_unavailable");
    for (const auto& write : state->pending) if (write.task.actor == intended.actor)
        return reject(AdmissionCode::ReconciliationRequired,"actor_journal_write_pending");
    if (request.kind != adapter.OperationKind() || request.effects != adapter.OperationEffects() ||
        request.persistence != adapter.PersistencePolicy() ||
        (!request.itemGain.Empty() && !adapter.SupportsItemGain()) ||
        (!request.consumption.empty() && !adapter.SupportsClaimedConsumption()))
        return reject(AdmissionCode::InvalidRequest, "native_adapter_mismatch");
    const auto saved = state->cache.find(intended.id);
    Player* bot = sRandomPlayerbotMgr.GetPlayerBot(intended.actor);
    if (!bot || !bot->GetPlayerbotAI() || saved == state->cache.end())
        return reject(AdmissionCode::StaleContext, "actor_not_available");
    const auto current = ReadNativeContext(*bot, state->policyRevision, state->boot);
    std::string blocker;
    if (!SavedTaskExecutable(saved->second, intended.revision, current, NowMs(), blocker) ||
        saved->second.phase != Phase::Executing || !grant.Permitted() || grant.action.task != intended.id ||
        grant.action.revision != intended.revision || (request.effects & ~grant.action.permittedEffects))
        return reject(AdmissionCode::StaleRevision, "exact_executing_grant_required");
    RefreshPermission(intended.actor, bot->GetPlayerbotAI()->GetActivityActorEpoch());
    const auto held = state->authority.Read(intended.actor).lease;
    if (!SameLease(held, grant.authority.lease)) return reject(AdmissionCode::StaleContext, "current_operation_lease_required");
    // Reserve bounded result/projection capacity before the nonrepeatable call.
    // This world-thread dispatch cannot interleave another claim admission.
    if (!request.itemGain.Empty() && !state->resources.CanAdmitNewClaims(MaximumItemGainStacks))
        return reject(AdmissionCode::Backpressure,"native_item_gain_claim_capacity");
    const uint64_t monotonic = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (state->authority.BeginAtomic(held, id, monotonic).code != AuthorityCode::Allowed)
        return reject(AdmissionCode::ReconciliationRequired, "atomic_admission_rejected");
    pending.held = held; pending.dispatched = true; // Consumed before native execution.
    state->operationDispatching = true;
    struct DispatchGuard { State& state; ActivityLease lease; std::string id; ~DispatchGuard() {
        state.authority.EndDispatch(lease, id);
        const auto binding = state.bindings.find(lease.actor);
        if (binding != state.bindings.end()) binding->second.publisher.Publish(state.authority.Read(lease.actor));
        state.operationDispatching = false;
    } } dispatchGuard{*state, held, id};
    NativeObservation observation;
    std::vector<VerifiedItemGain> nativeGains;
    bool nativeTransactionOpen = false;
    try {
        if (bot->GetTradeData() || !ValidateOperationResources(request,state->resources,NativeConsumptionBalances(*bot,request),blocker) ||
            !adapter.ValidateNative(*bot, request, blocker)) {
            observation.state = OperationState::Rejected;
            observation.evidence = bot->GetTradeData() ? "native_trade_in_progress" :
                (IsToken(blocker) ? blocker : "native_prerequisite_changed");
        } else if (state->authority.BeginDispatch(held, id, monotonic).code == AuthorityCode::Allowed) {
            auto executing = saved->second; executing.ownerGeneration = held.generation;
            auto action = grant.action; action.operation = id;
            const Effects effects{request.effects, Lane::Managed, true};
            if (state->authority.Authorize(effects, current, monotonic, &executing, &action) == AuthorityCode::Allowed) {
                state->bindings.at(intended.actor).publisher.Publish(state->authority.Read(intended.actor));
                ExecutionScope scope(executing, action);
                const auto nativeBefore=NativeConsumptionBalances(*bot,request,false);
                const auto itemsBefore=NativeGainStacks(*bot,request.itemGain);
                if (request.persistence != NativePersistence::JournalOnly) {
                    // Local service adapters are admitted only without a native
                    // transaction already in progress. Mail/guild operations
                    // with their own transactions require dedicated hooks.
                    nativeTransactionOpen = !CharacterDatabase.HasOpenTransaction() && CharacterDatabase.BeginTransaction();
                    if (nativeTransactionOpen) state->HoldNativeSave(intended.actor,true);
                }
                if (request.persistence == NativePersistence::JournalOnly || nativeTransactionOpen) {
                    ++state->nativeDispatches; result.executed = true;
                    observation = adapter.ExecuteNative(*bot, request);
                } else {
                    observation.state=OperationState::Rejected;
                    observation.evidence="native_save_transaction_unavailable";
                }
                if (observation.state == OperationState::Verified &&
                    !VerifyConsumedNativeResources(request,nativeBefore,NativeConsumptionBalances(*bot,request,false),blocker)) {
                    observation.state=OperationState::Reconciling;
                    observation.evidence=blocker; // Preserve actual adapter after-state and native reference.
                }
                if (observation.state == OperationState::Verified && !request.itemGain.Empty() &&
                    !VerifyNativeItemGain(intended.actor,request.itemGain,itemsBefore,NativeGainStacks(*bot,request.itemGain),nativeGains,blocker)) {
                    observation.state=OperationState::Reconciling; observation.evidence=blocker;
                }
            }
        }
    } catch (const std::exception&) { observation = {}; observation.evidence="native_adapter_exception"; }
    state->authority.EndDispatch(held, id); // No old operation scope survives the callback.
    state->bindings.at(intended.actor).publisher.Publish(state->authority.Read(intended.actor));
    if (!ValidateNativeObservation(observation)) observation = {};
    Task after = saved->second; ++after.revision; after.updatedAtMs = NowMs();
    pending.uncertain = observation.state == OperationState::Reconciling;
    if (pending.uncertain && !request.itemGain.Empty()) pending.saveBlocked=true;
    pending.outcome = observation.state;
    after.phase = pending.uncertain ? Phase::Reconciling : Phase::Verifying;
    after.checkpoint.blocker = pending.uncertain ? observation.evidence : "";
    // Uncertainty is itself a native observation. Retain its references and
    // measured after-state so a restart can reconcile without guessing/replay.
    OperationResult proof; proof.id = id; proof.task = intended.id; proof.taskRevision = intended.revision;
    proof.kind = request.kind; proof.state = observation.state; proof.nativeReference = observation.nativeReference;
    proof.evidence = observation.evidence;
    const auto receipt=NewId();
    WritePlan plan; std::vector<ClaimReceiptChange> changes;
    std::string gainReservation;
    try {
        if (proof.state == OperationState::Verified && !request.itemGain.Empty()) {
            auto acquired=AcquiredOperationWrite(after,saved->second.revision,proof,receipt,observation.afterState,
                request.consumption,request.itemGain,nativeGains);
            const auto acquiredClaims=ItemGainClaims(after,id,request.itemGain,nativeGains);
            std::vector<NativeResourceBalance> balances;
            for (const auto& gain : nativeGains) balances.push_back({intended.actor,gain.after.guid,gain.after.entry,gain.after.count,0,"bags"});
            // Protect the actual gained quantities before any map-worker turn,
            // and retain protection across persistence retries/lost replies.
            const auto heldGains=state->resources.ReservePending(receipt,acquiredClaims,balances);
            if (heldGains != ClaimInstall::Installed && heldGains != ClaimInstall::Duplicate) {
                state->resources.BlockProjection(); state->claimRestoreFailed=true;
                state->claimBlocker="native_acquired_items_require_reconciliation"; ++state->invalidClaims;
                pending.saveBlocked=true;
                throw std::runtime_error("Native gain protection failed");
            }
            gainReservation=receipt; plan=std::move(acquired.journal); changes=std::move(acquired.changes);
        } else if (proof.state == OperationState::Verified && !request.consumption.empty()) {
            auto consumed=ConsumedOperationWrite(after,saved->second.revision,proof,receipt,observation.afterState,request.consumption);
            plan=std::move(consumed.journal); changes=std::move(consumed.changes);
        } else plan=OperationOutcomeWrite(after,saved->second.revision,proof,receipt,observation.afterState);
    } catch (const std::exception&) {
        // Never replay a native effect because its consumed-claim projection
        // could not be produced. Preserve its actual after-state for recovery.
        pending.uncertain=true; pending.outcome=proof.state=OperationState::Reconciling;
        proof.evidence="claim_outcome_requires_reconciliation";
        after.phase=Phase::Reconciling; after.checkpoint.blocker=proof.evidence;
        changes.clear();
        plan=OperationOutcomeWrite(after,saved->second.revision,proof,receipt,observation.afterState);
    }
    State::Pending write{std::move(after),std::move(plan),"",id,true};
    write.claims=std::move(changes);
    write.gainReservation=gainReservation;
    if (nativeTransactionOpen) {
        try {
            if (!CharacterDatabase.HasOpenTransaction()) throw std::runtime_error("native_transaction_escaped");
            bot->SaveServiceStateToDB(request.persistence == NativePersistence::Profession);
            write.nativeSave=NativeSaveBatch::Capture(CharacterDatabase,write.plan,
                adapter.PersistedNativeProof(*bot,request,write.task)+NativeGainProof(nativeGains));
        } catch (const std::exception&) {
            // Never emit the old success receipt alone after a native save
            // could not be sealed. Preserve uncertainty and both actor holds.
            if (CharacterDatabase.HasOpenTransaction()) CharacterDatabase.RollbackTransaction();
            pending.saveBlocked=pending.uncertain=true;
            pending.outcome=proof.state=OperationState::Reconciling;
            proof.evidence="native_save_capture_requires_reconciliation";
            write.task.phase=Phase::Reconciling; write.task.checkpoint.blocker=proof.evidence;
            write.claims.clear();
            // A failed capture cannot acknowledge acquired claims separately.
            // Their pending holds remain until native domain reconciliation.
            write.gainReservation.clear();
            write.plan=OperationOutcomeWrite(write.task,saved->second.revision,proof,receipt,observation.afterState);
        }
    }
    state->pending.push_back(std::move(write));
    state->nextWork = 0; result.outcomeQueued = true;
    return reject(AdmissionCode::Pending, pending.uncertain ? "native_outcome_uncertain" : "native_result_receipt_pending");
}
