#pragma once
#include "LivingActivityReservations.h"
#include "LivingGuildProcurementPreparation.h"
class Player;
namespace LivingActivity {
    bool ReadNativeGuildProcurementMaterials(Player&,const Task&,const UnsettledClaimBatch&,
        GuildProcurementMaterialPlan&,std::string&);
    class NativeGuildProcurementReservation final : public NativeReservationAdapter {
    public:
        bool ValidatePurpose(Player&,const ReservationRequest&,std::string&) override;
    };
}
