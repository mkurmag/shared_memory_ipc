#ifndef _SHARED_DATA_CONTAINER_H_
#define _SHARED_DATA_CONTAINER_H_

#include <atomic>
#include <stdexcept>

#include <config.h>
#include <message.h>

// All class members are zero initialized. This allows to skip constructor call
// when class instance is created in the zero initialized memory.
class SharedDataContainer {
public:
    bool IsEmpty() const {
        return current_slot_id_ == 0;
    }

    // Locks slot with the most recent message by process with index `process_index`.
    // Slot won't be emptied until corresponding unlock by the same process.
    // Locks can't be nested, and the same slot can be locked by multiple processes.
    //
    // Function returns a handle to get message and to unlock it. Handle is valid until
    // `ReaderUnlock` call.
    //
    // Precondition: Single process should not lock several slots at the same time. This is not
    // checked here. This should be checked by the class user.
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
            if (current_value & (1 << process_index)) {
                throw std::runtime_error("ReaderLockDouble lock by the same process");
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
    // Assuming that a single process won't lock multiple slots, N+1 slots allow to always have an
    // unused slot to write to. In the worst case all readers (N-1) lock
    // different slots with old messages, Nth slot is used for current message, and one more is
    // needed to write new message without overriding current.
    Slot slots[Configuration::number_of_processes + 1];
};

#endif
