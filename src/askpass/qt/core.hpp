#ifndef BRAURSER_ASKPASS_QT_CORE_HPP
#define BRAURSER_ASKPASS_QT_CORE_HPP

#include <braurser/presenter/models.hpp>
#include <functional>
#include <span>
#include <vector>

namespace braurser::askpass {

presenter::AuthAction action(std::string_view value);
bool write_password(std::vector<char>& password, const std::function<bool(std::span<const char>)>& write);
std::optional<std::string> current_account();

}

#endif
