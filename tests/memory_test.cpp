#include <gtest/gtest.h>
#include "memory/memory_bus.h"

TEST(MemoryTest, WriteAndReadRAM) {
    MemoryBus bus;
    bus.write(0x0010, 0xAB);
    EXPECT_EQ(bus.read(0x0010), 0xABu);
}

TEST(MemoryTest, RAMMirror_0800) {
    MemoryBus bus;
    bus.write(0x0010, 0x55);
    EXPECT_EQ(bus.read(0x0810), 0x55u);  // зеркало $0800
}

TEST(MemoryTest, RAMMirror_1000) {
    MemoryBus bus;
    bus.write(0x0020, 0x77);
    EXPECT_EQ(bus.read(0x1020), 0x77u);  // зеркало $1000
}

TEST(MemoryTest, RAMMirror_1800) {
    MemoryBus bus;
    bus.write(0x0005, 0x33);
    EXPECT_EQ(bus.read(0x1805), 0x33u);  // зеркало $1800
}
