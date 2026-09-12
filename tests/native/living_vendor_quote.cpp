#include "LivingVendorQuote.h"
#include <cassert>
using namespace LivingActivity;
int main() {
    const NativeVendorQuote original{7,3371,5,20,100,3314,1,99};
    NativeVendorQuote q;
    const auto encoded=EncodeNativeVendorQuote(original);
    assert(DecodeNativeVendorQuote(encoded,q) && EncodeNativeVendorQuote(q)==encoded);
    for(unsigned i=0;i<9;++i) {
        auto invalid=original;
        switch(i) {
        case 0:invalid.actor=0;break;case 1:invalid.vendor=0;break;case 2:invalid.vendorEntry=0;break;
        case 3:invalid.entry=0;break;case 4:invalid.quantity=0;break;case 5:invalid.buyUnits=0;break;
        case 6:invalid.buyUnits=256;break;case 7:invalid.copper=0;break;case 8:invalid.moneyBefore=19;break;
        }
        assert(!DecodeNativeVendorQuote(EncodeNativeVendorQuote(invalid),q) && !q.actor);
    }
    auto mismatch=original;mismatch.buyUnits=2;
    assert(!DecodeNativeVendorQuote(EncodeNativeVendorQuote(mismatch),q));
    mismatch=original;mismatch.copper=UINT32_MAX;mismatch.moneyBefore=UINT32_MAX;
    assert(!DecodeNativeVendorQuote(EncodeNativeVendorQuote(mismatch),q));
    for(const auto& invalid : {std::string("{}"),encoded+" ",encoded.substr(0,encoded.size()-1)+",\"actor\":7}",
                              std::string(1025,'x'),std::string("null")})
        assert(!DecodeNativeVendorQuote(invalid,q) && !q.actor);
}
