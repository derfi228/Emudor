#pragma once
#include <cstdint>
#include <array>

class SnesBus;

// ─── WDC 65C816 ──────────────────────────────────────────────────────────────
// 16-битное расширение 6502, используется в SNES.
// Поддерживает native (16-бит) и emulation (6502-совместимый) режимы.
class CPU65816 {
public:
    // ─── Регистры ────────────────────────────────────────────────────────────
    uint16_t A   = 0;       // Аккумулятор (8 или 16 бит, флаг M)
    uint16_t X   = 0;       // Индекс X    (8 или 16 бит, флаг X)
    uint16_t Y   = 0;       // Индекс Y    (8 или 16 бит, флаг X)
    uint16_t SP  = 0x01FF;  // Указатель стека
    uint16_t PC  = 0;       // Счётчик команд
    uint16_t D   = 0;       // Direct Page (прямая страница)
    uint8_t  PBR = 0;       // Program Bank Register
    uint8_t  DBR = 0;       // Data Bank Register
    uint8_t  P   = 0x34;    // Флаги процессора
    bool     E   = true;    // true = emulation mode (6502-совместимый)

    uint64_t totalCycles_   = 0;
    int      pendingCycles_ = 0;  // тактов последней инструкции

    // Флаги регистра P
    enum Flag : uint8_t {
        FLAG_C  = (1 << 0),  // Carry
        FLAG_Z  = (1 << 1),  // Zero
        FLAG_I  = (1 << 2),  // IRQ Disable
        FLAG_D  = (1 << 3),  // Decimal mode
        FLAG_XB = (1 << 4),  // Index width (native) / Break (emulation)
        FLAG_M  = (1 << 5),  // Accum. width (native) / always 1 (emulation)
        FLAG_V  = (1 << 6),  // Overflow
        FLAG_N  = (1 << 7),  // Negative
    };

    CPU65816();
    void connectBus(SnesBus* bus);
    void reset();
    void clock();   // выполняет одну инструкцию, ставит pendingCycles_
    void nmi();
    void irq();

    bool stopped_ = false;  // STP: ожидание RESET
    bool waiting_ = false;  // WAI: ожидание прерывания

private:
    SnesBus* bus_ = nullptr;
    uint32_t addrAbs_ = 0;
    uint8_t  opcode_  = 0;

    // ─── Чтение/запись шины ─────────────────────────────────────────────────
    uint8_t  readByte (uint32_t addr);
    void     writeByte(uint32_t addr, uint8_t  data);
    uint16_t readWord (uint32_t addr);           // little-endian, wrap в банке
    void     writeWord(uint32_t addr, uint16_t data);
    uint32_t readLong (uint32_t addr);           // 3 байта little-endian

    // Чтение из потока команд (PBR:PC, PC++)
    uint8_t  fetchByte();
    uint16_t fetchWord();
    uint32_t fetchLong();

    // Стек
    void     push8 (uint8_t  v);
    uint8_t  pop8  ();
    void     push16(uint16_t v);
    uint16_t pop16 ();

    // Флаги
    bool flagM() const { return E || (bool)(P & FLAG_M);  }
    bool flagX() const { return E || (bool)(P & FLAG_XB); }
    void setFlag(Flag f, bool v);
    void setNZ8 (uint8_t  v);
    void setNZ16(uint16_t v);

    void doBranch(bool cond);  // вспомогательный переход для Bxx

    // ─── Режимы адресации ────────────────────────────────────────────────────
    void am_imp();      // Implied / Accumulator
    void am_immM();     // Immediate (M-зависимый: 1 или 2 байта)
    void am_immX();     // Immediate (X-зависимый: 1 или 2 байта)
    void am_imm8();     // Immediate 8-bit (REP/SEP/BRK/WDM)
    void am_dp();       // Direct Page
    void am_dpX();      // Direct Page,X
    void am_dpY();      // Direct Page,Y
    void am_dpInd();    // (Direct Page)
    void am_dpIndX();   // (Direct Page,X)
    void am_dpIndY();   // (Direct Page),Y
    void am_dpIndL();   // [Direct Page]
    void am_dpIndLY();  // [Direct Page],Y
    void am_abs();      // Absolute
    void am_absX();     // Absolute,X
    void am_absY();     // Absolute,Y
    void am_absL();     // Absolute Long
    void am_absLX();    // Absolute Long,X
    void am_absInd();   // (Absolute) — JMP
    void am_absIndL();  // [Absolute] — JML
    void am_absIndX();  // (Absolute,X) — JSR/JMP
    void am_sr();       // Stack Relative
    void am_srIndY();   // (Stack Relative),Y
    void am_rel();      // Relative 8-bit (branches)
    void am_relL();     // Relative 16-bit (BRL)
    void am_blk();      // Block Move (MVP/MVN)

    // ─── Опкоды ──────────────────────────────────────────────────────────────
    void op_ADC();    void op_AND();    void op_ASL();    void op_ASL_a();
    void op_BCC();    void op_BCS();    void op_BEQ();    void op_BIT();
    void op_BIT_imm();void op_BMI();   void op_BNE();    void op_BPL();
    void op_BRA();    void op_BRK();   void op_BRL();    void op_BVC();
    void op_BVS();    void op_CLC();   void op_CLD();    void op_CLI();
    void op_CLV();    void op_CMP();   void op_COP();    void op_CPX();
    void op_CPY();    void op_DEC();   void op_DEC_a();
    void op_DEX();    void op_DEY();   void op_EOR();
    void op_INC();    void op_INC_a();
    void op_INX();    void op_INY();
    void op_JML();    void op_JMP();   void op_JSL();    void op_JSR();
    void op_JSR_indX();
    void op_LDA();    void op_LDX();   void op_LDY();
    void op_LSR();    void op_LSR_a();
    void op_MVP();    void op_MVN();   void op_NOP();    void op_ORA();
    void op_PEA();    void op_PEI();   void op_PER();
    void op_PHA();    void op_PHB();   void op_PHD();    void op_PHK();
    void op_PHP();    void op_PHX();   void op_PHY();
    void op_PLA();    void op_PLB();   void op_PLD();    void op_PLP();
    void op_PLX();    void op_PLY();
    void op_REP();    void op_ROL();   void op_ROL_a();
    void op_ROR();    void op_ROR_a();
    void op_RTI();    void op_RTL();   void op_RTS();
    void op_SBC();    void op_SEC();   void op_SED();    void op_SEI();
    void op_SEP();    void op_STA();   void op_STP();    void op_STX();
    void op_STY();    void op_STZ();
    void op_TAX();    void op_TAY();   void op_TCS();    void op_TCD();
    void op_TDC();    void op_TRB();   void op_TSB();    void op_TSC();
    void op_TSX();    void op_TXA();   void op_TXS();    void op_TXY();
    void op_TYA();    void op_TYX();
    void op_WAI();    void op_WDM();   void op_XBA();    void op_XCE();
    void op_XXX();

    struct Instruction {
        const char*             name;
        void (CPU65816::*addrmode)();
        void (CPU65816::*operation)();
        uint8_t cycles;
    };
    std::array<Instruction, 256> lookup_;
    void buildLookupTable();
};
