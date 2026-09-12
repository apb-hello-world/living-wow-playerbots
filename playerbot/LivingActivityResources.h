#ifndef LIVING_ACTIVITY_RESOURCES_H
#define LIVING_ACTIVITY_RESOURCES_H
#include "LivingActivity.h"
#include "LivingActivityResourceView.h"
#include <map>
#include <set>
#include <utility>
#include <tuple>

namespace LivingActivity {
    bool ValidResourceClaim(const ResourceClaim& claim);
    bool ProtectsResources(const ResourceClaim& claim);
    bool SameResourceClaim(const ResourceClaim& left, const ResourceClaim& right);

    struct ClaimReceiptChange {
        ResourceClaim after;
        uint64_t expectedRevision = 0;
    };
    struct UnsettledClaimBatch {
        uint64_t bookRevision=0;
        bool complete=false;
        std::vector<ResourceClaim> claims; // At most one existing 16-claim batch.
    };
    enum class ClaimInstall { Installed, Duplicate, Invalid, Stale, Capacity, NotReady };

    // Supplied by the compiled native reservation adapter, never model input.
    // A balance is an admission snapshot, not evidence of a purchase/transfer.
    struct NativeResourceBalance {
        uint32_t actor = 0, itemGuid = 0, itemEntry = 0, quantity = 0, copper = 0;
        std::string location;
        uint64_t nativeReference = 0; // Mail ID for an exact native attachment; zero for bags/bank/money.
    };
    inline bool ValidNativeResourceBalance(const NativeResourceBalance& b) {
        if (!b.actor) return false;
        if (b.location=="money") return !b.itemGuid && !b.itemEntry && !b.quantity && !b.nativeReference;
        return b.itemGuid && b.itemEntry && !b.copper &&
            ((b.location=="mail" && b.nativeReference && b.nativeReference<=UINT32_MAX) ||
             ((b.location=="bags" || b.location=="bank") && !b.nativeReference));
    }
    // Preparation-only reservation/release. No transfer, consumption, native
    // effect or completion can be written through this path. The coordinator
    // must validate actual possession and protect pending quantities before
    // enqueueing; only an exact receipt may update its acknowledged claim book.
    WritePlan ResourceReservationWrite(const Task& task, uint64_t expectedRevision,
        const std::string& receipt, std::vector<ClaimReceiptChange> changes,
        std::vector<NativeResourceBalance> balances);

    // Immutable value projection for consumers. It is protection, NOT an
    // execution grant or a copy of native possessions. Only actual native item
    // counts/money may be passed to the unreserved-quantity helpers.
    struct ResourceProtection {
        bool ready = false;
        uint64_t revision = 0;
        std::map<uint32_t, uint64_t> items;
        std::map<std::pair<uint32_t, uint32_t>, uint64_t> uncertainEntries;
        std::map<uint32_t, uint64_t> money;
        uint64_t ProtectedItem(uint32_t actor, uint32_t guid, uint32_t entry) const;
        uint64_t ProtectedMoney(uint32_t actor) const;
        uint32_t UnreservedItem(uint32_t actor, uint32_t guid, uint32_t entry, uint32_t nativeCount) const;
        uint32_t UnreservedMoney(uint32_t actor, uint32_t nativeCopper) const;
    };

    // World-thread-owned acknowledged claim cache. No timer, expiry, scheduling,
    // DB access, native pointers, native operation or implicit release exists
    // here. The coordinator installs only exact persisted receipt results.
    // Domain validators/journals must prove splits, transfers and consumption;
    // accepting a row here alone cannot authorize any of those operations.
    class ResourceClaimBook {
    public:
        explicit ResourceClaimBook(size_t capacity = 50000) : capacity(capacity > 50000 ? 50000 : capacity) {}
        ClaimInstall RestoreBatch(const std::vector<ResourceClaim>& rows);
        bool FinishRestore();
        void BlockProjection();
        ClaimInstall InstallReceipt(const std::vector<ClaimReceiptChange>& changes);
        // Protect additional quantities BEFORE asynchronous reservation SQL.
        // No timeout releases these holds. A negative/uncertain SQL result must
        // be reconciled; only the exact committed receipt may settle the batch.
        ClaimInstall ReservePending(const std::string& receipt, const std::vector<ClaimReceiptChange>& changes,
            const std::vector<NativeResourceBalance>& nativeBalances);
        // Called only after a compiled native whole-stack transfer verifies its
        // surviving identity. Protect both old and new GUIDs until the SAME
        // native-save/claim receipt commits. This never proves the transfer.
        ClaimInstall ReserveTransferred(const std::string& receipt,const ClaimReceiptChange& change,
            const NativeResourceBalance& destination);
        ClaimInstall CommitReservation(const std::string& receipt);
        bool HasPending(const std::string& receipt) const { return pending.count(receipt) != 0; }
        size_t PendingCount() const { return pending.size(); }
        const ResourceProtection& Protection() const { return protection; }
        ResourceReader Reader() const { return publisher.Reader(); }
        const ResourceClaim* Inspect(const std::string& id) const;
        size_t Size() const { return records.size(); }
        bool CanAdmitNewClaims(size_t count) const;
        // Planning availability for one acknowledged root: its own saved,
        // held stock remains usable while other roots and unacknowledged holds
        // remain protected. This does not authorize consumption or a transfer.
        // False means ambiguous/unreconciled ownership, NOT zero stock to buy.
        bool AvailableToTask(const std::string& task,const NativeResourceBalance& native,uint32_t& available) const;
        // Indexed, acknowledged root obligations, including proposals and
        // transfers. A bounded first batch, NOT a completion/consumption grant.
        // Call again after a saved settlement; never assume a truncated batch
        // contains every claim. Pending reservations fail closed.
        bool ReadUnsettled(const std::string& task,UnsettledClaimBatch& batch,std::string& blocker) const;
    private:
        ClaimInstall ReservePendingImpl(const std::string& receipt,const std::vector<ClaimReceiptChange>& changes,
            const std::vector<NativeResourceBalance>& nativeBalances,bool transferred);
        void Index(const ResourceClaim& claim, bool add);
        void IndexAcknowledged(const ResourceClaim& claim,bool add);
        size_t PendingSlots(const std::string& excluding = "") const;
        struct PendingReservation {
            bool transferred=false;
            std::vector<ClaimReceiptChange> changes;
            std::vector<NativeResourceBalance> balances;
            std::vector<ResourceClaim> additional;
        };
        std::map<std::string,PendingReservation> pending;
        std::string committing;
        size_t capacity;
        bool restoreFailed = false;
        std::map<std::string, ResourceClaim> records;
        using OwnedKey=std::tuple<std::string,uint32_t,uint32_t,uint32_t,std::string,uint64_t>;
        std::map<OwnedKey,uint64_t> acknowledgedOwned; // Updated only with saved receipts, not pending holds.
        using RootItemKey=std::tuple<std::string,uint32_t,uint32_t,uint32_t>;
        std::map<RootItemKey,uint64_t> acknowledgedProtected;
        std::map<std::string,std::set<std::string>> unsettledByTask;
        ResourceProtection protection;
        ResourcePublisher publisher;
    };
}
#endif
