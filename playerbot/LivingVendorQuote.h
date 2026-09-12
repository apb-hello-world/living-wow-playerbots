#pragma once
#include <cstdint>
#include <string>

namespace LivingActivity {
struct NativeVendorQuote {
    uint32_t actor=0,entry=0,quantity=0,copper=0,moneyBefore=0;
    uint32_t vendorEntry=0,buyUnits=0;
    uint64_t vendor=0;
};
std::string EncodeNativeVendorQuote(const NativeVendorQuote& quote);
// Decoding a journal is not renewed purchase authority. Native stock, price,
// prerequisites, budget, reservations and task context are rechecked on dispatch.
bool DecodeNativeVendorQuote(const std::string& value,NativeVendorQuote& quote);
}
