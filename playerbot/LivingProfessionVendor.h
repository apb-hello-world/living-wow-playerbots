#ifndef LIVING_PROFESSION_VENDOR_H
#define LIVING_PROFESSION_VENDOR_H
#include "LivingActivityReservations.h"
#include "LivingProfessionDemand.h"
namespace LivingActivity {
    // Startup/reload hooks on the native travel table; no per-tick DB work.
    void ClearNativeVendorSources();
    void RegisterNativeVendorSource(uint32_t vendor);
    void SealNativeVendorSources();
    bool NativeProfessionVendorSources(Player& actor,uint32_t entry,uint32_t quantity,
        std::vector<int32_t>& vendors,std::string& blocker);
    bool NextNativeProfessionVendorItem(Player& actor,const Task& saved,
        ProfessionReagent& need,std::vector<int32_t>& vendors,std::string& blocker);
    bool PlanNativeProfessionPurchase(Player& actor,const Task& saved,const ProfessionReagent& need,
        NativeVendorQuote& quote,std::string& blocker);
    class NativeProfessionMoneyReservation final : public NativeReservationAdapter {
    public:
        NativeProfessionMoneyReservation(NativeVendorQuote quote,std::string operation)
            : quote(std::move(quote)),operation(std::move(operation)) {}
        bool ValidatePurpose(Player& actor,const ReservationRequest& request,std::string& blocker) override;
    private:
        NativeVendorQuote quote;
        std::string operation;
    };
}
#endif
