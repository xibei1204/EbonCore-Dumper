#include "SDK.hpp"

int main() {
    static_assert(EbonSDK::TypeIndex.size() == 2);
    return EbonSDK::TypeIndex[0].Size == 16 ? 0 : 1;
}
