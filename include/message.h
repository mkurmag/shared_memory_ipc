#ifndef _MESSAGE_H_
#define _MESSAGE_H_

#include <stdint.h>
// For simplicity use uint64 value as message
struct Message {
    uint64_t val;
    // unsigned length;
    // std::byte data[Configuration::max_message_size];
};

#endif
