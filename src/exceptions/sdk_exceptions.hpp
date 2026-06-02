#pragma once

#include <stdexcept>
#include <string>

namespace hand_sdk
{

class SdkException : public std::runtime_error
{
public:
    explicit SdkException(const std::string& msg) : std::runtime_error(msg) {}
};

} // namespace hand_sdk