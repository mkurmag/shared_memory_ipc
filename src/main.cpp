#include <algorithm>
#include <atomic>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <ostream>
#include <random>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

namespace bipc = boost::interprocess;

namespace Configuration {
const unsigned number_of_processes = PROCESSES_COUNT;
static_assert(number_of_processes <= 31, "supported up to 31 processes");
const std::string shared_obj_name_prefix = "shared_memory";
}  // namespace Configuration

struct Message {
    uint64_t val;
    // unsigned length;
    // std::byte data[Configuration::max_message_size];
};

// All class members are zero initialized. This allows to skip constructor call
// when class instance is created in the zero initialized memory.
class SharedDataContainer {
public:
    bool IsEmpty() const {
        return current_slot_id_ == 0;
    }

    // Locks slot with the most recent message by process with index `process_index`.
    // Slot won't be emptied until corresponding unlock by the same process.
    // Locks can't be nested, and several processes can simultaneously lock the same slot.
    //
    // Function returns a handle to get message and to unlock it. Handle is valid until
    // `ReaderUnlock` call.
    //
    // Function should not be called on empty container.
    int ReaderLock(int process_index) {
        if (current_slot_id_ == 0) {
            throw std::runtime_error("ReaderLock should not be called for empty container");
        }
        int slot_index;
        uint32_t current_value, new_value;

        // To lock the most recent slot, one needs to set `process_index` bit in the
        // `slots[current_slot_id_ - 1].used_by` field. But this whole operation can't be done
        // atomically. While setting the bit, new messages can be written and the slot can be
        // (partially) overwritten. It is ok, if the slot stops being the most recent, but it is not
        // ok if slot's message is (partially) overwritten.
        // To prevent this one must be sure that the locking slot is still used.
        // Use CAS for that. If it fails, reread current_slot_id_ as it may change and try again.
        do {
            // current_slot_id_ value can change, save current value it
            slot_index = current_slot_id_ - 1;
            current_value = slots[slot_index].used_by;
            if ((current_value & Slot::used_by_writer) == 0) {
                // slot is not used, because new message have been written.
                // It is unsafe to lock the slot. Repeat to get newer slot.
                continue;
            }
            new_value = current_value | (1 << process_index);
        } while (
            !atomic_compare_exchange_weak(&slots[slot_index].used_by, &current_value, new_value));

        return slot_index;
    }

    // Unlocks slot locked by process with index process_index.
    // Slot is specified by handle.
    void ReaderUnlock(int process_index, int handle) {
        // Handle is an index of the slot.
        uint32_t current_value = slots[handle].used_by;
        if ((current_value & (1 << process_index)) == 0) {
            throw std::runtime_error("Attempt to unlock not locked slot");
        }
        // clear the bit
        std::atomic_fetch_and(&slots[handle].used_by, ~(1 << process_index));
    }

    // Resets all locks made by the process.
    // Usefully for recovery after the crash.
    void ReaderReset(int process_index) {
        // Unlock every slot locked by the process
        for (int i = 0, num = std::size(slots); i < num; ++i) {
            if (slots[i].used_by & (1 << process_index)) {
                std::atomic_fetch_and(&slots[i].used_by, ~(1 << process_index));
            }
        }
    }

    // Returns message by handle
    Message* ReaderGetMessage(int handle) {
        return &slots[handle].message;
    }

    // Writes new message
    void WriterUpdateMessage(const Message& msg) {
        // Find next free slot
        int next_slot_index = [this]() {
            for (int i = 0, num = std::size(slots); i < num; ++i) {
                if (slots[i].used_by == 0)
                    return i;
            }
            throw std::runtime_error("No free slots for writer");
        }();

        slots[next_slot_index].message = msg;
        std::atomic_fetch_or(&slots[next_slot_index].used_by, Slot::used_by_writer);

        int old_slot_id = current_slot_id_;
        current_slot_id_ = next_slot_index + 1;
        // Clear used_by_writer bit in old slot if it exists
        if (old_slot_id > 0) {
            std::atomic_fetch_and(&slots[old_slot_id - 1].used_by, ~Slot::used_by_writer);
        }
    }

    // Fixes the state after crash during write
    void WriterReset() {
        // Clear used_by_writer bit for all slots except current_slot_id_
        for (int i = 0, num = std::size(slots); i < num; ++i) {
            if ((i != current_slot_id_ - 1) && (slots[i].used_by & Slot::used_by_writer)) {
                std::atomic_fetch_and(&slots[i].used_by, ~Slot::used_by_writer);
            }
        }
    }

private:
    struct Slot {
        static const uint32_t used_by_writer = 1 << 31;
        // 32-bit variable. Bits from 0 to 31 are set if slot is locked by process with
        // corresponding index. The highest bit (used_by_writer) is set if slot is used by writer.
        std::atomic<uint32_t> used_by = 0;
        Message message;
    };
    // Id of the slot with the most recent message. Id is 1 + index of the slot.
    // Value zero is reserved for indication of an empty container.
    std::atomic<int> current_slot_id_ = 0;
    // N+1 slots, because in the worst case all readers (N-1) lock different slots with old
    // messages, Nth slot is used for current message, and one more is needed to write new message
    // without overriding current.
    Slot slots[Configuration::number_of_processes + 1];
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
        mem_region_ = bipc::mapped_region(shared_mem_obj_, bipc::read_write);

        // If this is a fresh start, then SharedDataContainer must be initialized,
        // if this is a start after crash, then SharedDataContainer should not be changed.
        //
        // Shared memory on creation is filled with zeroes and SharedDataContainer members are
        // initialized with zeros. That's why we can just cast memory, without the need to
        // optionally call the SharedDataContainer's constructor
        shared_data_ptr_ = static_cast<SharedDataContainer*>(mem_region_.get_address());

        // Reset old unfinished writes
        shared_data_ptr_->WriterReset();
    }

    void UpdateMessage(const Message& msg) {
        shared_data_ptr_->WriterUpdateMessage(msg);
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

        // Wait until producer creates shared object
        while (true) {
            // shared_memory_object and mapped_region throw exceptions, if shared object is not
            // available.
            try {
                shared_mem_obj_ =
                    bipc::shared_memory_object(bipc::open_only, sh_name.c_str(), bipc::read_write);
                mem_region_ = bipc::mapped_region(shared_mem_obj_, bipc::read_write);
                // creation was successful
                break;
            } catch (bipc::interprocess_exception& err) {}
        }

        shared_data_ptr_ = static_cast<SharedDataContainer*>(mem_region_.get_address());

        // Reset unfinished reads
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
    int producer_process_index; // index of process-producer

private:
    bipc::shared_memory_object shared_mem_obj_;
    bipc::mapped_region mem_region_;
    int locked_message_handle_ = -1;
    int process_index_; // Index of current process
    SharedDataContainer* shared_data_ptr_ = nullptr;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <index>\n";
        return 1;
    }
    int process_index = std::stoi(argv[1]);
    if (process_index > Configuration::number_of_processes) {
        std::cout << "Too big index. Max index is " << Configuration::number_of_processes - 1
                  << "\n";
        return 1;
    }

    // Start with creating a producer to prevent deadlock
    Producer producer(process_index);
    // Create consumers, each consumer waits for its process-producer to create a shared object.
    std::vector<Consumer> consumers;
    std::cout << process_index << ": waiting for other processes\n";
    for (int i = 0; i < Configuration::number_of_processes; i++) {
        if (i != process_index) {
            consumers.emplace_back(process_index, i);
        }
    }
    std::cout << process_index << ": ready\n";

    std::default_random_engine random_gen(std::random_device{}());
    std::uniform_int_distribution<unsigned> dist{1, 1'000'000};

    // Constantly increasing variable. The value is used to construct producers message
    uint64_t prod_value = 0;
    while (true) {
        for (int i = 0, num = consumers.size(); i < num; i++) {
            Consumer& consumer = consumers[i];
            if (consumer.HasMessage()) {
                Message* msg = consumer.LockMessage();
                std::cout << process_index << ": read info from " << consumer.producer_process_index
                          << ": " << msg->val << "\n";
                consumer.UnlockMessage();
            } else {
                std::cout << process_index << ": read info from " << consumer.producer_process_index
                          << ": empty\n";
            }
        }

        prod_value++;
        std::cout << process_index << ": write " << prod_value << "\n";
        producer.UpdateMessage(Message{prod_value});

        // sleep for random time
        std::this_thread::sleep_for(std::chrono::microseconds{dist(random_gen)});
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
// * unit tests?
