#pragma once
#include "LivingActivityReservations.h"
#include "LivingGuildProcurementPreparation.h"
class Player;
class Item;
namespace LivingActivity {
    // Same personal-stock filter for admission, reservation, purchase demand
    // and bank withdrawal. A kept personal stack is not a guild contribution.
    bool NativeGuildProcurementItemUsable(Player&,uint32_t,Item*,bool alreadyClaimed);
    struct NativeGuildProcurementSource {
        uint32_t quantity=0,reference=0,estimatedCopper=0;
        std::string kind;
    };
    // Read-only, bounded candidate. Actual price, stock, route, claims and daily
    // spending limits are rechecked by shared adapters before any transaction.
    bool ReadNativeGuildProcurementSource(Player&,uint32_t,uint32_t,NativeGuildProcurementSource&,std::string&);
    bool ReadNativeGuildProcurementMaterials(Player&,const Task&,const UnsettledClaimBatch&,
        GuildProcurementMaterialPlan&,std::string&);
    class NativeGuildProcurementReservation final : public NativeReservationAdapter {
    public:
        bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
    };
}
