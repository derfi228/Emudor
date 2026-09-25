#pragma once
#include <cstdint>

class N64System;

// ─── Процессор Nintendo 64: NEC VR4300 (MIPS III, 64 бита) ───────────────────
// 32 регистра общего назначения по 64 бита, HI/LO, сопроцессор 0 (управление:
// исключения, прерывания, TLB на 32 записи, таймер Count/Compare) и
// сопроцессор 1 (FPU: одинарная и двойная точность).
//
// Переходы с задержкой: команда после перехода (delay slot) исполняется всегда,
// у «likely»-вариантов — только если переход взят. Адреса — 32-битные сегменты
// kuseg/kseg0/kseg1/ksseg/kseg3, как их использует N64 (64-битные режимы
// адресации играми не применяются).
//
// Время: каждая команда — 2 такта процессора (93.75 МГц), Count растёт на 1.
// Кэши не моделируются, CACHE — пустая команда.
class Vr4300 {
public:
    static constexpr uint32_t CYCLES_PER_INSTR = 2;

    // ── Регистры ─────────────────────────────────────────────────────────────
    uint64_t gpr[32]{};
    uint64_t hi = 0, lo = 0;
    uint64_t pc = 0;                  // адрес следующей исполняемой команды
    uint64_t nextPc = 0;              // адрес команды после неё (или цель перехода)
    uint64_t fpr[32]{};               // регистры FPU, сырые биты
    uint32_t fcr31 = 0;               // управление/статус FPU
    uint64_t cop0[32]{};
    bool     llbit = false;

    // Номера регистров COP0
    enum : int {
        C0_INDEX = 0, C0_RANDOM = 1, C0_ENTRYLO0 = 2, C0_ENTRYLO1 = 3, C0_CONTEXT = 4,
        C0_PAGEMASK = 5, C0_WIRED = 6, C0_BADVADDR = 8, C0_COUNT = 9, C0_ENTRYHI = 10,
        C0_COMPARE = 11, C0_STATUS = 12, C0_CAUSE = 13, C0_EPC = 14, C0_PRID = 15,
        C0_CONFIG = 16, C0_LLADDR = 17, C0_WATCHLO = 18, C0_WATCHHI = 19,
        C0_XCONTEXT = 20, C0_PARITYERR = 26, C0_CACHEERR = 27, C0_TAGLO = 28,
        C0_TAGHI = 29, C0_ERROREPC = 30,
    };

    void connect(N64System* sys) { sys_ = sys; }
    void reset();                     // холодный старт: вектор 0xBFC00000
    void jump(uint64_t address);      // начать исполнение с адреса (загрузчик)

    // Исполнять, пока счётчик тактов не дойдёт до target. stopAt() может
    // приблизить границу (шина запланировала событие раньше).
    void runUntil(uint64_t target);
    void stopAt(uint64_t cycle) { if (cycle < runTarget_) runTarget_ = cycle; }
    uint64_t cycles() const { return cycles_; }

    // Прерывание от RCP (MI) — линия IP2 в Cause.
    void setRcpInterrupt(bool on);

    template<class S> void serialize(S& s);

private:
    N64System* sys_ = nullptr;
    uint64_t curPc_ = 0;              // адрес исполняемой сейчас команды
    bool     inDelaySlot_ = false;    // текущая команда — в слоте задержки
    bool     nextInDelay_ = false;    // следующая будет в слоте задержки
    uint64_t cycles_ = 0;             // такты CPU с включения
    uint64_t runTarget_ = 0;
    uint64_t randomBase_ = 0;         // команда, с которой Random считает от 31
    uint64_t cop0Latch_ = 0;          // последнее записанное в COP0 (читается из пустых регистров)
    uint64_t cop2Latch_ = 0;          // «регистр» отсутствующего COP2

    struct TlbEntry {
        uint64_t entryHi = 0;         // VPN2 и ASID (биты под маской страницы сброшены)
        uint32_t entryLo0 = 0, entryLo1 = 0;   // PFN, C, D, V (без G)
        uint32_t pageMask = 0;
        bool     global = false;
    };
    TlbEntry tlb_[32];

    // ── Исключения ───────────────────────────────────────────────────────────
    enum Exc : uint32_t {
        EXC_INT = 0, EXC_MOD = 1, EXC_TLBL = 2, EXC_TLBS = 3, EXC_ADEL = 4, EXC_ADES = 5,
        EXC_IBE = 6, EXC_DBE = 7, EXC_SYS = 8, EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11,
        EXC_OV = 12, EXC_TR = 13, EXC_FPE = 15, EXC_WATCH = 23,
    };
    void raise(Exc code, uint32_t copNum = 0, bool tlbRefill = false);
    void addressError(uint64_t vaddr, bool store);
    bool kernelMode() const;

    // ── Память ───────────────────────────────────────────────────────────────
    enum class Access { Read, Write, Fetch };
    bool translate(uint64_t vaddr, Access acc, uint32_t& paddr);
    bool tlbLookup(uint64_t vaddr, Access acc, uint32_t& paddr);
    void tlbException(uint64_t vaddr, Exc code, bool refill);
    bool load8 (uint64_t vaddr, uint8_t&  v);
    bool load16(uint64_t vaddr, uint16_t& v);
    bool load32(uint64_t vaddr, uint32_t& v);
    bool load64(uint64_t vaddr, uint64_t& v);
    bool store8 (uint64_t vaddr, uint8_t  v);
    bool store16(uint64_t vaddr, uint16_t v);
    bool store32(uint64_t vaddr, uint32_t v, uint32_t mask = 0xFFFFFFFFu);
    bool store64(uint64_t vaddr, uint64_t v, uint64_t mask = ~0ull);

    // ── Исполнение ───────────────────────────────────────────────────────────
    void step();
    void execute(uint32_t instr);
    void special(uint32_t instr);
    void regimm(uint32_t instr);
    void cop0Op(uint32_t instr);
    void cop1Op(uint32_t instr);
    void cop2Op(uint32_t instr);
    void loadStore(uint32_t instr);
    void branch(bool take, uint64_t target, bool likely);
    void jumpTo(uint64_t target);

    // COP0
    uint64_t readCop0(int reg) const;
    void     writeCop0(int reg, uint64_t v);
    void     tlbRead();
    void     tlbWrite(int index);
    void     tlbProbe();
    int      randomIndex() const;

    // COP1
    bool     cop1Usable();
    bool     fr() const { return (cop0[C0_STATUS] >> 26) & 1; }
    uint32_t getFpr32(int n) const;
    void     setFpr32(int n, uint32_t v);
    uint64_t getFpr64(int n) const;
    void     setFpr64(int n, uint64_t v);
    float    getF(int n) const;
    double   getD(int n) const;
    void     setF(int n, float v);
    void     setD(int n, double v);
    void     writeFcr31(uint32_t v);
    void     applyRounding() const;             // режим округления FCR31 → хост
    bool     fpuTrap(uint32_t cause);           // причины → FCR31; true = исключение
    enum class FpIn { Ok, Stop, Nan };          // норма / исключение / NaN в результат
    template<class F> FpIn fpuCheckIn(F v);     // денормал / NaN на входе
    template<class F> bool fpuFinish(F& r);     // флаги хоста, денормал на выходе
    template<class F> void fpuArith(uint32_t funct, int fd, int fs, int ft);
    template<class F> void fpuCompare(uint32_t cond, int fs, int ft);
    template<class F> void fpuToInt(uint32_t funct, int fd, int fs);
};
