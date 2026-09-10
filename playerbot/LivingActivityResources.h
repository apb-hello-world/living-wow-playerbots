#ifndef LIVING_ACTIVITY_RESOURCES_H
#define LIVING_ACTIVITY_RESOURCES_H
#include "LivingActivity.h"
#include "LivingActivityResourceView.h"
#include <map>
#include <utility>

namespace LivingActivity {
    bool ValidResourceClaim(const ResourceClaim& claim);
    bool ProtectsResources(const ResourceClaim& claim);
    bool SameResourceClaim(const ResourceClaim& left, const ResourceClaim& right);

    struct ClaimReceiptChange {
        ResourceClaim after;
        uint64_t expectedRevision = 0;
    };
    enum class ClaimInstall { Installed, Duplicate, Invalid, Stale, Capacity, NotReady };

    // Supplied by the compiled native reservation adapter, never model input.
    // A balance is an admission snapshot, not evidence of a purchase/transfer.
    struct NativeResourceBalance {
        uint32_t actor = 0, itemGuid = 0, itemEntry = 0, quantity = 0, copper = 0;
        std::string location;
    };
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
        ClaimInstall InstallReceipt(const std::vector<ClaimReceiptChange>& changes);
        // Protect additional quantities BEFORE asynchronous reservation SQL.
        // No timeout releases these holds. A negative/uncertain SQL result must
        // be reconciled; only the exact committed receipt may settle the batch.
        ClaimInstall ReservePending(const std::string& receipt, const std::vector<ClaimReceiptChange>& changes,
            const std::vector<NativeResourceBalance>& nativeBalances);
        ClaimInstall CommitReservation(const std::string& receipt);
        bool HasPending(const std::string& receipt) const { return pending.count(receipt) != 0; }
        size_t PendingCount() const { return pending.size(); }
        const ResourceProtection& Protection() const { return protection; }
        ResourceReader Reader() const { return publisher.Reader(); }
        const ResourceClaim* Inspect(const std::string& id) const;
        size_t Size() const { return records.size(); }
    private:
        void Index(const ResourceClaim& claim, bool add);
        size_t PendingSlots(const std::string& excluding = "") const;
        struct PendingReservation {
            std::vector<ClaimReceiptChange> changes;
            std::vector<NativeResourceBalance> balances;
            std::vector<ResourceClaim> additional;
        };
        std::map<std::string,PendingReservation> pending;
        std::string committing;
        size_t capacity;
        bool restoreFailed = false;
        std::map<std::string, ResourceClaim> records;
        ResourceProtection protection;
        ResourcePublisher publisher;
    };
}
#endif
