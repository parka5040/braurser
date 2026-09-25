#ifndef BRAURSER_DAEMON_CORE_CONNECT_HPP
#define BRAURSER_DAEMON_CORE_CONNECT_HPP

#include <sys/socket.h>

namespace braurser::daemon::core {

enum class AddressClass {
    Public,
    Loopback,
    Private,
    LinkLocal,
    UniqueLocal,
    Multicast,
    Unspecified,
    Reserved,
    CarrierGradeNat,
    Documentation,
    Ipv4MappedPrivate
};

/// in: address points to a complete IPv4 or IPv6 socket address.
/// out: returns its address class, or Reserved for invalid input.
AddressClass classify_address(const sockaddr* address, socklen_t length);

class ConnectGuard {
public:
    virtual ~ConnectGuard() = default;
    /// in: address is the target address about to be connected.
    /// out: true only if the target is permitted.
    virtual bool allow(const sockaddr* address, socklen_t length) const = 0;
};

class PublicOnlyGuard final : public ConnectGuard {
public:
    bool allow(const sockaddr* address, socklen_t length) const override;
};

}

#endif
