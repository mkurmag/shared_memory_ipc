#include <algorithm>
#include <atomic>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <tuple>
#include <vector>
#include <random>
#include <thread>

namespace bipc = boost::interprocess;

namespace Configuration {
const unsigned cache_line_size = 64;
const unsigned max_message_size = 16;
const unsigned number_of_processes = 3;
static_assert(number_of_processes <= 31, "supported up to 31 processes");
const std::string shared_obj_name_prefix = "shared_memory";
}  // namespace Configuration

struct Message {
    uint64_t val;
    // unsigned length;
    // std::byte data[Configuration::max_message_size];
};

// All field are zero initialized. No need to call constructor if object is
// created in zero initialized memory.
class SharedDataContainer {
public:
    bool IsEmpty() const {
        return current_slot_id_ == 0;
    }

    // Returns handle to get message and to unlock it.
    // Handle is a slot index.
    int ReaderLock(int process_index) {
        std::cout << "Lock " << process_index << "\n";
        if (current_slot_id_ == 0) {
            throw std::runtime_error("ReaderLock should not be called for empty Container");
        }
        int slot_index;
        uint32_t current_value, new_value;
        do {
            // current_slot_id_ value can change. Save value
            slot_index = current_slot_id_ - 1;
            current_value = slots[slot_index].used_by;
            if ((current_value & Slot::is_used_mask) == 0) {
                // slot was reclaimed. Repeat
                continue;
            }
            new_value = current_value | (1 << process_index);
        } while (
            !atomic_compare_exchange_weak(&slots[slot_index].used_by, &current_value, new_value));
        return slot_index;
    }

    void ReaderUnlock(int process_index, int handle) {
        uint32_t current_value = slots[handle].used_by;
        std::cout << "Unlock " << process_index << " " << std::hex << current_value << std::dec
                  << "\n";
        assert(current_value & (1 << process_index));
        std::atomic_fetch_xor(&slots[handle].used_by, 1 << process_index);
    }

    void ReaderReset(int process_index) {
        // Unlock every slot locked by the process
        for (int i = 0, num = std::size(slots); i < num; ++i) {
            if (slots[i].used_by & (1 << process_index)) {
                std::cout << "Reset " << process_index << " " << i << " " << std::hex
                          << slots[i].used_by << std::dec << "\n";
                std::atomic_fetch_xor(&slots[i].used_by, 1 << process_index);
            }
        }
    }

    Message* ReaderGetMessage(int handle) {
        return &slots[handle].message;
    }

    void WriterAddMessage(const Message& msg) {
        // Find next free slot
        int next_slot_index = [this]() {
            for (int i = 0, num = std::size(slots); i < num; ++i) {
                if (slots[i].used_by == 0)
                    return i;
            }
            throw std::runtime_error("No free slots for writer");
        }();

        slots[next_slot_index].message = msg;
        std::atomic_fetch_or(&slots[next_slot_index].used_by, Slot::is_used_mask);

        int old_slot_id = current_slot_id_;
        current_slot_id_ = next_slot_index + 1;
        std::cout << "Write to slot " << next_slot_index << " kill\n" << std::flush;
        sleep(1);
        if (old_slot_id > 0) {
            std::atomic_fetch_xor(&slots[old_slot_id - 1].used_by, Slot::is_used_mask);
        }
    }

    void WriterReset() {
        // Unset is_used_mask for all slots except current_slot_id_
        for (int i = 0, num = std::size(slots); i < num; ++i) {
            if ((i != current_slot_id_ - 1) && (slots[i].used_by & Slot::is_used_mask)) {
                std::cout << "WriterReset"
                          << "\n";
                std::atomic_fetch_xor(&slots[i].used_by, Slot::is_used_mask);
            }
        }
    }

private:
    struct Slot {
        static const uint32_t is_used_mask = 1 << 31;  // TODO
        std::atomic<uint32_t> used_by;
        Message message;
    };
    // 1 + index of last written slot. Reserve value zero for empty container.
    // TODO
    std::atomic<int> current_slot_id_;
    // TODO explain size
    Slot slots[Configuration::number_of_processes + 1];

    // false sharing
    // cache line 64
};

class Producer {
public:
    Producer(int process_index) {
        std::string sh_name(Configuration::shared_obj_name_prefix + std::to_string(process_index));

        // Create or open shared memory object. Create on fresh start,
        // open on starts after crash.
        shared_mem_obj_ =
            bipc::shared_memory_object(bipc::open_or_create, sh_name.c_str(), bipc::read_write);
        shared_mem_obj_.truncate(sizeof(SharedDataContainer));
        // Map region to truncated shared memory object
        mem_region_ = bipc::mapped_region(shared_mem_obj_, bipc::read_write);

        // Shared memory on creation is filled with zeroes. So there is no
        // need to call SharedDataContainer constructor. Just cast it.
        shared_data_ptr_ = static_cast<SharedDataContainer*>(mem_region_.get_address());

        shared_data_ptr_->WriterReset();
    }

    void UpdateMessage(const Message& msg) {
        shared_data_ptr_->WriterAddMessage(msg);
    }

private:
    bipc::shared_memory_object shared_mem_obj_;
    bipc::mapped_region mem_region_;
    SharedDataContainer* shared_data_ptr_ = nullptr;
};

class Consumer {
public:
    // current_process_index - index of current process
    // producer_index - index of process-producer to read from
    Consumer(int current_process_index, int producer_index)
        : producer_process_index(producer_index), process_index_(current_process_index) {
        std::string sh_name =
            Configuration::shared_obj_name_prefix + std::to_string(producer_index);

        // busy wait until shared data is available
        while (true) {
            // repeat until producer creates shared object
            try {
                shared_mem_obj_ =
                    bipc::shared_memory_object(bipc::open_only, sh_name.c_str(), bipc::read_write);
                mem_region_ = bipc::mapped_region(shared_mem_obj_, bipc::read_write);
                // creation was successful
                break;
            } catch (bipc::interprocess_exception& err) {}
        }
        shared_data_ptr_ = static_cast<SharedDataContainer*>(mem_region_.get_address());

        // Unlock everything locked by current process before crash.
        shared_data_ptr_->ReaderReset(process_index_);
    }

    bool HasMessage() const {
        return !shared_data_ptr_->IsEmpty();
    }

    Message* LockMessage() {
        if (locked_message_handle_ != -1) {
            throw std::runtime_error("Consumer: attempt to double lock a message");
        }
        if (!HasMessage()) {
            throw std::runtime_error("Consumer: attempt to lock an empty message");
        }
        locked_message_handle_ = shared_data_ptr_->ReaderLock(process_index_);
        return shared_data_ptr_->ReaderGetMessage(locked_message_handle_);
    }

    void UnlockMessage() {
        if (locked_message_handle_ == -1) {
            throw std::runtime_error("Consumer: attempt to unlock not locked message");
        }
        shared_data_ptr_->ReaderUnlock(process_index_, locked_message_handle_);
        locked_message_handle_ = -1;
    }

public:
    int producer_process_index;

private:
    bipc::shared_memory_object shared_mem_obj_;
    bipc::mapped_region mem_region_;
    int locked_message_handle_ = -1;
    // Index of current process
    int process_index_;
    SharedDataContainer* shared_data_ptr_ = nullptr;
};

class Worker {
public:
    void handle(const std::vector<Message*>& messages) {
        (void)messages;
        // do nothing
    }
    Message update() {
        counter++;
        return Message{counter};
    }

private:
    unsigned counter = 0;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <index>\n";
        return 1;
    }
    int process_index = std::stoi(argv[1]);
    if (process_index > Configuration::number_of_processes) {
        std::cout << "Too big index. Max index is " << Configuration::number_of_processes - 1 << "\n";
        return 1;
    }
    // Start with creating a producer to prevent deadlock
    Producer producer(process_index);
    std::vector<Consumer> consumers;
    std::cout << process_index << ": waiting for other processes\n";
    for (int i = 0; i < Configuration::number_of_processes; i++) {
        if (i != process_index) {
            consumers.emplace_back(process_index, i);
        }
    }
    std::cout << process_index << ": ready\n";

    std::default_random_engine random_gen(std::random_device{}());
    std::uniform_int_distribution<int> dist{1, 1'000'000};
    unsigned prod_value = 0;
    while (true) {
        // std::vector<Message*> messages(consumers.size(), nullptr);
        for (int i = 0, num = consumers.size(); i < num; i++) {
            Consumer& consumer = consumers[i];
            if (consumer.HasMessage()) {
                Message* msg = consumer.LockMessage();
                std::cout << process_index << ": info from " << consumer.producer_process_index << ": " << msg->val << "\n";
                consumer.UnlockMessage();
            }
        }

        prod_value++;
        std::cout << process_index << " producer: update " << prod_value << "\n";
        producer.UpdateMessage(Message{prod_value});
        std::this_thread::sleep_for(std::chrono::microseconds{dist(random_gen)});
        // for (int i = 0; i < 10; i++) {
        //     if (i % 2 == 0) {
        //         std::cout << index << " producer: inc " << producer.Inc() << "\n";
        //     }
        //     std::cout << index << " consumer: " << consumer.Get() << "\n";
        //     sleep(1);
        // }
    }
    return 0;
}

// Add version number, check shared_data size
// Shared memory is not removed. We need another synchronisation point for that
// check if too process called with same index;
//
// new assumptions:
//  - number of process statically configurable and <= 31.
//
// TODO:
// * document API
// * unit tests?
