#pragma once

#include <cerrno>
#include <string>
#include <system_error>

// Why the last file operation failed. C++ streams only say that something went
// wrong, so set errno to 0 right before the operation and call this right after.
inline std::string lastErrorText() {
    return errno == 0 ? std::string("the system gave no reason") : std::generic_category().message(errno);
}
