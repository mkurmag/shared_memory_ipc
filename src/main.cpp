#include <config.h>
#include <message.h>
#include <shared_data_container.h>

#include <algorithm>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <chrono>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

namespace bipc = boost::interprocess;

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
    int producer_process_index;  // index of process-producer

private:
    bipc::shared_memory_object shared_mem_obj_;
    bipc::mapped_region mem_region_;
    int locked_message_handle_ = -1;
    int process_index_;  // Index of current process
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
