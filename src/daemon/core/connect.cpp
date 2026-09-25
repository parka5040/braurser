#include <braurser/daemon/core/connect.hpp>

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>

namespace braurser::daemon::core {
namespace {

AddressClass classify_v4(std::uint32_t value) {
    const auto a = value >> 24;
    const auto b = (value >> 16) & 255U;
    if (a == 0) return AddressClass::Unspecified;
    if (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168)) return AddressClass::Private;
    if (a == 100 && b >= 64 && b <= 127) return AddressClass::CarrierGradeNat;
    if (a == 127) return AddressClass::Loopback;
    if (a == 169 && b == 254) return AddressClass::LinkLocal;
    if (a == 192 && b == 0 && ((value >> 8) & 255U) == 0) return AddressClass::Reserved;
    if (a == 192 && b == 0 && ((value >> 8) & 255U) == 2) return AddressClass::Documentation;
    if (a == 198 && (b == 18 || b == 19)) return AddressClass::Reserved;
    if (a == 198 && b == 51 && ((value >> 8) & 255U) == 100) return AddressClass::Documentation;
    if (a == 203 && b == 0 && ((value >> 8) & 255U) == 113) return AddressClass::Documentation;
    if (a >= 224 && a <= 239) return AddressClass::Multicast;
    if (a >= 240) return AddressClass::Reserved;
    return AddressClass::Public;
}

bool prefix(const unsigned char* bytes, const unsigned char* pattern, std::size_t length) {
    return std::memcmp(bytes, pattern, length) == 0;
}

}

AddressClass classify_address(const sockaddr* address, socklen_t length) {
    if (address == nullptr) return AddressClass::Reserved;
    if (address->sa_family == AF_INET) {
        if (length < sizeof(sockaddr_in)) return AddressClass::Reserved;
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        return classify_v4(ntohl(ipv4->sin_addr.s_addr));
    }
    if (address->sa_family != AF_INET6 || length < sizeof(sockaddr_in6)) return AddressClass::Reserved;
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
    const auto* bytes = ipv6->sin6_addr.s6_addr;
    constexpr unsigned char zero[16]{};
    if (prefix(bytes, zero, 16)) return AddressClass::Unspecified;
    if (prefix(bytes, zero, 15) && bytes[15] == 1) return AddressClass::Loopback;
    if (prefix(bytes, zero, 10) && bytes[10] == 255 && bytes[11] == 255) {
        const std::uint32_t value = static_cast<std::uint32_t>(bytes[12]) << 24 |
                                    static_cast<std::uint32_t>(bytes[13]) << 16 |
                                    static_cast<std::uint32_t>(bytes[14]) << 8 | bytes[15];
        const auto category = classify_v4(value);
        if (category == AddressClass::Private || category == AddressClass::CarrierGradeNat) {
            return AddressClass::Ipv4MappedPrivate;
        }
        return category;
    }
    constexpr unsigned char nat64[12]{0x00, 0x64, 0xff, 0x9b};
    if (prefix(bytes, nat64, 12)) {
        const std::uint32_t value = static_cast<std::uint32_t>(bytes[12]) << 24 |
                                    static_cast<std::uint32_t>(bytes[13]) << 16 |
                                    static_cast<std::uint32_t>(bytes[14]) << 8 | bytes[15];
        return classify_v4(value);
    }
    if (bytes[0] == 0xfe && (bytes[1] & 0xc0U) == 0x80) return AddressClass::LinkLocal;
    if ((bytes[0] & 0xfeU) == 0xfc) return AddressClass::UniqueLocal;
    if (bytes[0] == 0xff) return AddressClass::Multicast;
    constexpr unsigned char documentation[4]{0x20, 0x01, 0x0d, 0xb8};
    if (prefix(bytes, documentation, 4)) return AddressClass::Documentation;
    return AddressClass::Public;
}

bool PublicOnlyGuard::allow(const sockaddr* address, socklen_t length) const {
    return classify_address(address, length) == AddressClass::Public;
}

}
