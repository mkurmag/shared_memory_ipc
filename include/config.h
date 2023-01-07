#ifndef _CONFIGURATION_H_
#define _CONFIGURATION_H_

#include <string>

namespace Configuration {
// PROCESSES_COUNT is passed by cmake
const unsigned number_of_processes = PROCESSES_COUNT;
static_assert(number_of_processes <= 31, "supported up to 31 processes");
const std::string shared_obj_name_prefix = "shared_memory";
}  // namespace Configuration

#endif
