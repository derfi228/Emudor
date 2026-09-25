// n64_cpu.cpp — процессор NEC VR4300.
#include "n64_cpu.h"
#include "n64_system.h"
#include "console/state_io.h"
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {

inline uint64_t sext32(uint32_t v) { return (uint64_t)(int64_t)(int32_t)v; }
inline uint64_t sext16(uint16_t v) { return (uint64_t)(int64_t)(int16_t)v; }
inline uint64_t sext8 (uint8_t  v) { return (uint64_t)(int64_t)(int8_t)v;  }

// Биты Status
constexpr uint64_t ST_IE  = 1u << 0;
constexpr uint64_t ST_EXL = 1u << 1;
constexpr uint64_t ST_ERL = 1u << 2;
constexpr uint64_t ST_BEV = 1u << 22;
constexpr uint64_t ST_CU0 = 1u << 28;
constexpr uint64_t ST_CU1 = 1u << 29;
constexpr uint64_t ST_CU2 = 1u << 30;

// Биты причин FCR31 (сдвиг от 12): I U O Z V E
constexpr uint32_t FC_I = 1, FC_U = 2, FC_O = 4, FC_Z = 8, FC_V = 16, FC_E = 32;

// Маска VPN2 + R в EntryHi (32-битный процессор: 40 бит виртуального адреса)
constexpr uint64_t VPN2_MASK = 0xC00000FFFFFFE000ull;

template<class F> struct FBits;
template<> struct FBits<float>  { using U = uint32_t; static constexpr U defaultNan = 0x7FBFFFFFu;
                                  static constexpr U quietBit = 1u << 22; };
template<> struct FBits<double> { using U = uint64_t; static constexpr U defaultNan = 0x7FF7FFFFFFFFFFFFull;
                                  static constexpr U quietBit = 1ull << 51; };

template<class F> typename FBits<F>::U bitsOf(F v) { typename FBits<F>::U u; std::memcpy(&u, &v, sizeof u); return u; }
template<class F> F fromBits(typename FBits<F>::U u) { F v; std::memcpy(&v, &u, sizeof v); return v; }

// Округление к ближайшему чётному без оглядки на режим хоста.
template<class F> F roundEven(F v)
{
    const F r = std::floor(v + F(0.5));
    if (r - v == F(0.5) && std::fmod(r, F(2)) != 0) return r - 1;
    return r;
}

} // namespace

// ─── Сброс ────────────────────────────────────────────────────────────────────
void Vr4300::reset()
{
    for (auto& r : gpr) r = 0;
    for (auto& r : fpr) r = 0;
    for (auto& r : cop0) r = 0;
    hi = lo = 0;
    fcr31 = 0;
    llbit = false;
    cop0[C0_STATUS] = 0x00400004;     // BEV, ERL
    cop0[C0_CONFIG] = 0x7006E463;
    cop0[C0_PRID]   = 0x00000B22;
    // После включения TLB ни с чем не совпадает: записи смотрят в kseg0,
    // который через TLB не транслируется.
    for (int i = 0; i < 32; ++i) {
        tlb_[i] = TlbEntry{};
        tlb_[i].entryHi = 0xFFFFFFFF80000000ull + (uint64_t)i * 0x2000;
    }
    randomBase_ = cycles_ / CYCLES_PER_INSTR;
    cop0Latch_ = cop2Latch_ = 0;
    jump(0xFFFFFFFFBFC00000ull);
}

void Vr4300::jump(uint64_t address)
{
    pc = address;
    nextPc = address + 4;
    inDelaySlot_ = nextInDelay_ = false;
}

void Vr4300::setRcpInterrupt(bool on)
{
    if (on) cop0[C0_CAUSE] |= 0x400;
    else    cop0[C0_CAUSE] &= ~0x400ull;
}

bool Vr4300::kernelMode() const
{
    const uint64_t st = cop0[C0_STATUS];
    return (st & (ST_EXL | ST_ERL)) || ((st >> 3) & 3) == 0;
}

// ─── Главный цикл ─────────────────────────────────────────────────────────────
void Vr4300::runUntil(uint64_t target)
{
    // Режим округления FPU — у хоста на время исполнения, потом как было.
    const int hostRounding = std::fegetround();
    applyRounding();
    runTarget_ = target;
    while (cycles_ < runTarget_) {
        step();
        cycles_ += CYCLES_PER_INSTR;
        const uint32_t count = (uint32_t)cop0[C0_COUNT] + CYCLES_PER_INSTR / 2;
        cop0[C0_COUNT] = count;
        if (count == (uint32_t)cop0[C0_COMPARE]) cop0[C0_CAUSE] |= 0x8000;   // IP7
    }
    std::fesetround(hostRounding);
}

void Vr4300::step()
{
    curPc_ = pc;
    inDelaySlot_ = nextInDelay_;
    nextInDelay_ = false;

    // Прерывания: IE=1, EXL=0, ERL=0 и есть незамаскированная линия.
    const uint64_t status = cop0[C0_STATUS];
    if ((cop0[C0_CAUSE] & status & 0xFF00) && (status & 7) == ST_IE) {
        raise(EXC_INT);
        return;
    }

    if (curPc_ & 3) { addressError(curPc_, false); return; }
    uint32_t pa;
    const uint32_t a = (uint32_t)curPc_;
    if ((a & 0xC0000000u) == 0x80000000u) pa = a & 0x1FFFFFFFu;
    else if (!tlbLookup(curPc_, Access::Fetch, pa)) return;
    const uint32_t instr = sys_->read32(pa);

    pc = nextPc;
    nextPc = pc + 4;
    execute(instr);
    gpr[0] = 0;
}

// ─── Исключения ───────────────────────────────────────────────────────────────
void Vr4300::raise(Exc code, uint32_t copNum, bool tlbRefill)
{
    uint64_t& status = cop0[C0_STATUS];
    uint64_t& cause  = cop0[C0_CAUSE];
    const bool exl = status & ST_EXL;
    if (!exl) {
        cop0[C0_EPC] = inDelaySlot_ ? curPc_ - 4 : curPc_;
        if (inDelaySlot_) cause |= 0x80000000ull;
        else              cause &= ~0x80000000ull;
        status |= ST_EXL;
    }
    cause = (cause & ~0x3000007Cull) | ((uint64_t)code << 2) | ((uint64_t)copNum << 28);
    uint64_t vector = (status & ST_BEV) ? 0xFFFFFFFFBFC00200ull : 0xFFFFFFFF80000000ull;
    if (!tlbRefill || exl) {
        vector += 0x180;
    } else {
        // Промах TLB в 64-битном режиме адресации (KX/SX/UX для текущего
        // режима) уходит на вектор XTLB 0x080.
        const uint32_t ksu = (status & (ST_EXL | ST_ERL)) ? 0 : (uint32_t)(status >> 3) & 3;
        const uint64_t xbit = ksu == 0 ? (1u << 7) : ksu == 1 ? (1u << 6) : (1u << 5);
        if (status & xbit) vector += 0x080;
    }
    jump(vector);
}

// Как и промах TLB, пишет BadVPN2 в Context/XContext (EntryHi не трогает).
void Vr4300::addressError(uint64_t vaddr, bool store)
{
    cop0[C0_BADVADDR] = vaddr;
    cop0[C0_CONTEXT]  = (cop0[C0_CONTEXT] & ~0x7FFFFFull) | ((vaddr >> 9) & 0x7FFFF0);
    cop0[C0_XCONTEXT] = (cop0[C0_XCONTEXT] & ~0x1FFFFFFFFull) | ((vaddr >> 9) & 0x7FFFFFF0)
                      | ((vaddr >> 62 & 3) << 31);
    raise(store ? EXC_ADES : EXC_ADEL);
}

// ─── Трансляция адресов ───────────────────────────────────────────────────────
inline bool Vr4300::translate(uint64_t vaddr, Access acc, uint32_t& paddr)
{
    const uint32_t a = (uint32_t)vaddr;
    if ((a & 0xC0000000u) == 0x80000000u) { paddr = a & 0x1FFFFFFFu; return true; }  // kseg0/kseg1
    return tlbLookup(vaddr, acc, paddr);
}

// ponytail: линейный поиск по 32 записям; кэш последнего попадания — если
// игры с TLB (GoldenEye, Perfect Dark) окажутся медленными.
bool Vr4300::tlbLookup(uint64_t vaddr, Access acc, uint32_t& paddr)
{
    const uint32_t a = (uint32_t)vaddr;
    if (a < 0x80000000u && (cop0[C0_STATUS] & ST_ERL)) { paddr = a; return true; }   // kuseg при ERL

    const uint64_t va = sext32(a);
    const uint64_t asid = cop0[C0_ENTRYHI] & 0xFF;
    for (const TlbEntry& e : tlb_) {
        const uint64_t mask = ~((uint64_t)e.pageMask | 0x1FFF) & VPN2_MASK;
        if ((va & mask) != (e.entryHi & mask)) continue;
        if (!e.global && (e.entryHi & 0xFF) != asid) continue;
        const uint32_t offMask = (e.pageMask | 0x1FFFu) >> 1;     // смещение внутри половины
        const uint32_t lo = (a & (offMask + 1)) ? e.entryLo1 : e.entryLo0;
        const bool store = acc == Access::Write;
        if (!(lo & 2)) { tlbException(vaddr, store ? EXC_TLBS : EXC_TLBL, false); return false; }
        if (store && !(lo & 4)) { tlbException(vaddr, EXC_MOD, false); return false; }
        paddr = ((((lo >> 6) & 0xFFFFFu) << 12) & ~offMask) | (a & offMask);
        return true;
    }
    tlbException(vaddr, acc == Access::Write ? EXC_TLBS : EXC_TLBL, true);
    return false;
}

void Vr4300::tlbException(uint64_t vaddr, Exc code, bool refill)
{
    const uint64_t va = sext32((uint32_t)vaddr);
    cop0[C0_BADVADDR] = va;
    cop0[C0_CONTEXT]  = (cop0[C0_CONTEXT] & ~0x7FFFFFull) | ((va >> 9) & 0x7FFFF0);
    cop0[C0_XCONTEXT] = (cop0[C0_XCONTEXT] & ~0x1FFFFFFFFull) | ((va >> 9) & 0x7FFFFFF0)
                      | ((va >> 62 & 3) << 31);
    cop0[C0_ENTRYHI]  = (va & VPN2_MASK) | (cop0[C0_ENTRYHI] & 0xFF);
    raise(code, 0, refill);
}

// ─── Чтение и запись памяти ───────────────────────────────────────────────────
bool Vr4300::load8(uint64_t vaddr, uint8_t& v)
{
    uint32_t pa;
    if (!translate(vaddr, Access::Read, pa)) return false;
    v = sys_->read8(pa);
    return true;
}

bool Vr4300::load16(uint64_t vaddr, uint16_t& v)
{
    if (vaddr & 1) { addressError(vaddr, false); return false; }
    uint32_t pa;
    if (!translate(vaddr, Access::Read, pa)) return false;
    v = sys_->read16(pa);
    return true;
}

bool Vr4300::load32(uint64_t vaddr, uint32_t& v)
{
    if (vaddr & 3) { addressError(vaddr, false); return false; }
    uint32_t pa;
    if (!translate(vaddr, Access::Read, pa)) return false;
    v = sys_->read32(pa);
    return true;
}

bool Vr4300::load64(uint64_t vaddr, uint64_t& v)
{
    if (vaddr & 7) { addressError(vaddr, false); return false; }
    uint32_t pa;
    if (!translate(vaddr, Access::Read, pa)) return false;
    v = sys_->read64(pa);
    return true;
}

bool Vr4300::store8(uint64_t vaddr, uint8_t v)
{
    uint32_t pa;
    if (!translate(vaddr, Access::Write, pa)) return false;
    sys_->write8(pa, v);
    return true;
}

bool Vr4300::store16(uint64_t vaddr, uint16_t v)
{
    if (vaddr & 1) { addressError(vaddr, true); return false; }
    uint32_t pa;
    if (!translate(vaddr, Access::Write, pa)) return false;
    sys_->write16(pa, v);
    return true;
}

bool Vr4300::store32(uint64_t vaddr, uint32_t v, uint32_t mask)
{
    if (vaddr & 3) { addressError(vaddr, true); return false; }
    uint32_t pa;
    if (!translate(vaddr, Access::Write, pa)) return false;
    sys_->write32(pa, v, mask);
    return true;
}

bool Vr4300::store64(uint64_t vaddr, uint64_t v, uint64_t mask)
{
    if (vaddr & 7) { addressError(vaddr, true); return false; }
    uint32_t pa;
    if (!translate(vaddr, Access::Write, pa)) return false;
    sys_->write64(pa, v, mask);
    return true;
}

// ─── Переходы ─────────────────────────────────────────────────────────────────
// Сейчас pc указывает на слот задержки (curPc_ + 4).
void Vr4300::branch(bool take, uint64_t target, bool likely)
{
    if (take) {
        nextPc = target;
        nextInDelay_ = true;
    } else if (likely) {
        pc = nextPc;               // слот задержки аннулируется
        nextPc = pc + 4;
    } else {
        nextInDelay_ = true;
    }
}

void Vr4300::jumpTo(uint64_t target)
{
    nextPc = target;
    nextInDelay_ = true;
}

// ─── Декодер ──────────────────────────────────────────────────────────────────
void Vr4300::execute(uint32_t instr)
{
    const uint32_t op = instr >> 26;
    const int rs = (instr >> 21) & 31;
    const int rt = (instr >> 16) & 31;
    const uint64_t imm  = sext16((uint16_t)instr);
    const uint64_t uimm = instr & 0xFFFF;
    const uint64_t btarget = curPc_ + 4 + (imm << 2);

    switch (op) {
    case 0x00: special(instr); break;
    case 0x01: regimm(instr); break;
    case 0x02: jumpTo((pc & ~0x0FFFFFFFull) | ((uint64_t)(instr & 0x03FFFFFF) << 2)); break;   // J
    case 0x03:                                                                                   // JAL
        gpr[31] = curPc_ + 8;
        jumpTo((pc & ~0x0FFFFFFFull) | ((uint64_t)(instr & 0x03FFFFFF) << 2));
        break;
    case 0x04: branch(gpr[rs] == gpr[rt], btarget, false); break;              // BEQ
    case 0x05: branch(gpr[rs] != gpr[rt], btarget, false); break;              // BNE
    case 0x06: branch((int64_t)gpr[rs] <= 0, btarget, false); break;           // BLEZ
    case 0x07: branch((int64_t)gpr[rs] > 0, btarget, false); break;            // BGTZ
    case 0x08: {                                                               // ADDI
        const int32_t x = (int32_t)gpr[rs], y = (int32_t)imm;
        const int32_t r = (int32_t)((uint32_t)x + (uint32_t)y);
        if (((x ^ r) & (y ^ r)) < 0) { raise(EXC_OV); break; }
        gpr[rt] = sext32((uint32_t)r);
        break;
    }
    case 0x09: gpr[rt] = sext32((uint32_t)gpr[rs] + (uint32_t)imm); break;     // ADDIU
    case 0x0A: gpr[rt] = (int64_t)gpr[rs] < (int64_t)imm; break;               // SLTI
    case 0x0B: gpr[rt] = gpr[rs] < imm; break;                                 // SLTIU
    case 0x0C: gpr[rt] = gpr[rs] & uimm; break;                                // ANDI
    case 0x0D: gpr[rt] = gpr[rs] | uimm; break;                                // ORI
    case 0x0E: gpr[rt] = gpr[rs] ^ uimm; break;                                // XORI
    case 0x0F: gpr[rt] = sext32((uint32_t)(uimm << 16)); break;                // LUI
    case 0x10: cop0Op(instr); break;
    case 0x11: cop1Op(instr); break;
    case 0x12: cop2Op(instr); break;
    case 0x14: branch(gpr[rs] == gpr[rt], btarget, true); break;               // BEQL
    case 0x15: branch(gpr[rs] != gpr[rt], btarget, true); break;               // BNEL
    case 0x16: branch((int64_t)gpr[rs] <= 0, btarget, true); break;            // BLEZL
    case 0x17: branch((int64_t)gpr[rs] > 0, btarget, true); break;             // BGTZL
    case 0x18: {                                                               // DADDI
        const int64_t x = (int64_t)gpr[rs], y = (int64_t)imm;
        const int64_t r = (int64_t)((uint64_t)x + (uint64_t)y);
        if (((x ^ r) & (y ^ r)) < 0) { raise(EXC_OV); break; }
        gpr[rt] = (uint64_t)r;
        break;
    }
    case 0x19: gpr[rt] = gpr[rs] + imm; break;                                 // DADDIU
    case 0x2F: break;                                                          // CACHE
    case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E:
    case 0x30: case 0x31: case 0x34: case 0x35: case 0x37:
    case 0x38: case 0x39: case 0x3C: case 0x3D: case 0x3F:
        loadStore(instr);
        break;
    case 0x32: case 0x36: case 0x3A: case 0x3E:                                // LWC2/LDC2/SWC2/SDC2
        if (!(cop0[C0_STATUS] & ST_CU2)) raise(EXC_CPU, 2);
        else raise(EXC_RI);
        break;
    default: raise(EXC_RI); break;
    }
}

void Vr4300::special(uint32_t instr)
{
    const int rs = (instr >> 21) & 31;
    const int rt = (instr >> 16) & 31;
    const int rd = (instr >> 11) & 31;
    const int sa = (instr >> 6) & 31;
    const uint64_t s = gpr[rs], t = gpr[rt];

    switch (instr & 63) {
    case 0x00: gpr[rd] = sext32((uint32_t)t << sa); break;                             // SLL
    case 0x02: gpr[rd] = sext32((uint32_t)t >> sa); break;                             // SRL
    case 0x03: gpr[rd] = sext32((uint32_t)((int64_t)t >> sa)); break;                  // SRA
    case 0x04: gpr[rd] = sext32((uint32_t)t << (s & 31)); break;                       // SLLV
    case 0x06: gpr[rd] = sext32((uint32_t)t >> (s & 31)); break;                       // SRLV
    case 0x07: gpr[rd] = sext32((uint32_t)((int64_t)t >> (s & 31))); break;            // SRAV
    case 0x08: jumpTo(s); break;                                                       // JR
    case 0x09: gpr[rd] = curPc_ + 8; jumpTo(s); break;                                 // JALR
    case 0x0C: raise(EXC_SYS); break;                                                  // SYSCALL
    case 0x0D: raise(EXC_BP); break;                                                   // BREAK
    case 0x0F: break;                                                                  // SYNC
    case 0x10: gpr[rd] = hi; break;                                                    // MFHI
    case 0x11: hi = s; break;                                                          // MTHI
    case 0x12: gpr[rd] = lo; break;                                                    // MFLO
    case 0x13: lo = s; break;                                                          // MTLO
    case 0x14: gpr[rd] = t << (s & 63); break;                                         // DSLLV
    case 0x16: gpr[rd] = t >> (s & 63); break;                                         // DSRLV
    case 0x17: gpr[rd] = (uint64_t)((int64_t)t >> (s & 63)); break;                    // DSRAV
    case 0x18: {                                                                       // MULT
        const int64_t p = (int64_t)(int32_t)s * (int64_t)(int32_t)t;
        lo = sext32((uint32_t)p);
        hi = sext32((uint32_t)((uint64_t)p >> 32));
        break;
    }
    case 0x19: {                                                                       // MULTU
        const uint64_t p = (uint64_t)(uint32_t)s * (uint64_t)(uint32_t)t;
        lo = sext32((uint32_t)p);
        hi = sext32((uint32_t)(p >> 32));
        break;
    }
    case 0x1A: {                                                                       // DIV
        const int32_t n = (int32_t)s, d = (int32_t)t;
        if (d == 0) {
            lo = n < 0 ? 1 : ~0ull;
            hi = sext32((uint32_t)n);
        } else if (n == std::numeric_limits<int32_t>::min() && d == -1) {
            lo = sext32((uint32_t)n);
            hi = 0;
        } else {
            lo = sext32((uint32_t)(n / d));
            hi = sext32((uint32_t)(n % d));
        }
        break;
    }
    case 0x1B: {                                                                       // DIVU
        const uint32_t n = (uint32_t)s, d = (uint32_t)t;
        if (d == 0) { lo = ~0ull; hi = sext32(n); }
        else        { lo = sext32(n / d); hi = sext32(n % d); }
        break;
    }
    case 0x1C: {                                                                       // DMULT
        const __int128 p = (__int128)(int64_t)s * (__int128)(int64_t)t;
        lo = (uint64_t)p;
        hi = (uint64_t)((unsigned __int128)p >> 64);
        break;
    }
    case 0x1D: {                                                                       // DMULTU
        const unsigned __int128 p = (unsigned __int128)s * t;
        lo = (uint64_t)p;
        hi = (uint64_t)(p >> 64);
        break;
    }
    case 0x1E: {                                                                       // DDIV
        const int64_t n = (int64_t)s, d = (int64_t)t;
        if (d == 0) {
            lo = n < 0 ? 1 : ~0ull;
            hi = (uint64_t)n;
        } else if (n == std::numeric_limits<int64_t>::min() && d == -1) {
            lo = (uint64_t)n;
            hi = 0;
        } else {
            lo = (uint64_t)(n / d);
            hi = (uint64_t)(n % d);
        }
        break;
    }
    case 0x1F:                                                                         // DDIVU
        if (t == 0) { lo = ~0ull; hi = s; }
        else        { lo = s / t; hi = s % t; }
        break;
    case 0x20: {                                                                       // ADD
        const int32_t x = (int32_t)s, y = (int32_t)t;
        const int32_t r = (int32_t)((uint32_t)x + (uint32_t)y);
        if (((x ^ r) & (y ^ r)) < 0) { raise(EXC_OV); break; }
        gpr[rd] = sext32((uint32_t)r);
        break;
    }
    case 0x21: gpr[rd] = sext32((uint32_t)s + (uint32_t)t); break;                     // ADDU
    case 0x22: {                                                                       // SUB
        const int32_t x = (int32_t)s, y = (int32_t)t;
        const int32_t r = (int32_t)((uint32_t)x - (uint32_t)y);
        if (((x ^ y) & (x ^ r)) < 0) { raise(EXC_OV); break; }
        gpr[rd] = sext32((uint32_t)r);
        break;
    }
    case 0x23: gpr[rd] = sext32((uint32_t)s - (uint32_t)t); break;                     // SUBU
    case 0x24: gpr[rd] = s & t; break;                                                 // AND
    case 0x25: gpr[rd] = s | t; break;                                                 // OR
    case 0x26: gpr[rd] = s ^ t; break;                                                 // XOR
    case 0x27: gpr[rd] = ~(s | t); break;                                              // NOR
    case 0x2A: gpr[rd] = (int64_t)s < (int64_t)t; break;                               // SLT
    case 0x2B: gpr[rd] = s < t; break;                                                 // SLTU
    case 0x2C: {                                                                       // DADD
        const uint64_t r = s + t;
        if ((int64_t)((s ^ r) & (t ^ r)) < 0) { raise(EXC_OV); break; }
        gpr[rd] = r;
        break;
    }
    case 0x2D: gpr[rd] = s + t; break;                                                 // DADDU
    case 0x2E: {                                                                       // DSUB
        const uint64_t r = s - t;
        if ((int64_t)((s ^ t) & (s ^ r)) < 0) { raise(EXC_OV); break; }
        gpr[rd] = r;
        break;
    }
    case 0x2F: gpr[rd] = s - t; break;                                                 // DSUBU
    case 0x30: if ((int64_t)s >= (int64_t)t) raise(EXC_TR); break;                     // TGE
    case 0x31: if (s >= t) raise(EXC_TR); break;                                       // TGEU
    case 0x32: if ((int64_t)s < (int64_t)t) raise(EXC_TR); break;                      // TLT
    case 0x33: if (s < t) raise(EXC_TR); break;                                        // TLTU
    case 0x34: if (s == t) raise(EXC_TR); break;                                       // TEQ
    case 0x36: if (s != t) raise(EXC_TR); break;                                       // TNE
    case 0x38: gpr[rd] = t << sa; break;                                               // DSLL
    case 0x3A: gpr[rd] = t >> sa; break;                                               // DSRL
    case 0x3B: gpr[rd] = (uint64_t)((int64_t)t >> sa); break;                          // DSRA
    case 0x3C: gpr[rd] = t << (sa + 32); break;                                        // DSLL32
    case 0x3E: gpr[rd] = t >> (sa + 32); break;                                        // DSRL32
    case 0x3F: gpr[rd] = (uint64_t)((int64_t)t >> (sa + 32)); break;                   // DSRA32
    default: raise(EXC_RI); break;
    }
}

void Vr4300::regimm(uint32_t instr)
{
    const int rs = (instr >> 21) & 31;
    const uint64_t s = gpr[rs];
    const uint64_t imm = sext16((uint16_t)instr);
    const uint64_t target = curPc_ + 4 + (imm << 2);
    const bool neg = (int64_t)s < 0;

    switch ((instr >> 16) & 31) {
    case 0x00: branch(neg,  target, false); break;                     // BLTZ
    case 0x01: branch(!neg, target, false); break;                     // BGEZ
    case 0x02: branch(neg,  target, true);  break;                     // BLTZL
    case 0x03: branch(!neg, target, true);  break;                     // BGEZL
    case 0x08: if ((int64_t)s >= (int64_t)imm) raise(EXC_TR); break;   // TGEI
    case 0x09: if (s >= imm) raise(EXC_TR); break;                     // TGEIU
    case 0x0A: if ((int64_t)s < (int64_t)imm) raise(EXC_TR); break;    // TLTI
    case 0x0B: if (s < imm) raise(EXC_TR); break;                      // TLTIU
    case 0x0C: if (s == imm) raise(EXC_TR); break;                     // TEQI
    case 0x0E: if (s != imm) raise(EXC_TR); break;                     // TNEI
    case 0x10: gpr[31] = curPc_ + 8; branch(neg,  target, false); break;   // BLTZAL
    case 0x11: gpr[31] = curPc_ + 8; branch(!neg, target, false); break;   // BGEZAL
    case 0x12: gpr[31] = curPc_ + 8; branch(neg,  target, true);  break;   // BLTZALL
    case 0x13: gpr[31] = curPc_ + 8; branch(!neg, target, true);  break;   // BGEZALL
    default: raise(EXC_RI); break;
    }
}

// ─── Загрузки и сохранения ────────────────────────────────────────────────────
void Vr4300::loadStore(uint32_t instr)
{
    const uint32_t op = instr >> 26;
    const int rt = (instr >> 16) & 31;
    const uint64_t vaddr = gpr[(instr >> 21) & 31] + sext16((uint16_t)instr);
    uint64_t& r = gpr[rt];

    switch (op) {
    case 0x20: { uint8_t  v; if (load8 (vaddr, v)) r = sext8(v);  break; }      // LB
    case 0x24: { uint8_t  v; if (load8 (vaddr, v)) r = v;         break; }      // LBU
    case 0x21: { uint16_t v; if (load16(vaddr, v)) r = sext16(v); break; }      // LH
    case 0x25: { uint16_t v; if (load16(vaddr, v)) r = v;         break; }      // LHU
    case 0x23: { uint32_t v; if (load32(vaddr, v)) r = sext32(v); break; }      // LW
    case 0x27: { uint32_t v; if (load32(vaddr, v)) r = v;         break; }      // LWU
    case 0x37: { uint64_t v; if (load64(vaddr, v)) r = v;         break; }      // LD
    case 0x22: {                                                                // LWL
        uint32_t w;
        if (!load32(vaddr & ~3ull, w)) break;
        const uint32_t sh = 8 * (vaddr & 3);
        const uint32_t mask = 0xFFFFFFFFu << sh;
        r = sext32(((uint32_t)r & ~mask) | (w << sh));
        break;
    }
    case 0x26: {                                                                // LWR
        uint32_t w;
        if (!load32(vaddr & ~3ull, w)) break;
        const uint32_t sh = 8 * ((vaddr ^ 3) & 3);
        const uint32_t mask = 0xFFFFFFFFu >> sh;
        r = sext32(((uint32_t)r & ~mask) | (w >> sh));
        break;
    }
    case 0x1A: {                                                                // LDL
        uint64_t d;
        if (!load64(vaddr & ~7ull, d)) break;
        const uint32_t sh = 8 * (vaddr & 7);
        const uint64_t mask = ~0ull << sh;
        r = (r & ~mask) | (d << sh);
        break;
    }
    case 0x1B: {                                                                // LDR
        uint64_t d;
        if (!load64(vaddr & ~7ull, d)) break;
        const uint32_t sh = 8 * ((vaddr ^ 7) & 7);
        const uint64_t mask = ~0ull >> sh;
        r = (r & ~mask) | (d >> sh);
        break;
    }
    case 0x28: store8 (vaddr, (uint8_t)r);  break;                              // SB
    case 0x29: store16(vaddr, (uint16_t)r); break;                              // SH
    case 0x2B: store32(vaddr, (uint32_t)r); break;                              // SW
    case 0x3F: store64(vaddr, r);           break;                              // SD
    case 0x2A: {                                                                // SWL
        const uint32_t sh = 8 * (vaddr & 3);
        store32(vaddr & ~3ull, (uint32_t)r >> sh, 0xFFFFFFFFu >> sh);
        break;
    }
    case 0x2E: {                                                                // SWR
        const uint32_t sh = 8 * ((vaddr ^ 3) & 3);
        store32(vaddr & ~3ull, (uint32_t)r << sh, 0xFFFFFFFFu << sh);
        break;
    }
    case 0x2C: {                                                                // SDL
        const uint32_t sh = 8 * (vaddr & 7);
        store64(vaddr & ~7ull, r >> sh, ~0ull >> sh);
        break;
    }
    case 0x2D: {                                                                // SDR
        const uint32_t sh = 8 * ((vaddr ^ 7) & 7);
        store64(vaddr & ~7ull, r << sh, ~0ull << sh);
        break;
    }
    case 0x30: case 0x34: {                                                     // LL / LLD
        if (op == 0x30) { uint32_t v; if (!load32(vaddr, v)) break; r = sext32(v); }
        else            { uint64_t v; if (!load64(vaddr, v)) break; r = v; }
        uint32_t pa = 0;
        translate(vaddr, Access::Read, pa);       // уже удалось выше
        cop0[C0_LLADDR] = pa >> 4;
        llbit = true;
        break;
    }
    case 0x38: case 0x3C: {                                                     // SC / SCD
        if (!llbit) { r = 0; break; }
        const bool ok = op == 0x38 ? store32(vaddr, (uint32_t)r) : store64(vaddr, r);
        if (ok) r = 1;
        break;
    }
    case 0x31: { if (!cop1Usable()) break; uint32_t v; if (load32(vaddr, v)) setFpr32(rt, v); break; }   // LWC1
    case 0x35: { if (!cop1Usable()) break; uint64_t v; if (load64(vaddr, v)) setFpr64(rt, v); break; }   // LDC1
    case 0x39: if (cop1Usable()) store32(vaddr, getFpr32(rt)); break;                                    // SWC1
    case 0x3D: if (cop1Usable()) store64(vaddr, getFpr64(rt)); break;                                    // SDC1
    default: raise(EXC_RI); break;
    }
}

// ─── COP0 ─────────────────────────────────────────────────────────────────────
void Vr4300::cop0Op(uint32_t instr)
{
    if (!kernelMode() && !(cop0[C0_STATUS] & ST_CU0)) { raise(EXC_CPU, 0); return; }
    const int rt = (instr >> 16) & 31;
    const int rd = (instr >> 11) & 31;

    switch ((instr >> 21) & 31) {
    case 0x00: gpr[rt] = sext32((uint32_t)readCop0(rd)); break;     // MFC0
    case 0x01: gpr[rt] = readCop0(rd); break;                       // DMFC0
    case 0x04: writeCop0(rd, sext32((uint32_t)gpr[rt])); break;     // MTC0
    case 0x05: writeCop0(rd, gpr[rt]); break;                       // DMTC0
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        switch (instr & 63) {
        case 0x01: tlbRead(); break;                                            // TLBR
        case 0x02: tlbWrite((int)(cop0[C0_INDEX] & 31)); break;                 // TLBWI
        case 0x06: tlbWrite(randomIndex()); break;                              // TLBWR
        case 0x08: tlbProbe(); break;                                           // TLBP
        case 0x18: {                                                            // ERET
            uint64_t target;
            if (cop0[C0_STATUS] & ST_ERL) { target = cop0[C0_ERROREPC]; cop0[C0_STATUS] &= ~ST_ERL; }
            else                          { target = cop0[C0_EPC];      cop0[C0_STATUS] &= ~ST_EXL; }
            jump(target);
            llbit = false;
            break;
        }
        default: raise(EXC_RI); break;
        }
        break;
    default: raise(EXC_RI); break;
    }
}

uint64_t Vr4300::readCop0(int reg) const
{
    switch (reg) {
    case C0_RANDOM: return (uint64_t)randomIndex();
    case C0_COUNT:  return (uint32_t)cop0[C0_COUNT];
    case 7: case 21: case 22: case 23: case 24: case 25: case 31:
        return cop0Latch_;                     // пустые регистры отдают защёлку шины
    default: return cop0[reg];
    }
}

void Vr4300::writeCop0(int reg, uint64_t v)
{
    cop0Latch_ = v;
    uint64_t& c = cop0[reg];
    switch (reg) {
    case C0_INDEX:    c = v & 0x8000003F; break;
    case C0_ENTRYLO0:
    case C0_ENTRYLO1: c = v & 0x3FFFFFFF; break;
    case C0_CONTEXT:  c = (c & 0x7FFFFF) | (v & ~0x7FFFFFull); break;
    case C0_PAGEMASK: c = v & 0x01FFE000; break;
    case C0_WIRED:    c = v & 0x3F; randomBase_ = cycles_ / CYCLES_PER_INSTR; break;
    case C0_COUNT:    c = (uint32_t)v; break;
    case C0_ENTRYHI:  c = v & (VPN2_MASK | 0xFF); break;
    case C0_COMPARE:  c = (uint32_t)v; cop0[C0_CAUSE] &= ~0x8000ull; break;
    case C0_STATUS:   c = v & 0xFF57FFFF; break;
    case C0_CAUSE:    c = (c & ~0x300ull) | (v & 0x300); break;
    case C0_EPC:
    case C0_ERROREPC: c = v; break;
    case C0_CONFIG:   c = (c & ~0x0F00800Full) | (v & 0x0F00800F); break;
    case C0_LLADDR:   c = (uint32_t)v; break;
    case C0_WATCHLO:  c = v & 0xFFFFFFFB; break;
    case C0_WATCHHI:  c = v & 0xF; break;
    case C0_XCONTEXT: c = (c & 0x1FFFFFFFFull) | (v & ~0x1FFFFFFFFull); break;
    case C0_PARITYERR: c = v & 0xFF; break;
    case C0_TAGLO:    c = v & 0x0FFFFFC0; break;
    default: break;                            // только чтение или не существуют
    }
}

int Vr4300::randomIndex() const
{
    const uint32_t wired = (uint32_t)cop0[C0_WIRED] & 31;
    const uint64_t instrs = cycles_ / CYCLES_PER_INSTR - randomBase_;
    return 31 - (int)(instrs % (32 - wired));
}

void Vr4300::tlbRead()
{
    const TlbEntry& e = tlb_[cop0[C0_INDEX] & 31];
    cop0[C0_PAGEMASK] = e.pageMask;
    cop0[C0_ENTRYHI]  = e.entryHi;
    cop0[C0_ENTRYLO0] = e.entryLo0 | (e.global ? 1u : 0u);
    cop0[C0_ENTRYLO1] = e.entryLo1 | (e.global ? 1u : 0u);
}

void Vr4300::tlbWrite(int index)
{
    TlbEntry& e = tlb_[index & 31];
    e.pageMask = (uint32_t)cop0[C0_PAGEMASK] & 0x01FFE000;
    e.entryHi  = cop0[C0_ENTRYHI] & (VPN2_MASK | 0xFF) & ~(uint64_t)e.pageMask;
    e.entryLo0 = (uint32_t)cop0[C0_ENTRYLO0] & 0x3FFFFFFE;
    e.entryLo1 = (uint32_t)cop0[C0_ENTRYLO1] & 0x3FFFFFFE;
    e.global   = cop0[C0_ENTRYLO0] & cop0[C0_ENTRYLO1] & 1;
}

void Vr4300::tlbProbe()
{
    const uint64_t hi = cop0[C0_ENTRYHI];
    for (int i = 0; i < 32; ++i) {
        const TlbEntry& e = tlb_[i];
        const uint64_t mask = ~((uint64_t)e.pageMask | 0x1FFF) & VPN2_MASK;
        if ((e.entryHi & mask) != (hi & mask)) continue;
        if (!e.global && (e.entryHi & 0xFF) != (hi & 0xFF)) continue;
        cop0[C0_INDEX] = (uint64_t)i;
        return;
    }
    cop0[C0_INDEX] |= 0x80000000u;
}

// ─── COP2: у N64 его нет, но регистр-защёлка на шине есть ──────────────────────
void Vr4300::cop2Op(uint32_t instr)
{
    if (!(cop0[C0_STATUS] & ST_CU2)) { raise(EXC_CPU, 2); return; }
    const int rt = (instr >> 16) & 31;
    switch ((instr >> 21) & 31) {
    case 0x00: gpr[rt] = sext32((uint32_t)cop2Latch_); break;        // MFC2
    case 0x01: gpr[rt] = cop2Latch_; break;                          // DMFC2
    case 0x02: gpr[rt] = sext32((uint32_t)cop2Latch_); break;        // CFC2
    case 0x04: case 0x05: case 0x06: cop2Latch_ = gpr[rt]; break;    // MTC2/DMTC2/CTC2
    default: raise(EXC_RI); break;
    }
}

// ─── COP1 (FPU) ───────────────────────────────────────────────────────────────
bool Vr4300::cop1Usable()
{
    if (cop0[C0_STATUS] & ST_CU1) return true;
    raise(EXC_CPU, 1);
    return false;
}

// FR=1: 32 независимых 64-битных регистра. FR=0: 16 пар, нечётный 32-битный
// регистр — старшая половина чётного.
uint32_t Vr4300::getFpr32(int n) const
{
    if (fr() || !(n & 1)) return (uint32_t)fpr[n];
    return (uint32_t)(fpr[n & ~1] >> 32);
}

void Vr4300::setFpr32(int n, uint32_t v)
{
    if (fr() || !(n & 1)) fpr[n] = (fpr[n] & ~0xFFFFFFFFull) | v;
    else                  fpr[n & ~1] = (fpr[n & ~1] & 0xFFFFFFFFull) | ((uint64_t)v << 32);
}

uint64_t Vr4300::getFpr64(int n) const { return fpr[fr() ? n : (n & ~1)]; }
void     Vr4300::setFpr64(int n, uint64_t v) { fpr[fr() ? n : (n & ~1)] = v; }
float    Vr4300::getF(int n) const { return fromBits<float>(getFpr32(n)); }
double   Vr4300::getD(int n) const { return fromBits<double>(getFpr64(n)); }
void     Vr4300::setF(int n, float v)  { setFpr32(n, bitsOf(v)); }
void     Vr4300::setD(int n, double v) { setFpr64(n, bitsOf(v)); }

void Vr4300::applyRounding() const
{
    static const int modes[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
    std::fesetround(modes[fcr31 & 3]);
}

void Vr4300::writeFcr31(uint32_t v)
{
    fcr31 = v & 0x0183FFFF;
    applyRounding();
    const uint32_t cause = (fcr31 >> 12) & 0x3F;
    const uint32_t enables = ((fcr31 >> 7) & 0x1F) | FC_E;
    if (cause & enables) raise(EXC_FPE);
}

// Записать причины в FCR31. Если причина разрешена (или это E) — исключение,
// результат не записывается, флаги не копятся.
bool Vr4300::fpuTrap(uint32_t cause)
{
    fcr31 = (fcr31 & ~0x3F000u) | (cause << 12);
    const uint32_t enables = ((fcr31 >> 7) & 0x1F) | FC_E;
    if (cause & enables) { raise(EXC_FPE); return true; }
    fcr31 |= (cause & 0x1F) << 2;
    return false;
}

// Денормал на входе — «не реализовано». NaN: сигнальный (в кодировке MIPS
// старший бит мантиссы = 1) — тоже «не реализовано», тихий — «недопустимая
// операция» с каноническим NaN в результате.
template<class F> Vr4300::FpIn Vr4300::fpuCheckIn(F v)
{
    switch (std::fpclassify(v)) {
    case FP_SUBNORMAL: fpuTrap(FC_E); return FpIn::Stop;
    case FP_NAN:
        if (bitsOf(v) & FBits<F>::quietBit) { fpuTrap(FC_E); return FpIn::Stop; }
        return fpuTrap(FC_V) ? FpIn::Stop : FpIn::Nan;
    default: return FpIn::Ok;
    }
}

// Флаги хоста → причины. NaN-результат — канонический NaN MIPS; денормал на
// выходе при FS=1 сбрасывается в ноль, иначе — «не реализовано».
template<class F> bool Vr4300::fpuFinish(F& r)
{
    const int ex = std::fetestexcept(FE_ALL_EXCEPT);
    uint32_t cause = 0;
    if (ex & FE_INEXACT)   cause |= FC_I;
    if (ex & FE_OVERFLOW)  cause |= FC_O;
    if (ex & FE_DIVBYZERO) cause |= FC_Z;
    if (ex & FE_INVALID)   cause |= FC_V;
    if (std::isnan(r)) r = fromBits<F>(FBits<F>::defaultNan);
    if (std::fpclassify(r) == FP_SUBNORMAL) {
        const bool fs = fcr31 & (1u << 24);
        const bool trapUI = fcr31 & ((1u << 7) | (1u << 8));
        if (!fs || trapUI) return !fpuTrap(FC_E);
        cause |= FC_U | FC_I;
        const bool neg = std::signbit(r);
        const uint32_t rm = fcr31 & 3;
        const bool toMin = (rm == 2 && !neg) || (rm == 3 && neg);
        r = toMin ? std::numeric_limits<F>::min() : F(0);
        if (neg) r = -r;
    } else if (ex & FE_UNDERFLOW) {
        cause |= FC_U;
    }
    return !fpuTrap(cause);
}

template<class F> void Vr4300::fpuArith(uint32_t funct, int fd, int fs, int ft)
{
    auto get = [&](int n) -> F { if constexpr (std::is_same_v<F, float>) return getF(n); else return getD(n); };
    auto put = [&](int n, F v) { if constexpr (std::is_same_v<F, float>) setF(n, v); else setD(n, v); };
    const F nan = fromBits<F>(FBits<F>::defaultNan);

    if (funct == 0x06) { put(fd, get(fs)); return; }             // MOV — просто биты
    std::feclearexcept(FE_ALL_EXCEPT);
    const F a = get(fs);
    FpIn in = fpuCheckIn(a);
    if (in == FpIn::Ok && funct <= 0x03) in = fpuCheckIn(get(ft));
    if (in == FpIn::Stop) return;
    if (in == FpIn::Nan) { put(fd, nan); return; }

    volatile F va = a;                                           // не даём компилятору свернуть
    F r;
    switch (funct) {
    case 0x00: { volatile F vb = get(ft); r = va + vb; break; }  // ADD
    case 0x01: { volatile F vb = get(ft); r = va - vb; break; }  // SUB
    case 0x02: { volatile F vb = get(ft); r = va * vb; break; }  // MUL
    case 0x03: { volatile F vb = get(ft); r = va / vb; break; }  // DIV
    case 0x04: r = std::sqrt((F)va); break;                      // SQRT
    case 0x05: r = std::fabs(a); break;                          // ABS
    case 0x07: r = -a; break;                                    // NEG
    default: fpuTrap(FC_E); return;
    }
    if (!fpuFinish(r)) return;
    put(fd, r);
}

// C.cond: биты условия — 0 «неупорядочено», 1 «равно», 2 «меньше», 3 — сигнальное.
template<class F> void Vr4300::fpuCompare(uint32_t cond, int fs, int ft)
{
    F a, b;
    if constexpr (std::is_same_v<F, float>) { a = getF(fs); b = getF(ft); }
    else                                    { a = getD(fs); b = getD(ft); }
    bool c;
    if (std::isnan(a) || std::isnan(b)) {
        const bool signaling = (cond & 8) || (std::isnan(a) && (bitsOf(a) & FBits<F>::quietBit))
                                          || (std::isnan(b) && (bitsOf(b) & FBits<F>::quietBit));
        if (signaling && fpuTrap(FC_V)) return;
        if (!signaling) fpuTrap(0);
        c = cond & 1;
    } else {
        fpuTrap(0);
        c = ((cond & 2) && a == b) || ((cond & 4) && a < b);
    }
    if (c) fcr31 |= 1u << 23;
    else   fcr31 &= ~(1u << 23);
}

// ROUND/TRUNC/CEIL/FLOOR/CVT в целое. NaN, бесконечность и выход за диапазон
// у VR4300 — «не реализовано».
template<class F> void Vr4300::fpuToInt(uint32_t funct, int fd, int fs)
{
    F v;
    if constexpr (std::is_same_v<F, float>) v = getF(fs); else v = getD(fs);
    const bool toLong = funct == 0x25 || funct < 0x0C;
    if (std::isnan(v) || std::isinf(v)) { fpuTrap(FC_E); return; }
    if (fpuCheckIn(v) != FpIn::Ok) return;
    F r;
    if (funct >= 0x24) {                            // CVT.W / CVT.L — по режиму FCR31
        volatile F vv = v;
        r = std::nearbyint((F)vv);
    } else {
        switch (funct & 3) {
        case 0:  r = roundEven(v); break;           // ROUND
        case 1:  r = std::trunc(v); break;          // TRUNC
        case 2:  r = std::ceil(v);  break;          // CEIL
        default: r = std::floor(v); break;          // FLOOR
        }
    }
    if ((toLong  && (r >= F(9007199254740992.0) || r <= F(-9007199254740992.0))) ||
        (!toLong && (r >= F(2147483648.0) || r < F(-2147483648.0)))) {
        fpuTrap(FC_E);
        return;
    }
    if (fpuTrap(r != v ? FC_I : 0)) return;
    if (toLong) setFpr64(fd, (uint64_t)(int64_t)r);
    else        setFpr32(fd, (uint32_t)(int32_t)r);
}

void Vr4300::cop1Op(uint32_t instr)
{
    if (!cop1Usable()) return;
    const uint32_t fmt = (instr >> 21) & 31;
    const int ft = (instr >> 16) & 31;
    const int fs = (instr >> 11) & 31;
    const int fd = (instr >> 6) & 31;
    const uint32_t funct = instr & 63;

    switch (fmt) {
    case 0x00: gpr[ft] = sext32(getFpr32(fs)); return;                     // MFC1
    case 0x01: gpr[ft] = getFpr64(fs); return;                             // DMFC1
    case 0x02:                                                             // CFC1
        gpr[ft] = fs == 31 ? sext32(fcr31) : fs == 0 ? 0x0A00 : 0;
        return;
    case 0x04: setFpr32(fs, (uint32_t)gpr[ft]); return;                    // MTC1
    case 0x05: setFpr64(fs, gpr[ft]); return;                              // DMTC1
    case 0x06: if (fs == 31) writeFcr31((uint32_t)gpr[ft]); return;        // CTC1
    case 0x08: {                                                           // BC1
        const bool c = fcr31 & (1u << 23);
        const uint64_t target = curPc_ + 4 + (sext16((uint16_t)instr) << 2);
        switch (ft & 3) {
        case 0: branch(!c, target, false); break;
        case 1: branch(c,  target, false); break;
        case 2: branch(!c, target, true);  break;
        default: branch(c, target, true);  break;
        }
        return;
    }
    case 0x10: case 0x11: {                                                // S / D
        const bool dbl = fmt == 0x11;
        if (funct < 0x08) {
            if (dbl) fpuArith<double>(funct, fd, fs, ft); else fpuArith<float>(funct, fd, fs, ft);
        } else if (funct < 0x10 || funct == 0x24 || funct == 0x25) {
            if (dbl) fpuToInt<double>(funct, fd, fs); else fpuToInt<float>(funct, fd, fs);
        } else if (funct == 0x20 && dbl) {                                 // CVT.S.D
            std::feclearexcept(FE_ALL_EXCEPT);
            const double v = getD(fs);
            const FpIn in = fpuCheckIn(v);
            if (in == FpIn::Stop) return;
            if (in == FpIn::Nan) { setF(fd, fromBits<float>(FBits<float>::defaultNan)); return; }
            volatile double vv = v;
            float r = (float)vv;
            if (!fpuFinish(r)) return;
            setF(fd, r);
        } else if (funct == 0x21 && !dbl) {                                // CVT.D.S
            const float v = getF(fs);
            const FpIn in = fpuCheckIn(v);
            if (in == FpIn::Stop) return;
            if (in == FpIn::Nan) { setD(fd, fromBits<double>(FBits<double>::defaultNan)); return; }
            fpuTrap(0);
            setD(fd, (double)v);
        } else if (funct >= 0x30) {
            if (dbl) fpuCompare<double>(funct & 15, fs, ft); else fpuCompare<float>(funct & 15, fs, ft);
        } else {
            fpuTrap(FC_E);
        }
        return;
    }
    case 0x14: case 0x15: {                                                // W / L → S / D
        if (funct != 0x20 && funct != 0x21) { fpuTrap(FC_E); return; }
        const int64_t v = fmt == 0x14 ? (int64_t)(int32_t)getFpr32(fs) : (int64_t)getFpr64(fs);
        if (fmt == 0x15 && (v >= (1ll << 55) || v < -(1ll << 55))) { fpuTrap(FC_E); return; }
        std::feclearexcept(FE_ALL_EXCEPT);
        volatile int64_t vv = v;
        if (funct == 0x20) { float r = (float)vv;  if (!fpuFinish(r)) return; setF(fd, r); }
        else               { double r = (double)vv; if (!fpuFinish(r)) return; setD(fd, r); }
        return;
    }
    default: raise(EXC_RI); return;
    }
}

// ─── Save state ───────────────────────────────────────────────────────────────
template<class S> void Vr4300::serialize(S& s)
{
    for (auto& r : gpr) s.io(r);
    s.io(hi); s.io(lo); s.io(pc); s.io(nextPc);
    for (auto& r : fpr) s.io(r);
    s.io(fcr31);
    for (auto& r : cop0) s.io(r);
    s.io(llbit);
    s.io(curPc_); s.io(inDelaySlot_); s.io(nextInDelay_);
    s.io(cycles_); s.io(randomBase_); s.io(cop0Latch_); s.io(cop2Latch_);
    for (auto& e : tlb_) {
        s.io(e.entryHi); s.io(e.entryLo0); s.io(e.entryLo1); s.io(e.pageMask); s.io(e.global);
    }
}

template void Vr4300::serialize<StateWriter>(StateWriter&);
template void Vr4300::serialize<StateReader>(StateReader&);
