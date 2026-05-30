#include <gtest/gtest.h>
#include "snes/cpu65816.h"
#include "snes/snes_bus.h"
#include <array>
#include <cstring>

// ─── Мок-шина: 64 KB линейная память ─────────────────────────────────────────
class MockSnesBus : public SnesBus {
public:
    std::array<uint8_t, 0x10000> mem{};

    MockSnesBus() { mem.fill(0xEA); /* NOP */ }

    uint8_t read(uint32_t addr) override {
        return mem[addr & 0xFFFF];
    }
    void write(uint32_t addr, uint8_t data) override {
        mem[addr & 0xFFFF] = data;
    }

    // Запись слова (little-endian)
    void writeW(uint16_t addr, uint16_t v) {
        mem[addr]   = (uint8_t)(v & 0xFF);
        mem[addr+1] = (uint8_t)(v >> 8);
    }

    // Инструкция по адресу
    void writeCode(uint16_t addr, std::initializer_list<uint8_t> bytes) {
        uint16_t a = addr;
        for (uint8_t b : bytes) mem[a++] = b;
    }
};

// ─── Фикстура ─────────────────────────────────────────────────────────────────
class SnesCpuTest : public ::testing::Test {
protected:
    MockSnesBus bus;
    CPU65816    cpu;

    void SetUp() override {
        cpu.connectBus(&bus);
        // Вектор сброса $FFFC → $8000
        bus.writeW(0xFFFC, 0x8000);
        cpu.reset();
    }

    // Выполнить N инструкций
    void step(int n = 1) {
        for (int i = 0; i < n; i++) cpu.clock();
    }
};

// ─── Тест 1: reset загружает PC из вектора $FFFC ─────────────────────────────
TEST_F(SnesCpuTest, ResetLoadsPC) {
    EXPECT_EQ(cpu.PC,  0x8000u);
    EXPECT_EQ(cpu.PBR, 0x00u);
    EXPECT_TRUE(cpu.E);  // сброс всегда в emulation mode
}

// ─── Тест 2: XCE переключает emulation ↔ native ──────────────────────────────
TEST_F(SnesCpuTest, XCE_SwitchNativeEmulation) {
    // Код: CLC ($18), XCE ($FB) → переход в native mode
    bus.writeCode(0x8000, { 0x18, 0xFB });
    EXPECT_TRUE(cpu.E);

    step();  // CLC — C=0
    EXPECT_FALSE(cpu.P & CPU65816::FLAG_C);

    step();  // XCE — E=0 (native), C=1 (старый E)
    EXPECT_FALSE(cpu.E);
    EXPECT_TRUE(cpu.P & CPU65816::FLAG_C);  // бывший E=1 → C=1
}

// ─── Тест 3: REP #$30 сбрасывает флаги M и X (16-бит режим) ─────────────────
TEST_F(SnesCpuTest, REP_ClearsMX) {
    // Сначала уходим в native mode: CLC + XCE
    bus.writeCode(0x8000, { 0x18, 0xFB, 0xC2, 0x30 });
    step(); step();        // CLC, XCE → native
    EXPECT_FALSE(cpu.E);

    step();  // REP #$30 (сброс M=$20 и X=$10)
    EXPECT_FALSE(cpu.P & CPU65816::FLAG_M);   // A теперь 16-бит
    EXPECT_FALSE(cpu.P & CPU65816::FLAG_XB);  // X,Y теперь 16-бит
}

// ─── Тест 4: LDA #$1234 в 16-бит режиме ─────────────────────────────────────
TEST_F(SnesCpuTest, LDA_16bit_Immediate) {
    // native, REP #$30, LDA #$1234
    bus.writeCode(0x8000, { 0x18, 0xFB, 0xC2, 0x30, 0xA9, 0x34, 0x12 });
    step(); step(); step();  // CLC, XCE, REP #$30

    step();  // LDA #$1234
    EXPECT_EQ(cpu.A, 0x1234u);
    EXPECT_FALSE(cpu.P & CPU65816::FLAG_Z);
    EXPECT_FALSE(cpu.P & CPU65816::FLAG_N);
}

// ─── Тест 5: STA abs / LDA abs roundtrip ─────────────────────────────────────
TEST_F(SnesCpuTest, STA_LDA_Roundtrip) {
    // native, 16-бит, LDA #$ABCD, STA $0100, LDA #$0000, LDA $0100
    bus.writeCode(0x8000, {
        0x18, 0xFB,              // CLC, XCE → native
        0xC2, 0x30,              // REP #$30 → 16-bit
        0xA9, 0xCD, 0xAB,        // LDA #$ABCD
        0x8D, 0x00, 0x01,        // STA $0100
        0xA9, 0x00, 0x00,        // LDA #$0000
        0xAD, 0x00, 0x01,        // LDA $0100
    });
    step(); step(); step();  // CLC, XCE, REP
    step();  // LDA #$ABCD
    EXPECT_EQ(cpu.A, 0xABCDu);

    step();  // STA $0100
    EXPECT_EQ(bus.mem[0x0100], 0xCDu);
    EXPECT_EQ(bus.mem[0x0101], 0xABu);

    step();  // LDA #$0000
    EXPECT_EQ(cpu.A, 0x0000u);

    step();  // LDA $0100
    EXPECT_EQ(cpu.A, 0xABCDu);
}

// ─── Тест 6: BNE и BEQ (ветвления) ───────────────────────────────────────────
TEST_F(SnesCpuTest, BranchBNE_BEQ) {
    // LDA #$01 → Z=0 → BEQ +2 (не прыгает) → BNE +2 (прыгает)
    bus.writeCode(0x8000, {
        0xA9, 0x01,   // LDA #$01 (Z=0)
        0xF0, 0x02,   // BEQ +2  → не должен прыгнуть
        0xA9, 0xFF,   // LDA #$FF (должен выполниться)
        0xD0, 0x01,   // BNE +1  → должен прыгнуть, пропустить следующий байт
        0xEA,         // NOP (пропустить)
        0xEA,         // NOP (сюда прыгаем)
    });
    step();  // LDA #$01 → A=$01, Z=0

    step();  // BEQ +2 — Z=0, не прыгаем
    EXPECT_EQ(cpu.PC, 0x8004u);  // PC сразу после BEQ

    step();  // LDA #$FF → A=$FF
    EXPECT_EQ(cpu.A, 0xFFu);

    step();  // BNE +1 — Z=0 (A≠0), прыгаем
    EXPECT_EQ(cpu.PC, 0x8009u);  // пропустили $08 (NOP)
}

// ─── Тест 7: JSR / RTS ────────────────────────────────────────────────────────
TEST_F(SnesCpuTest, JSR_RTS) {
    // JSR $8010, NOP, NOP, ...
    // $8010: LDA #$42, RTS
    bus.writeCode(0x8000, {
        0x20, 0x10, 0x80,   // JSR $8010
        0xEA,               // NOP (возврат сюда)
        0xEA,               // NOP
    });
    bus.writeCode(0x8010, {
        0xA9, 0x42,         // LDA #$42
        0x60,               // RTS
    });

    step();  // JSR $8010 → PC=$8010, стек=[ret=$8002]
    EXPECT_EQ(cpu.PC, 0x8010u);

    step();  // LDA #$42 → A=$42
    EXPECT_EQ(cpu.A, 0x42u);

    step();  // RTS → PC=$8003
    EXPECT_EQ(cpu.PC, 0x8003u);
}
