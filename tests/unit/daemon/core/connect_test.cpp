#include <braurser/daemon/core/connect.hpp>

#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <string>
#include <vector>

namespace braurser::daemon::core {
namespace {

struct Case {
    const char* address;
    AddressClass category;
};

TEST(Connect, ClassifiesIpv4AndIpv6) {
    const std::vector<Case> cases{
        {"0.0.0.0", AddressClass::Unspecified},
        {"0.255.255.255", AddressClass::Unspecified},
        {"1.1.1.1", AddressClass::Public},
        {"8.8.8.8", AddressClass::Public},
        {"9.255.255.255", AddressClass::Public},
        {"10.0.0.1", AddressClass::Private},
        {"10.255.255.255", AddressClass::Private},
        {"11.0.0.1", AddressClass::Public},
        {"100.63.255.255", AddressClass::Public},
        {"100.64.0.1", AddressClass::CarrierGradeNat},
        {"100.127.255.255", AddressClass::CarrierGradeNat},
        {"100.128.0.0", AddressClass::Public},
        {"127.0.0.1", AddressClass::Loopback},
        {"127.255.255.255", AddressClass::Loopback},
        {"169.254.1.1", AddressClass::LinkLocal},
        {"172.15.255.255", AddressClass::Public},
        {"172.16.0.0", AddressClass::Private},
        {"172.31.255.255", AddressClass::Private},
        {"172.32.0.0", AddressClass::Public},
        {"192.0.0.7", AddressClass::Reserved},
        {"192.0.2.1", AddressClass::Documentation},
        {"192.168.0.1", AddressClass::Private},
        {"192.169.0.1", AddressClass::Public},
        {"198.18.1.1", AddressClass::Reserved},
        {"198.19.255.255", AddressClass::Reserved},
        {"198.20.0.1", AddressClass::Public},
        {"198.51.100.1", AddressClass::Documentation},
        {"203.0.113.10", AddressClass::Documentation},
        {"223.255.255.1", AddressClass::Public},
        {"224.0.0.1", AddressClass::Multicast},
        {"239.255.255.255", AddressClass::Multicast},
        {"240.0.0.0", AddressClass::Reserved},
        {"255.255.255.255", AddressClass::Reserved},
        {"::", AddressClass::Unspecified},
        {"::1", AddressClass::Loopback},
        {"2001:4860:4860::8888", AddressClass::Public},
        {"2001:db8::1", AddressClass::Documentation},
        {"2001:db9::1", AddressClass::Public},
        {"fe80::1", AddressClass::LinkLocal},
        {"febf::1", AddressClass::LinkLocal},
        {"fc00::1", AddressClass::UniqueLocal},
        {"fdff::1", AddressClass::UniqueLocal},
        {"ff02::1", AddressClass::Multicast},
        {"::ffff:192.168.1.1", AddressClass::Ipv4MappedPrivate},
        {"::ffff:10.0.0.1", AddressClass::Ipv4MappedPrivate},
        {"::ffff:100.64.0.1", AddressClass::Ipv4MappedPrivate},
        {"::ffff:127.0.0.1", AddressClass::Loopback},
        {"::ffff:8.8.8.8", AddressClass::Public},
        {"64:ff9b::10.0.0.1", AddressClass::Private},
        {"64:ff9b::127.0.0.1", AddressClass::Loopback},
        {"64:ff9b::8.8.8.8", AddressClass::Public},
        {"64:ff9b::192.0.2.1", AddressClass::Documentation},
        {"192.88.99.1", AddressClass::Reserved},
        {"192.88.100.1", AddressClass::Public},
        {"::127.0.0.1", AddressClass::Reserved},
        {"::8.8.8.8", AddressClass::Reserved},
        {"::2", AddressClass::Reserved},
        {"64:ff9b:1::10.0.0.1", AddressClass::Reserved},
        {"64:ff9b:1:ffff::1", AddressClass::Reserved},
        {"64:ff9b:2::1", AddressClass::Public},
        {"2002:c0a8:0101::1", AddressClass::Reserved},
        {"2002:0808:0808::1", AddressClass::Reserved},
        {"2001::1", AddressClass::Reserved},
        {"2001:0:4136:e378:8000:63bf:3fff:fdd2", AddressClass::Reserved},
        {"2001:1::1", AddressClass::Public},
        {"fec0::1", AddressClass::Private},
        {"feff::1", AddressClass::Private},
        {"100::1", AddressClass::Reserved},
        {"100::ffff:ffff:ffff:ffff", AddressClass::Reserved},
        {"100:0:0:1::1", AddressClass::Public},
        {"2003::1", AddressClass::Public}
    };
    PublicOnlyGuard guard;
    for (const auto& item : cases) {
        SCOPED_TRACE(item.address);
        sockaddr_storage address{};
        socklen_t length;
        if (std::string(item.address).find(':') != std::string::npos) {
            auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&address);
            ipv6->sin6_family = AF_INET6;
            ASSERT_EQ(inet_pton(AF_INET6, item.address, &ipv6->sin6_addr), 1);
            length = sizeof(sockaddr_in6);
        } else {
            auto* ipv4 = reinterpret_cast<sockaddr_in*>(&address);
            ipv4->sin_family = AF_INET;
            ASSERT_EQ(inet_pton(AF_INET, item.address, &ipv4->sin_addr), 1);
            length = sizeof(sockaddr_in);
        }
        const auto* sock = reinterpret_cast<const sockaddr*>(&address);
        EXPECT_EQ(classify_address(sock, length), item.category);
        EXPECT_EQ(guard.allow(sock, length), item.category == AddressClass::Public);
    }
}

TEST(Connect, InvalidAddressesFailClosed) {
    PublicOnlyGuard guard;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    EXPECT_FALSE(guard.allow(nullptr, 0));
    EXPECT_FALSE(guard.allow(reinterpret_cast<const sockaddr*>(&address), 1));
    address.sin_family = AF_UNIX;
    EXPECT_FALSE(guard.allow(reinterpret_cast<const sockaddr*>(&address), sizeof(address)));
}

}
}
