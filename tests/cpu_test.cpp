#include <gtest/gtest.h>
#include "cpu/cpu.h"
#include "memory/memory_bus.h"
#include <array>

// TestBus — изолированная шина с плоской памятью 64KB
class TestBus : public MemoryBus {
public:
    std::array<uint8_t, 0x10000> mem = {};

    uint8_t read(uint16_t addr, bool /*readOnly*/ = false) override {
        return mem[addr];
    }
    void write(uint16_t addr, uint8_t data) override {
        mem[addr] = data;
    }
};

class CpuTest : public ::testing::Test {
protected:
    TestBus bus;
    CPU     cpu;

    void SetUp() override {
        cpu.connectBus(&bus);
        // Вектор reset на $8000
        bus.mem[0xFFFC] = 0x00;
        bus.mem[0xFFFD] = 0x80;
        cpu.reset();
        // Пропустить 7 стартовых циклов
        for (int i = 0; i < 7; i++) cpu.clock();
    }

    // Запустить одну инструкцию (ждём пока remainingCycles == 0)
    void step() {
        do { cpu.clock(); } while (cpu.remainingCycles > 0);
    }
};

// ─── Reset ───────────────────────────────────────────────────────────────────

TEST_F(CpuTest, ResetLoadsPC) {
    bus.mem[0xFFFC] = 0x34;
    bus.mem[0xFFFD] = 0x12;
    cpu.reset();
    for (int i = 0; i < 7; i++) cpu.clock();
    EXPECT_EQ(cpu.PC, 0x1234u);
}

TEST_F(CpuTest, ResetSetsStackPointer) {
    EXPECT_EQ(cpu.SP, 0xFDu);
}

TEST_F(CpuTest, ResetSetsCycles) {
    // reset() инициализирует totalCycles_ = 7 (стартовые такты 6502)
    // SetUp уже вызвал reset + 7 clock = 14. Делаем свежий reset.
    CPU fresh;
    TestBus freshBus;
    fresh.connectBus(&freshBus);
    freshBus.mem[0xFFFC] = 0x00;
    freshBus.mem[0xFFFD] = 0x80;
    fresh.reset();
    EXPECT_EQ(fresh.totalCycles_, 7u);
}

// ─── Флаги ───────────────────────────────────────────────────────────────────

TEST_F(CpuTest, SEI_SetsInterruptFlag) {
    cpu.setFlag(CPU::I, false);
    bus.mem[0x8000] = 0x78;  // SEI
    step();
    EXPECT_TRUE(cpu.getFlag(CPU::I));
}

TEST_F(CpuTest, CLC_ClearsCarryFlag) {
    cpu.setFlag(CPU::C, true);
    bus.mem[0x8000] = 0x18;  // CLC
    step();
    EXPECT_FALSE(cpu.getFlag(CPU::C));
}

// ─── NOP ─────────────────────────────────────────────────────────────────────

TEST_F(CpuTest, NOP_AdvancesPC) {
    uint16_t before = cpu.PC;
    bus.mem[cpu.PC] = 0xEA;  // NOP
    step();
    EXPECT_EQ(cpu.PC, before + 1);
}

// ─── LDA ─────────────────────────────────────────────────────────────────────

TEST_F(CpuTest, LDA_IMM_LoadsAccumulator) {
    bus.mem[cpu.PC]   = 0xA9;  // LDA #imm
    bus.mem[cpu.PC+1] = 0x42;
    step();
    EXPECT_EQ(cpu.A, 0x42u);
}

TEST_F(CpuTest, LDA_IMM_SetsZeroFlag) {
    bus.mem[cpu.PC]   = 0xA9;
    bus.mem[cpu.PC+1] = 0x00;
    step();
    EXPECT_TRUE(cpu.getFlag(CPU::Z));
    EXPECT_FALSE(cpu.getFlag(CPU::N));
}

TEST_F(CpuTest, LDA_IMM_SetsNegativeFlag) {
    bus.mem[cpu.PC]   = 0xA9;
    bus.mem[cpu.PC+1] = 0x80;
    step();
    EXPECT_TRUE(cpu.getFlag(CPU::N));
    EXPECT_FALSE(cpu.getFlag(CPU::Z));
}

// ─── ADC ─────────────────────────────────────────────────────────────────────

TEST_F(CpuTest, ADC_AddsWithCarry) {
    cpu.A = 0x50;
    cpu.setFlag(CPU::C, false);
    bus.mem[cpu.PC]   = 0x69;  // ADC #imm
    bus.mem[cpu.PC+1] = 0x50;
    step();
    EXPECT_EQ(cpu.A, 0xA0u);
    EXPECT_FALSE(cpu.getFlag(CPU::C));
    EXPECT_TRUE(cpu.getFlag(CPU::V));   // 0x50+0x50 переполнение
    EXPECT_TRUE(cpu.getFlag(CPU::N));
}

TEST_F(CpuTest, ADC_SetsCarry) {
    cpu.A = 0xFF;
    bus.mem[cpu.PC]   = 0x69;  // ADC #01
    bus.mem[cpu.PC+1] = 0x01;
    step();
    EXPECT_EQ(cpu.A, 0x00u);
    EXPECT_TRUE(cpu.getFlag(CPU::C));
    EXPECT_TRUE(cpu.getFlag(CPU::Z));
}

// ─── SBC ─────────────────────────────────────────────────────────────────────

TEST_F(CpuTest, SBC_SubtractsWithBorrow) {
    cpu.A = 0x50;
    cpu.setFlag(CPU::C, true);  // no borrow
    bus.mem[cpu.PC]   = 0xE9;   // SBC #imm
    bus.mem[cpu.PC+1] = 0x30;
    step();
    EXPECT_EQ(cpu.A, 0x20u);
    EXPECT_TRUE(cpu.getFlag(CPU::C));
    EXPECT_FALSE(cpu.getFlag(CPU::V));
}

// ─── Ветвления ───────────────────────────────────────────────────────────────

TEST_F(CpuTest, BEQ_BranchTaken) {
    uint16_t pc = cpu.PC;
    cpu.setFlag(CPU::Z, true);
    bus.mem[pc]   = 0xF0;  // BEQ +4
    bus.mem[pc+1] = 0x04;
    step();
    EXPECT_EQ(cpu.PC, pc + 2 + 4);
}

TEST_F(CpuTest, BEQ_BranchNotTaken) {
    uint16_t pc = cpu.PC;
    cpu.setFlag(CPU::Z, false);
    bus.mem[pc]   = 0xF0;  // BEQ
    bus.mem[pc+1] = 0x04;
    step();
    EXPECT_EQ(cpu.PC, pc + 2);
}

// ─── JSR / RTS ───────────────────────────────────────────────────────────────

TEST_F(CpuTest, JSR_PushesReturnAddress) {
    uint16_t pc = cpu.PC;
    bus.mem[pc]   = 0x20;  // JSR $9000
    bus.mem[pc+1] = 0x00;
    bus.mem[pc+2] = 0x90;
    step();
    // На стеке должен быть pc+2 (PC-1 после инкремента)
    uint8_t lo = bus.mem[0x0100 + (uint8_t)(cpu.SP + 1)];
    uint8_t hi = bus.mem[0x0100 + (uint8_t)(cpu.SP + 2)];
    uint16_t ret = (hi << 8) | lo;
    EXPECT_EQ(ret, pc + 2);
    EXPECT_EQ(cpu.PC, 0x9000u);
}

TEST_F(CpuTest, RTS_RestoresPC) {
    uint16_t pc = cpu.PC;
    // JSR $9000
    bus.mem[pc]   = 0x20;
    bus.mem[pc+1] = 0x00;
    bus.mem[pc+2] = 0x90;
    step();
    // RTS в $9000
    bus.mem[0x9000] = 0x60;
    step();
    EXPECT_EQ(cpu.PC, pc + 3);
}
