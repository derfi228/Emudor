#pragma once
#include <cstdint>
#include <string>
#include <array>
#include <functional>

class MemoryBus;

class CPU {
public:
    // Регистры
    uint8_t  A  = 0;
    uint8_t  X  = 0;
    uint8_t  Y  = 0;
    uint8_t  SP = 0;
    uint8_t  P  = 0;
    uint16_t PC = 0;

    uint8_t  remainingCycles = 0;
    uint64_t totalCycles_    = 0;

    enum Flag : uint8_t {
        C = (1 << 0),  // Carry
        Z = (1 << 1),  // Zero
        I = (1 << 2),  // Interrupt Disable
        D = (1 << 3),  // Decimal (не используется в NES)
        B = (1 << 4),  // Break
        U = (1 << 5),  // Unused (всегда 1)
        V = (1 << 6),  // Overflow
        N = (1 << 7),  // Negative
    };

    CPU();
    void connectBus(MemoryBus* bus);

    void reset();
    void clock();
    void irq();
    void nmi();

    bool getFlag(Flag f) const;
    void setFlag(Flag f, bool v);

    // Трассировка в формате nestest.log
    std::string trace(int ppuScanline, int ppuDot);

private:
    MemoryBus* bus_ = nullptr;

    // Текущее состояние инструкции
    uint16_t addrAbs_  = 0;
    uint16_t addrRel_  = 0;
    uint8_t  opcode_   = 0;
    uint8_t  fetchedVal_ = 0;

    uint8_t read(uint16_t addr);
    void    write(uint16_t addr, uint8_t data);
    uint8_t fetch();

    void push(uint8_t val);
    uint8_t pop();

    // Режимы адресации (возвращают 1 при пересечении страницы)
    uint8_t IMP(); uint8_t ACC();
    uint8_t IMM(); uint8_t ZP0();
    uint8_t ZPX(); uint8_t ZPY();
    uint8_t REL(); uint8_t ABS();
    uint8_t ABX(); uint8_t ABY();
    uint8_t IND(); uint8_t IZX();
    uint8_t IZY();

    // Инструкции (возвращают 1 если нужен доп. цикл при page cross)
    uint8_t ADC(); uint8_t AND(); uint8_t ASL(); uint8_t BCC();
    uint8_t BCS(); uint8_t BEQ(); uint8_t BIT(); uint8_t BMI();
    uint8_t BNE(); uint8_t BPL(); uint8_t BRK(); uint8_t BVC();
    uint8_t BVS(); uint8_t CLC(); uint8_t CLD(); uint8_t CLI();
    uint8_t CLV(); uint8_t CMP(); uint8_t CPX(); uint8_t CPY();
    uint8_t DEC(); uint8_t DEX(); uint8_t DEY(); uint8_t EOR();
    uint8_t INC(); uint8_t INX(); uint8_t INY(); uint8_t JMP();
    uint8_t JSR(); uint8_t LDA(); uint8_t LDX(); uint8_t LDY();
    uint8_t LSR(); uint8_t NOP(); uint8_t ORA(); uint8_t PHA();
    uint8_t PHP(); uint8_t PLA(); uint8_t PLP(); uint8_t ROL();
    uint8_t ROR(); uint8_t RTI(); uint8_t RTS(); uint8_t SBC();
    uint8_t SEC(); uint8_t SED(); uint8_t SEI(); uint8_t STA();
    uint8_t STX(); uint8_t STY(); uint8_t TAX(); uint8_t TAY();
    uint8_t TSX(); uint8_t TXA(); uint8_t TXS(); uint8_t TYA();
    uint8_t XXX();  // нелегальный опкод

    struct Instruction {
        const char* name;
        uint8_t (CPU::*operate)();
        uint8_t (CPU::*addrmode)();
        uint8_t cycles;
    };

    std::array<Instruction, 256> lookup_;
    void buildLookupTable();
    bool isAccumulatorMode_ = false;
};
