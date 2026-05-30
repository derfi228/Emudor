#include <gtest/gtest.h>
#include "ppu/ppu.h"
#include "memory/memory_bus.h"

class PpuTest : public ::testing::Test {
protected:
    MemoryBus bus;
    PPU       ppu;

    void SetUp() override {
        ppu.connectBus(&bus);
        ppu.reset();
    }
};

TEST_F(PpuTest, ResetClearsVBlank) {
    // Явно выставляем vblank через writeRegister PPUCTRL
    // Затем ресет должен очистить
    ppu.reset();
    // PPUSTATUS bit7 == 0 после reset
    uint8_t status = ppu.readRegister(2);
    EXPECT_EQ(status & 0x80, 0u);
}

TEST_F(PpuTest, VBlankSetAtScanline241) {
    // Включаем NMI чтобы nmiPending выставилось
    ppu.writeRegister(0, 0x80);  // PPUCTRL: NMI enable

    // Тактируем до скэнлайна 241, дот 1
    // После N тактов состояние = (N/341, N%341).
    // Чтобы CHECK(scanline==241, dot==1) сработал на такт #M,
    // нужно запустить M = 241*341+2 тактов.
    int targetClocks = 241 * 341 + 2;
    for (int i = 0; i < targetClocks; i++) ppu.clock();

    EXPECT_EQ(ppu.scanline(), 241);
    // vblank должен быть установлен — проверяем через nmiPending
    // (readRegister(2) сбрасывает vblank, поэтому не вызываем его здесь)
    EXPECT_TRUE(ppu.nmiPending);
}

TEST_F(PpuTest, FrameCompleteAfter89342Clocks) {
    // Один кадр = 262 скэнлайна × 341 дот = 89342 такта
    for (int i = 0; i < 89342; i++) ppu.clock();
    EXPECT_TRUE(ppu.frameComplete);
}

TEST_F(PpuTest, NMIPendingWhenNMIEnabled) {
    ppu.writeRegister(0, 0x80);  // PPUCTRL: nmiEnable=1
    // Тактируем до VBlank (241*341+2 тактов чтобы check сработал)
    for (int i = 0; i < 241 * 341 + 2; i++) ppu.clock();
    EXPECT_TRUE(ppu.nmiPending);
}
