#ifndef BRAURSER_ELEVATE_NORMALIZE_HPP
#define BRAURSER_ELEVATE_NORMALIZE_HPP

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace braurser::elevate {

struct Normalized {
    std::vector<std::string> args;
    char operation{};
    std::string reason;
    explicit operator bool() const { return operation != 0; }
};

Normalized normalize(std::span<const std::string> input);
bool allowed_for_job(const Normalized& request, std::string_view job_dir);

}

#endif
