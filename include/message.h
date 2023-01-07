#ifndef _MESSAGE_H_
#define _MESSAGE_H_

#include <stdint.h>
// For simplicity use uint64 value as message.
// If there is a need to determine that the message has not been updated since the last read,
// then some kind of generation is needed.
struct Message {
    uint64_t val;
    // unsigned length;
    // std::byte data[Configuration::max_message_size];
};

#endif
