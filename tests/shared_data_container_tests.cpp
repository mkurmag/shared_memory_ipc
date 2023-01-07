#include <shared_data_container.h>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Lock empty") {
    SharedDataContainer shd;
    REQUIRE_THROWS(shd.ReaderLock(0));
}
TEST_CASE("empty") {
    SharedDataContainer shd;
    REQUIRE(shd.IsEmpty());
}

TEST_CASE("Double lock of the same slot by the same process") {
    SharedDataContainer shd;
    shd.WriterUpdateMessage(Message{5});
    shd.ReaderLock(0);
    REQUIRE_THROWS(shd.ReaderLock(0));
}

TEST_CASE("Several locks by several processes") {
    SharedDataContainer shd;
    shd.WriterUpdateMessage(Message{5});
    auto handle0 = shd.ReaderLock(0);
    auto handle1 = shd.ReaderLock(1);
    auto handle2 = shd.ReaderLock(2);
    REQUIRE_NOTHROW(shd.ReaderUnlock(1, handle1));
    REQUIRE_NOTHROW(shd.ReaderUnlock(0, handle0));
    REQUIRE_NOTHROW(shd.ReaderUnlock(2, handle2));
}

TEST_CASE("Lock several slots by the same process") {
    SharedDataContainer shd;
    shd.WriterUpdateMessage(Message{10});
    auto handle1 = shd.ReaderLock(0);
    shd.WriterUpdateMessage(Message{20});
    auto handle2 = shd.ReaderLock(0);
    shd.WriterUpdateMessage(Message{30});

    REQUIRE(10 == (*shd.ReaderGetMessage(handle1)).val);
    REQUIRE(20 == (*shd.ReaderGetMessage(handle2)).val);

    REQUIRE_NOTHROW(shd.ReaderUnlock(0, handle1));
    REQUIRE_NOTHROW(shd.ReaderUnlock(0, handle2));
}

TEST_CASE("Unlock another's handle for the same slot") {
    SharedDataContainer shd;
    shd.WriterUpdateMessage(Message{5});
    auto handle1 = shd.ReaderLock(0);
    auto handle2 = shd.ReaderLock(1);
    REQUIRE_NOTHROW(shd.ReaderUnlock(0, handle2));
    REQUIRE_NOTHROW(shd.ReaderUnlock(1, handle1));
}

TEST_CASE("Unlock another's handle for another slot") {
    SharedDataContainer shd;
    shd.WriterUpdateMessage(Message{5});
    auto handle1 = shd.ReaderLock(0);
    shd.WriterUpdateMessage(Message{6});
    auto handle2 = shd.ReaderLock(1);
    REQUIRE_THROWS(shd.ReaderUnlock(0, handle2));
    REQUIRE_THROWS(shd.ReaderUnlock(1, handle1));
}

TEST_CASE("Writes when all readers has a single locked slot") {
    SharedDataContainer shd;
    std::vector<int> handles(Configuration::number_of_processes);
    // Assume process-producer has index `Configuration::number_of_processes - 1`
    for (unsigned i = 0; i < Configuration::number_of_processes - 1; i++) {
        shd.WriterUpdateMessage(Message{i * 10});
        handles[i] = shd.ReaderLock(i);
    }
    REQUIRE_NOTHROW(shd.WriterUpdateMessage(Message{1}));
    REQUIRE_NOTHROW(shd.WriterUpdateMessage(Message{2}));

    for (unsigned i = 0; i < Configuration::number_of_processes - 1; i++) {
        REQUIRE_NOTHROW(shd.ReaderUnlock(i, handles[i]));
    }
}

// This is serious problem! API of SharedDataContainer allows this situation.
// To not overcomplicate the SharedDataContainer, this is checked inside Consumer class.
TEST_CASE("Writes when one process locked all slots") {
    SharedDataContainer shd;
    std::vector<int> handles(Configuration::number_of_processes);
    for (unsigned i = 0; i < Configuration::number_of_processes+1; i++) {
        shd.WriterUpdateMessage(Message{i * 10});
        shd.ReaderLock(0);
    }
    // No empty slot :(
    REQUIRE_THROWS(shd.WriterUpdateMessage(Message{1}));
}
