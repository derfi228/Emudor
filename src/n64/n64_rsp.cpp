// n64_rsp.cpp — RSP: скалярное ядро и векторный блок.
#include "n64_rsp.h"
#include "n64_system.h"
#include "console/state_io.h"
#include <algorithm>
#include <cmath>

namespace {

// Таблицы ПЗУ блока деления: мантиссы 1/x и 1/sqrt(x) по 9 старшим битам.
struct DivTables {
    uint16_t rcp[512];
    uint16_t rsq[512];
    DivTables()
    {
        for (uint32_t i = 0; i < 512; ++i) {
            const uint64_t v = ((1ull << 34) / (i + 512) + 1) >> 8;
            rcp[i] = (uint16_t)std::min<uint64_t>(v - 0x10000, 0xFFFF);
        }
        // Нечётный индекс — нечётная степень двойки: аргумент вдвое меньше.
        for (uint32_t i = 0; i < 512; ++i) {
            const uint64_t a = (i + 512) >> (i & 1);
            uint64_t c = (uint64_t)std::sqrt((double)(1ull << 44) / (double)a);   // наибольшее c: a·c² < 2^44
            while (a * c * c >= (1ull << 44)) --c;
            while (a * (c + 1) * (c + 1) < (1ull << 44)) ++c;
            const uint64_t b = std::max<uint64_t>(c - 1, 1u << 17);
            rsq[i] = (uint16_t)(b >> 1);
        }
    }
};
const DivTables& tables() { static const DivTables t; return t; }

inline int16_t  clamp16(int32_t v) { return (int16_t)std::clamp(v, -32768, 32767); }
inline uint32_t sext16(uint16_t v) { return (uint32_t)(int32_t)(int16_t)v; }
inline uint32_t sext8(uint8_t v)   { return (uint32_t)(int32_t)(int8_t)v; }

// Полоса vt, которую видит полоса n при элементе e: весь вектор, пары,
// четвёрки или один элемент на все полосы.
inline int laneOf(int e, int n)
{
    if (e < 2) return n;
    if (e < 4) return (n & ~1) | (e & 1);
    if (e < 8) return (n & ~3) | (e & 3);
    return e & 7;
}

inline bool bit(uint8_t mask, int n) { return (mask >> n) & 1; }

} // namespace

void Rsp::reset()
{
    for (auto& r : r_) r = 0;
    for (auto& v : vr_) for (auto& x : v) x = 0;
    for (auto& a : acc_) a = 0;
    vcoLo_ = vcoHi_ = vccLo_ = vccHi_ = vce_ = 0;
    divIn_ = divOut_ = 0;
    divDp_ = false;
    pc = 0;
    nextPc_ = 4;
}

void Rsp::setPc(uint32_t address)
{
    pc = address & 0xFFC;
    nextPc_ = (pc + 4) & 0xFFC;
}

void Rsp::run(uint32_t cycles)
{
    for (uint32_t i = 0; i < cycles && !sys_->spHalted(); ++i) step();
}

void Rsp::step()
{
    const uint32_t instr = N64System::get32(imem_ + (pc & 0xFFC));
    curPc_ = pc;
    pc = nextPc_;
    nextPc_ = (pc + 4) & 0xFFC;
    execute(instr);
    r_[0] = 0;
}

void Rsp::branch(bool take, uint32_t target)
{
    if (take) nextPc_ = target & 0xFFC;
}

uint32_t Rsp::rd32(uint32_t a) const
{
    return ((uint32_t)rd8(a) << 24) | ((uint32_t)rd8(a + 1) << 16) | ((uint32_t)rd8(a + 2) << 8) | rd8(a + 3);
}

void Rsp::wr32(uint32_t a, uint32_t v)
{
    wr8(a, (uint8_t)(v >> 24)); wr8(a + 1, (uint8_t)(v >> 16));
    wr8(a + 2, (uint8_t)(v >> 8)); wr8(a + 3, (uint8_t)v);
}

uint8_t Rsp::vbyte(int v, int k) const
{
    const uint16_t e = (uint16_t)vr_[v][(k >> 1) & 7];
    return (k & 1) ? (uint8_t)e : (uint8_t)(e >> 8);
}

void Rsp::setVbyte(int v, int k, uint8_t x)
{
    uint16_t e = (uint16_t)vr_[v][(k >> 1) & 7];
    e = (k & 1) ? (uint16_t)((e & 0xFF00) | x) : (uint16_t)((e & 0x00FF) | (x << 8));
    vr_[v][(k >> 1) & 7] = (int16_t)e;
}

// ─── Скалярные команды ────────────────────────────────────────────────────────
void Rsp::execute(uint32_t instr)
{
    const uint32_t op = instr >> 26;
    const int rs = (instr >> 21) & 31;
    const int rt = (instr >> 16) & 31;
    const int rd = (instr >> 11) & 31;
    const uint32_t imm = sext16((uint16_t)instr);
    const uint32_t s = r_[rs], t = r_[rt];
    const uint32_t btarget = curPc_ + 4 + (imm << 2);

    switch (op) {
    case 0x00: {                                                         // SPECIAL
        const int sa = (instr >> 6) & 31;
        switch (instr & 63) {
        case 0x00: r_[rd] = t << sa; break;                              // SLL
        case 0x02: r_[rd] = t >> sa; break;                              // SRL
        case 0x03: r_[rd] = (uint32_t)((int32_t)t >> sa); break;         // SRA
        case 0x04: r_[rd] = t << (s & 31); break;                        // SLLV
        case 0x06: r_[rd] = t >> (s & 31); break;                        // SRLV
        case 0x07: r_[rd] = (uint32_t)((int32_t)t >> (s & 31)); break;   // SRAV
        case 0x08: branch(true, s); break;                               // JR
        case 0x09: r_[rd] = (curPc_ + 8) & 0xFFC; branch(true, s); break;// JALR
        case 0x0D: sys_->rspBreak(); break;                              // BREAK
        case 0x20: case 0x21: r_[rd] = s + t; break;                     // ADD(U)
        case 0x22: case 0x23: r_[rd] = s - t; break;                     // SUB(U)
        case 0x24: r_[rd] = s & t; break;
        case 0x25: r_[rd] = s | t; break;
        case 0x26: r_[rd] = s ^ t; break;
        case 0x27: r_[rd] = ~(s | t); break;
        case 0x2A: r_[rd] = (int32_t)s < (int32_t)t; break;              // SLT
        case 0x2B: r_[rd] = s < t; break;                                // SLTU
        default: break;
        }
        break;
    }
    case 0x01:                                                           // REGIMM
        switch (rt) {
        case 0x00: branch((int32_t)s < 0, btarget); break;               // BLTZ
        case 0x01: branch((int32_t)s >= 0, btarget); break;              // BGEZ
        case 0x10: r_[31] = (curPc_ + 8) & 0xFFC; branch((int32_t)s < 0, btarget); break;
        case 0x11: r_[31] = (curPc_ + 8) & 0xFFC; branch((int32_t)s >= 0, btarget); break;
        default: break;
        }
        break;
    case 0x02: branch(true, instr << 2); break;                          // J
    case 0x03: r_[31] = (curPc_ + 8) & 0xFFC; branch(true, instr << 2); break;   // JAL
    case 0x04: branch(s == t, btarget); break;                           // BEQ
    case 0x05: branch(s != t, btarget); break;                           // BNE
    case 0x06: branch((int32_t)s <= 0, btarget); break;                  // BLEZ
    case 0x07: branch((int32_t)s > 0, btarget); break;                   // BGTZ
    case 0x08: case 0x09: r_[rt] = s + imm; break;                       // ADDI(U)
    case 0x0A: r_[rt] = (int32_t)s < (int32_t)imm; break;                // SLTI
    case 0x0B: r_[rt] = s < imm; break;                                  // SLTIU
    case 0x0C: r_[rt] = s & (instr & 0xFFFF); break;                     // ANDI
    case 0x0D: r_[rt] = s | (instr & 0xFFFF); break;                     // ORI
    case 0x0E: r_[rt] = s ^ (instr & 0xFFFF); break;                     // XORI
    case 0x0F: r_[rt] = instr << 16; break;                              // LUI
    case 0x10:                                                           // COP0: регистры SP/DP
        if (rs == 0x00)      r_[rt] = sys_->spRegRead(rd & 15);
        else if (rs == 0x04) sys_->spRegWrite(rd & 15, t);
        break;
    case 0x12: cop2(instr); break;
    case 0x20: r_[rt] = sext8(rd8(s + imm)); break;                      // LB
    case 0x21: r_[rt] = sext16((uint16_t)(rd8(s + imm) << 8 | rd8(s + imm + 1))); break;   // LH
    case 0x23: case 0x27: r_[rt] = rd32(s + imm); break;                 // LW / LWU
    case 0x24: r_[rt] = rd8(s + imm); break;                             // LBU
    case 0x25: r_[rt] = (uint32_t)(rd8(s + imm) << 8 | rd8(s + imm + 1)); break;           // LHU
    case 0x28: wr8(s + imm, (uint8_t)t); break;                          // SB
    case 0x29: wr8(s + imm, (uint8_t)(t >> 8)); wr8(s + imm + 1, (uint8_t)t); break;       // SH
    case 0x2B: wr32(s + imm, t); break;                                  // SW
    case 0x32: vectorLoad(instr); break;                                 // LWC2
    case 0x3A: vectorStore(instr); break;                                // SWC2
    default: break;
    }
}

// ─── COP2: пересылки и векторные команды ─────────────────────────────────────
void Rsp::cop2(uint32_t instr)
{
    const uint32_t fmt = (instr >> 21) & 31;
    if (fmt & 0x10) { vectorOp(instr); return; }
    const int rt = (instr >> 16) & 31;
    const int rd = (instr >> 11) & 31;
    const int e = (instr >> 7) & 15;
    switch (fmt) {
    case 0x00:                                                           // MFC2
        r_[rt] = sext16((uint16_t)(vbyte(rd, e) << 8 | vbyte(rd, (e + 1) & 15)));
        break;
    case 0x02:                                                           // CFC2
        switch (rd & 3) {
        case 0:  r_[rt] = sext16((uint16_t)(vcoHi_ << 8 | vcoLo_)); break;
        case 1:  r_[rt] = sext16((uint16_t)(vccHi_ << 8 | vccLo_)); break;
        default: r_[rt] = vce_; break;
        }
        break;
    case 0x04:                                                           // MTC2
        setVbyte(rd, e, (uint8_t)(r_[rt] >> 8));
        if (e < 15) setVbyte(rd, e + 1, (uint8_t)r_[rt]);
        break;
    case 0x06:                                                           // CTC2
        switch (rd & 3) {
        case 0:  vcoLo_ = (uint8_t)r_[rt]; vcoHi_ = (uint8_t)(r_[rt] >> 8); break;
        case 1:  vccLo_ = (uint8_t)r_[rt]; vccHi_ = (uint8_t)(r_[rt] >> 8); break;
        default: vce_ = (uint8_t)r_[rt]; break;
        }
        break;
    default: break;
    }
}

// Насыщение аккумулятора: если 48-битное значение помещается в выбранный
// 16-битный срез (со знаком) — отдаём срез, иначе neg/pos.
uint16_t Rsp::saturate(int n, bool mid, uint16_t neg, uint16_t pos) const
{
    const int16_t h = acch(n), m = accm(n);
    if (h < 0) { if (h != -1 || m >= 0) return neg; }
    else       { if (h != 0 || m < 0)   return pos; }
    return mid ? (uint16_t)m : (uint16_t)accl(n);
}

void Rsp::vectorOp(uint32_t instr)
{
    const int e  = (instr >> 21) & 15;
    const int vt = (instr >> 16) & 31;
    const int vs = (instr >> 11) & 31;
    const int vd = (instr >> 6) & 31;
    const uint32_t funct = instr & 63;

    if (funct == 0x37 || funct == 0x3F) return;                          // VNOP, VNULL
    if (funct >= 0x30 && funct <= 0x36) { divide(funct, vd, vs, vt, e); return; }

    int16_t s[8], t[8], res[8];
    for (int n = 0; n < 8; ++n) { s[n] = vr_[vs][n]; t[n] = vr_[vt][laneOf(e, n)]; }

    switch (funct) {
    // ── Умножения ────────────────────────────────────────────────────────────
    case 0x00: case 0x01:                                                // VMULF / VMULU
        for (int n = 0; n < 8; ++n) {
            accSet(n, (int64_t)s[n] * t[n] * 2 + 0x8000);
            res[n] = (int16_t)(funct == 0 ? saturate(n, true, 0x8000, 0x7FFF)
                     : acch(n) < 0 ? 0 : ((acch(n) ^ accm(n)) < 0 ? 0xFFFF : (uint16_t)accm(n)));
        }
        break;
    case 0x02: case 0x0A:                                                // VRNDP / VRNDN
        for (int n = 0; n < 8; ++n) {
            int64_t product = t[n];
            if (vs & 1) product *= 65536;                                // по номеру регистра vs
            const bool apply = funct == 0x02 ? acc_[n] >= 0 : acc_[n] < 0;
            if (apply) accAdd(n, product);
            res[n] = (int16_t)saturate(n, true, 0x8000, 0x7FFF);
        }
        break;
    case 0x03:                                                           // VMULQ
        for (int n = 0; n < 8; ++n) {
            int32_t p = (int32_t)s[n] * t[n];
            if (p < 0) p += 31;
            accSet(n, (int64_t)p * 65536);
            res[n] = (int16_t)(clamp16(p >> 1) & ~15);
        }
        break;
    case 0x04:                                                           // VMUDL
        for (int n = 0; n < 8; ++n) {
            accSet(n, ((uint32_t)(uint16_t)s[n] * (uint16_t)t[n]) >> 16);
            res[n] = accl(n);
        }
        break;
    case 0x05:                                                           // VMUDM
        for (int n = 0; n < 8; ++n) { accSet(n, (int64_t)s[n] * (uint16_t)t[n]); res[n] = accm(n); }
        break;
    case 0x06:                                                           // VMUDN
        for (int n = 0; n < 8; ++n) { accSet(n, (int64_t)(uint16_t)s[n] * t[n]); res[n] = accl(n); }
        break;
    case 0x07:                                                           // VMUDH
        for (int n = 0; n < 8; ++n) {
            accSet(n, (int64_t)((int32_t)s[n] * t[n]) * 65536);
            res[n] = (int16_t)saturate(n, true, 0x8000, 0x7FFF);
        }
        break;
    case 0x08: case 0x09:                                                // VMACF / VMACU
        for (int n = 0; n < 8; ++n) {
            accAdd(n, (int64_t)s[n] * t[n] * 2);
            res[n] = (int16_t)(funct == 0x08 ? saturate(n, true, 0x8000, 0x7FFF)
                     : acch(n) < 0 ? 0 : ((acch(n) != 0 || accm(n) < 0) ? 0xFFFF : (uint16_t)accm(n)));
        }
        break;
    case 0x0B:                                                           // VMACQ
        for (int n = 0; n < 8; ++n) {
            int32_t p = (int32_t)(acc_[n] >> 16);
            if (p < 0 && !(p & 32))        p += 32;
            else if (p >= 32 && !(p & 32)) p -= 32;
            accSet(n, (int64_t)p * 65536 + (uint16_t)accl(n));
            res[n] = (int16_t)(clamp16(p >> 1) & ~15);
        }
        break;
    case 0x0C:                                                           // VMADL
        for (int n = 0; n < 8; ++n) {
            accAdd(n, ((uint32_t)(uint16_t)s[n] * (uint16_t)t[n]) >> 16);
            res[n] = (int16_t)saturate(n, false, 0x0000, 0xFFFF);
        }
        break;
    case 0x0D:                                                           // VMADM
        for (int n = 0; n < 8; ++n) {
            accAdd(n, (int64_t)s[n] * (uint16_t)t[n]);
            res[n] = (int16_t)saturate(n, true, 0x8000, 0x7FFF);
        }
        break;
    case 0x0E:                                                           // VMADN
        for (int n = 0; n < 8; ++n) {
            accAdd(n, (int64_t)(uint16_t)s[n] * t[n]);
            res[n] = (int16_t)saturate(n, false, 0x0000, 0xFFFF);
        }
        break;
    case 0x0F:                                                           // VMADH
        for (int n = 0; n < 8; ++n) {
            accAdd(n, (int64_t)((int32_t)s[n] * t[n]) * 65536);
            res[n] = (int16_t)saturate(n, true, 0x8000, 0x7FFF);
        }
        break;

    // ── Сложения ─────────────────────────────────────────────────────────────
    case 0x10: case 0x11:                                                // VADD / VSUB
        for (int n = 0; n < 8; ++n) {
            const int32_t c = bit(vcoLo_, n);
            const int32_t r = funct == 0x10 ? s[n] + t[n] + c : s[n] - t[n] - c;
            setAccl(n, (int16_t)r);
            res[n] = clamp16(r);
        }
        vcoLo_ = vcoHi_ = 0;
        break;
    case 0x13:                                                           // VABS
        for (int n = 0; n < 8; ++n) {
            int16_t r;
            if (s[n] < 0)       r = (int16_t)-(int32_t)t[n];
            else if (s[n] == 0) r = 0;
            else                r = t[n];
            setAccl(n, r);
            res[n] = (s[n] < 0 && t[n] == -32768) ? (int16_t)0x7FFF : r;
        }
        break;
    case 0x14: {                                                         // VADDC
        uint8_t carry = 0;
        for (int n = 0; n < 8; ++n) {
            const uint32_t r = (uint32_t)(uint16_t)s[n] + (uint16_t)t[n];
            res[n] = (int16_t)r;
            setAccl(n, res[n]);
            carry |= (uint8_t)((r >> 16) << n);
        }
        vcoLo_ = carry; vcoHi_ = 0;
        break;
    }
    case 0x15: {                                                         // VSUBC
        uint8_t carry = 0, ne = 0;
        for (int n = 0; n < 8; ++n) {
            const int32_t r = (int32_t)(uint16_t)s[n] - (uint16_t)t[n];
            res[n] = (int16_t)r;
            setAccl(n, res[n]);
            carry |= (uint8_t)((r < 0) << n);
            ne    |= (uint8_t)((r != 0) << n);
        }
        vcoLo_ = carry; vcoHi_ = ne;
        break;
    }
    case 0x1D:                                                           // VSAR
        for (int n = 0; n < 8; ++n)
            res[n] = e == 8 ? acch(n) : e == 9 ? accm(n) : e == 10 ? accl(n) : (int16_t)0;
        break;

    // ── Сравнения и выбор ────────────────────────────────────────────────────
    case 0x20: case 0x21: case 0x22: case 0x23: {                        // VLT / VEQ / VNE / VGE
        uint8_t cc = 0;
        for (int n = 0; n < 8; ++n) {
            const bool eqFlag = bit(vcoLo_, n) && bit(vcoHi_, n);
            bool c;
            switch (funct) {
            case 0x20: c = s[n] < t[n] || (s[n] == t[n] && eqFlag); break;
            case 0x21: c = s[n] == t[n] && !bit(vcoHi_, n); break;
            case 0x22: c = s[n] != t[n] || bit(vcoHi_, n); break;
            default:   c = s[n] > t[n] || (s[n] == t[n] && !eqFlag); break;
            }
            cc |= (uint8_t)(c << n);
            res[n] = c ? s[n] : t[n];
            setAccl(n, res[n]);
        }
        vccLo_ = cc; vccHi_ = 0;
        vcoLo_ = vcoHi_ = 0;
        break;
    }
    case 0x24: {                                                         // VCL
        uint8_t lo = vccLo_, hi = vccHi_;
        for (int n = 0; n < 8; ++n) {
            const uint16_t us = (uint16_t)s[n], ut = (uint16_t)t[n];
            bool le = bit(lo, n), ge = bit(hi, n);
            if (bit(vcoLo_, n)) {
                if (!bit(vcoHi_, n)) {
                    const uint32_t sum = (uint32_t)us + ut;
                    const bool lz = (sum & 0xFFFF) == 0, carry = sum > 0xFFFF;
                    le = bit(vce_, n) ? (lz || !carry) : (lz && !carry);
                }
                res[n] = le ? (int16_t)-(int32_t)t[n] : s[n];
            } else {
                if (!bit(vcoHi_, n)) ge = us >= ut;
                res[n] = ge ? t[n] : s[n];
            }
            lo = (uint8_t)((lo & ~(1 << n)) | (le << n));
            hi = (uint8_t)((hi & ~(1 << n)) | (ge << n));
            setAccl(n, res[n]);
        }
        vccLo_ = lo; vccHi_ = hi;
        vcoLo_ = vcoHi_ = 0; vce_ = 0;
        break;
    }
    case 0x25: {                                                         // VCH
        uint8_t lo = 0, hi = 0, co = 0, ne = 0, ce = 0;
        for (int n = 0; n < 8; ++n) {
            const bool neqPattern = (uint16_t)s[n] != ((uint16_t)t[n] ^ 0xFFFF);
            bool le, ge;
            if ((s[n] ^ t[n]) < 0) {
                const int32_t r = s[n] + t[n];
                le = r <= 0; ge = t[n] < 0;
                co |= (uint8_t)(1 << n);
                ne |= (uint8_t)((r != 0 && neqPattern) << n);
                ce |= (uint8_t)((r == -1) << n);
                res[n] = le ? (int16_t)-(int32_t)t[n] : s[n];
            } else {
                const int32_t r = s[n] - t[n];
                le = t[n] < 0; ge = r >= 0;
                ne |= (uint8_t)((r != 0 && neqPattern) << n);
                res[n] = ge ? t[n] : s[n];
            }
            lo |= (uint8_t)(le << n); hi |= (uint8_t)(ge << n);
            setAccl(n, res[n]);
        }
        vccLo_ = lo; vccHi_ = hi; vcoLo_ = co; vcoHi_ = ne; vce_ = ce;
        break;
    }
    case 0x26: {                                                         // VCR
        uint8_t lo = 0, hi = 0;
        for (int n = 0; n < 8; ++n) {
            bool le, ge;
            if ((s[n] ^ t[n]) < 0) {
                ge = t[n] < 0;
                le = s[n] + t[n] + 1 <= 0;
                res[n] = le ? (int16_t)~t[n] : s[n];
            } else {
                le = t[n] < 0;
                ge = s[n] - t[n] >= 0;
                res[n] = ge ? t[n] : s[n];
            }
            lo |= (uint8_t)(le << n); hi |= (uint8_t)(ge << n);
            setAccl(n, res[n]);
        }
        vccLo_ = lo; vccHi_ = hi;
        vcoLo_ = vcoHi_ = 0; vce_ = 0;
        break;
    }
    case 0x27:                                                           // VMRG
        for (int n = 0; n < 8; ++n) { res[n] = bit(vccLo_, n) ? s[n] : t[n]; setAccl(n, res[n]); }
        vcoLo_ = vcoHi_ = 0;
        break;

    // ── Логика ───────────────────────────────────────────────────────────────
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D:
        for (int n = 0; n < 8; ++n) {
            int16_t r;
            switch (funct) {
            case 0x28: r = (int16_t)(s[n] & t[n]); break;                // VAND
            case 0x29: r = (int16_t)~(s[n] & t[n]); break;               // VNAND
            case 0x2A: r = (int16_t)(s[n] | t[n]); break;                // VOR
            case 0x2B: r = (int16_t)~(s[n] | t[n]); break;               // VNOR
            case 0x2C: r = (int16_t)(s[n] ^ t[n]); break;                // VXOR
            default:   r = (int16_t)~(s[n] ^ t[n]); break;               // VNXOR
            }
            res[n] = r;
            setAccl(n, r);
        }
        break;

    default:                                                             // зарезервированные
        for (int n = 0; n < 8; ++n) { setAccl(n, (int16_t)(s[n] + t[n])); res[n] = 0; }
        break;
    }
    for (int n = 0; n < 8; ++n) vr_[vd][n] = res[n];
}

// VRCP/VRSQ (одинарная точность), …L (младшая половина двойной), …H (старшая),
// VMOV. de — элемент назначения, e — элемент источника.
void Rsp::divide(uint32_t funct, int vd, int de, int vt, int e)
{
    for (int n = 0; n < 8; ++n) setAccl(n, vr_[vt][laneOf(e, n)]);
    const int16_t src = vr_[vt][e & 7];
    switch (funct) {
    case 0x30: case 0x31: case 0x34: case 0x35: {
        const bool sq = funct >= 0x34;
        const int32_t input = ((funct & 1) && divDp_)
            ? (int32_t)(((uint32_t)(uint16_t)divIn_ << 16) | (uint16_t)src) : (int32_t)src;
        const int32_t mask = input >> 31;
        int32_t data = input ^ mask;
        if (input > -32768) data -= mask;
        int32_t result;
        if (data == 0) {
            result = 0x7FFFFFFF;
        } else if (input == -32768) {
            result = (int32_t)0xFFFF0000u;
        } else {
            const uint32_t shift = (uint32_t)__builtin_clz((uint32_t)data);
            const uint32_t index = (uint32_t)((((uint64_t)(uint32_t)data << shift) & 0x7FC00000u) >> 22);
            uint32_t r;
            if (!sq) {
                r = (0x10000u | tables().rcp[index]) << 14;
                r >>= 31 - shift;
            } else {
                r = (0x10000u | tables().rsq[(index & 0x1FE) | (shift & 1)]) << 14;
                r >>= (31 - shift) >> 1;
            }
            result = (int32_t)(r ^ (uint32_t)mask);
        }
        divDp_ = false;
        divOut_ = (int16_t)(result >> 16);
        vr_[vd][de & 7] = (int16_t)result;
        break;
    }
    case 0x32: case 0x36:                                                // VRCPH / VRSQH
        divDp_ = true;
        divIn_ = src;
        vr_[vd][de & 7] = divOut_;
        break;
    default:                                                             // VMOV
        vr_[vd][de & 7] = vr_[vt][laneOf(e, de & 7)];
        break;
    }
}

// ─── Векторные загрузки (LWC2) ────────────────────────────────────────────────
void Rsp::vectorLoad(uint32_t instr)
{
    const uint32_t base = r_[(instr >> 21) & 31];
    const int vt = (instr >> 16) & 31;
    const uint32_t op = (instr >> 11) & 31;
    const int e = (instr >> 7) & 15;
    const int32_t off = (int32_t)((instr & 0x7F) ^ 0x40) - 0x40;         // 7 бит со знаком

    switch (op) {
    case 0x00: setVbyte(vt, e, rd8(base + off)); break;                  // LBV
    case 0x01: case 0x02: case 0x03: {                                   // LSV / LLV / LDV
        const int size = 1 << op;
        const uint32_t a = base + (uint32_t)(off * size);
        for (int i = 0; i < size && e + i < 16; ++i) setVbyte(vt, e + i, rd8(a + i));
        break;
    }
    case 0x04: {                                                         // LQV: до конца блока 16
        const uint32_t a = base + (uint32_t)(off * 16);
        const uint32_t n = 16 - (a & 15);
        for (uint32_t i = 0; i < n && e + (int)i < 16; ++i) setVbyte(vt, e + (int)i, rd8(a + i));
        break;
    }
    case 0x05: {                                                         // LRV: от начала блока
        const uint32_t a = base + (uint32_t)(off * 16);
        uint32_t addr = a & ~15u;
        for (int k = e + 16 - (int)(a & 15); k < 16; ++k) setVbyte(vt, k, rd8(addr++));
        break;
    }
    case 0x06: case 0x07: {                                              // LPV / LUV
        const uint32_t a = base + (uint32_t)(off * 8);
        const int index = (int)(a & 7) - e;
        const uint32_t aligned = a & ~7u;
        const int sh = op == 0x06 ? 8 : 7;
        for (int n = 0; n < 8; ++n) vr_[vt][n] = (int16_t)(rd8(aligned + ((index + n) & 15)) << sh);
        break;
    }
    case 0x08: {                                                         // LHV
        const uint32_t a = base + (uint32_t)(off * 16);
        const int index = (int)(a & 7) - e;
        const uint32_t aligned = a & ~7u;
        for (int n = 0; n < 8; ++n) vr_[vt][n] = (int16_t)(rd8(aligned + ((index + n * 2) & 15)) << 7);
        break;
    }
    case 0x09: {                                                         // LFV
        const uint32_t a = base + (uint32_t)(off * 16);
        const int index = (int)(a & 7) - e;
        const uint32_t aligned = a & ~7u;
        uint16_t tmp[8];
        for (int n = 0; n < 4; ++n) {
            tmp[n]     = (uint16_t)(rd8(aligned + ((index + n * 4) & 15)) << 7);
            tmp[n + 4] = (uint16_t)(rd8(aligned + ((index + n * 4 + 8) & 15)) << 7);
        }
        for (int k = e; k < std::min(e + 8, 16); ++k)
            setVbyte(vt, k, (k & 1) ? (uint8_t)tmp[k >> 1] : (uint8_t)(tmp[k >> 1] >> 8));
        break;
    }
    case 0x0B: {                                                         // LTV: транспонированная загрузка
        const uint32_t a = base + (uint32_t)(off * 16);
        const uint32_t begin = a & ~7u;
        uint32_t addr = begin + ((e + (a & 8)) & 15);
        const int vbase = vt & ~7;
        int voff = e >> 1;
        for (int i = 0; i < 8; ++i) {
            setVbyte(vbase + voff, i * 2, rd8(addr++));
            if (addr == begin + 16) addr = begin;
            setVbyte(vbase + voff, i * 2 + 1, rd8(addr++));
            if (addr == begin + 16) addr = begin;
            voff = (voff + 1) & 7;
        }
        break;
    }
    default: break;
    }
}

// ─── Векторные сохранения (SWC2) ──────────────────────────────────────────────
void Rsp::vectorStore(uint32_t instr)
{
    const uint32_t base = r_[(instr >> 21) & 31];
    const int vt = (instr >> 16) & 31;
    const uint32_t op = (instr >> 11) & 31;
    const int e = (instr >> 7) & 15;
    const int32_t off = (int32_t)((instr & 0x7F) ^ 0x40) - 0x40;

    switch (op) {
    case 0x00: wr8(base + off, vbyte(vt, e)); break;                     // SBV
    case 0x01: case 0x02: case 0x03: {                                   // SSV / SLV / SDV
        const int size = 1 << op;
        const uint32_t a = base + (uint32_t)(off * size);
        for (int i = 0; i < size; ++i) wr8(a + i, vbyte(vt, (e + i) & 15));
        break;
    }
    case 0x04: {                                                         // SQV
        const uint32_t a = base + (uint32_t)(off * 16);
        const uint32_t n = 16 - (a & 15);
        for (uint32_t i = 0; i < n; ++i) wr8(a + i, vbyte(vt, (e + (int)i) & 15));
        break;
    }
    case 0x05: {                                                         // SRV
        const uint32_t a = base + (uint32_t)(off * 16);
        const int shift = 16 - (int)(a & 15);
        uint32_t addr = a & ~15u;
        for (int k = e; k < e + (int)(a & 15); ++k) wr8(addr++, vbyte(vt, (k + shift) & 15));
        break;
    }
    case 0x06: case 0x07: {                                              // SPV / SUV
        uint32_t a = base + (uint32_t)(off * 8);
        for (int k = e; k < e + 8; ++k) {
            const bool packed = ((k & 15) < 8) == (op == 0x06);
            wr8(a++, packed ? vbyte(vt, (k & 7) << 1) : (uint8_t)((uint16_t)vr_[vt][k & 7] >> 7));
        }
        break;
    }
    case 0x08: {                                                         // SHV
        const uint32_t a = base + (uint32_t)(off * 16);
        const int index = (int)(a & 7);
        const uint32_t aligned = a & ~7u;
        for (int n = 0; n < 8; ++n) {
            const int b = e + n * 2;
            const uint8_t v = (uint8_t)(vbyte(vt, b & 15) << 1 | vbyte(vt, (b + 1) & 15) >> 7);
            wr8(aligned + ((index + n * 2) & 15), v);
        }
        break;
    }
    case 0x09: {                                                         // SFV
        const uint32_t a = base + (uint32_t)(off * 16);
        int index = (int)(a & 7);
        const uint32_t aligned = a & ~7u;
        for (int k = e >> 1; k < (e >> 1) + 4; ++k) {
            wr8(aligned + (index & 15), (uint8_t)((uint16_t)vr_[vt][k & 7] >> 7));
            index += 4;
        }
        break;
    }
    case 0x0A: {                                                         // SWV
        const uint32_t a = base + (uint32_t)(off * 16);
        int index = (int)(a & 7);
        const uint32_t aligned = a & ~7u;
        for (int k = e; k < e + 16; ++k) wr8(aligned + (index++ & 15), vbyte(vt, k & 15));
        break;
    }
    case 0x0B: {                                                         // STV
        const uint32_t a = base + (uint32_t)(off * 16);
        int element = 16 - (e & ~1);
        int index = (int)(a & 7) - (e & ~1);
        const uint32_t aligned = a & ~7u;
        for (int v = vt & ~7; v < (vt & ~7) + 8; ++v) {
            wr8(aligned + (index++ & 15), vbyte(v, element++ & 15));
            wr8(aligned + (index++ & 15), vbyte(v, element++ & 15));
        }
        break;
    }
    default: break;
    }
}

// ─── Save state ───────────────────────────────────────────────────────────────
template<class S> void Rsp::serialize(S& s)
{
    s.io(pc); s.io(nextPc_); s.io(curPc_);
    for (auto& r : r_) s.io(r);
    for (auto& v : vr_) for (auto& x : v) s.io(x);
    for (auto& a : acc_) s.io(a);
    s.io(vcoLo_); s.io(vcoHi_); s.io(vccLo_); s.io(vccHi_); s.io(vce_);
    s.io(divIn_); s.io(divOut_); s.io(divDp_);
}

template void Rsp::serialize<StateWriter>(StateWriter&);
template void Rsp::serialize<StateReader>(StateReader&);
