#ifndef LIVING_ACTIVITY_RESERVATIONS_H
#define LIVING_ACTIVITY_RESERVATIONS_H
#include "LivingActivityRequests.h"
#include "LivingActivityResources.h"
class Player;
namespace LivingActivity {
    struct ReservationRequest {
        TaskRequest transition;
        ActionContext authorization;
        std::vector<ClaimReceiptChange> changes;
    };
    // Finite compiled domain validation (recipe, delivery, safe sale, etc.).
    // This cannot supply balances or execute native operations. The coordinator
    // independently reads actual native possessions after this purpose check.
    class NativeReservationAdapter {
    public:
        virtual ~NativeReservationAdapter() = default;
        virtual bool ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& blocker) = 0;
    };
}
#endif
