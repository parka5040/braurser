#ifndef BRAURSER_ELEVATE_AUTH_HPP
#define BRAURSER_ELEVATE_AUTH_HPP

#include <string>
#include <vector>

namespace braurser::elevate {

int auth_check(const std::string& program, const std::vector<std::string>& args,
               const std::string& job_dir, const std::vector<std::string>& environment = {});

}

#endif
