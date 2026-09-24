// gb_cart.cpp — картридж Game Boy: заголовок, MBC1/2/3/5, часы, батарейка.
#include "gb_cart.h"
#include "console/state_io.h"
#include <algorithm>
#include <cstring>
#include <ctime>

namespace {

// Размер ОЗУ по байту заголовка $0149.
uint32_t ramSizeFromHeader(uint8_t code)
{
    switch (code) {
    case 0x01: return 0x800;      // 2 КБ (неофициальный)
    case 0x02: return 0x2000;     // 8 КБ
    case 0x03: return 0x8000;     // 32 КБ
    case 0x04: return 0x20000;    // 128 КБ
    case 0x05: return 0x10000;    // 64 КБ
    default:   return 0;
    }
}

void put32(std::vector<uint8_t>& v, uint32_t x)
{
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t)(x >> (8 * i)));
}
uint32_t get32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

} // namespace

// ─── Загрузка ─────────────────────────────────────────────────────────────────
bool GbCart::load(std::vector<uint8_t> rom)
{
    if (rom.size() < 0x150) return false;
    rom_ = std::move(rom);

    // ПЗУ дополняем до степени двойки банков: номер банка просто маскируется.
    size_t banks = std::max<size_t>(2, (rom_.size() + 0x3FFF) / 0x4000);
    size_t pow2 = 1;
    while (pow2 < banks) pow2 <<= 1;
    rom_.resize(pow2 * 0x4000, 0xFF);
    romBanks_ = (uint32_t)pow2;

    cgbFlag_ = rom_[0x143];
    title_.clear();
    for (uint16_t a = 0x134; a < 0x144; ++a) {
        uint8_t ch = rom_[a];
        if (ch == 0 || (a >= 0x13F && (cgbFlag_ & 0x80))) break;  // у CGB-игр заголовок короче
        if (ch >= 0x20 && ch < 0x7F) title_ += (char)ch;
    }

    const uint8_t type = rom_[0x147];
    battery_ = false;
    rtc_ = false;
    switch (type) {
    case 0x00: case 0x08:              mbc_ = Mbc::None; break;
    case 0x09:                         mbc_ = Mbc::None; battery_ = true; break;
    case 0x01: case 0x02:              mbc_ = Mbc::Mbc1; break;
    case 0x03: case 0xFF:              mbc_ = Mbc::Mbc1; battery_ = true; break;  // $FF — HuC1, как MBC1
    case 0x05:                         mbc_ = Mbc::Mbc2; break;
    case 0x06:                         mbc_ = Mbc::Mbc2; battery_ = true; break;
    case 0x0F: case 0x10:              mbc_ = Mbc::Mbc3; battery_ = true; rtc_ = true; break;
    case 0x11: case 0x12:              mbc_ = Mbc::Mbc3; break;
    case 0x13:                         mbc_ = Mbc::Mbc3; battery_ = true; break;
    case 0x19: case 0x1A: case 0x1C: case 0x1D: mbc_ = Mbc::Mbc5; break;
    case 0x1B: case 0x1E:              mbc_ = Mbc::Mbc5; battery_ = true; break;
    default:                           return false;
    }

    uint32_t ramBytes = (mbc_ == Mbc::Mbc2) ? 512u : ramSizeFromHeader(rom_[0x149]);
    ram_.assign(ramBytes, 0xFF);
    ramBanks_ = ramBytes / 0x2000;

    romHash_ = 2166136261u;
    for (uint8_t b : rom_) { romHash_ ^= b; romHash_ *= 16777619u; }

    resetMapper();
    rtcLive_ = Rtc{};
    rtcLatched_ = Rtc{};
    rtcLatchPrev_ = 0xFF;
    ramDirty_ = false;
    return true;
}

void GbCart::resetMapper()
{
    ramEnable_ = false;
    romBank_ = 1;
    ramBank_ = 0;
    mode_ = 0;
}

// ─── Адресация ────────────────────────────────────────────────────────────────
uint32_t GbCart::romOffset(uint32_t bank, uint16_t addr) const
{
    return (bank % romBanks_) * 0x4000u + (addr & 0x3FFFu);
}

uint32_t GbCart::ramOffset(uint16_t addr) const
{
    uint32_t bank = 0;
    switch (mbc_) {
    case Mbc::Mbc1: bank = mode_ ? (ramBank_ & 3u) : 0u; break;
    case Mbc::Mbc3: bank = ramBank_ & 7u; break;
    case Mbc::Mbc5: bank = ramBank_ & 0x0Fu; break;
    default: break;
    }
    if (ramBanks_ > 0) bank %= ramBanks_; else bank = 0;
    return (bank * 0x2000u + (addr & 0x1FFFu)) % (uint32_t)ram_.size();
}

// ─── Чтение ───────────────────────────────────────────────────────────────────
uint8_t GbCart::read(uint16_t addr) const
{
    if (addr < 0x4000) {
        // MBC1 в режиме 1 подставляет старшие биты и в нижнее окно (большие ПЗУ).
        uint32_t bank = (mbc_ == Mbc::Mbc1 && mode_) ? ((ramBank_ & 3u) << 5) : 0u;
        return rom_[romOffset(bank, addr)];
    }
    if (addr < 0x8000) {
        uint32_t bank = 1;
        switch (mbc_) {
        case Mbc::None: bank = 1; break;
        case Mbc::Mbc1: bank = ((ramBank_ & 3u) << 5) | (romBank_ & 0x1Fu); break;
        case Mbc::Mbc2: bank = romBank_ & 0x0Fu; break;
        case Mbc::Mbc3: bank = romBank_ & 0x7Fu; break;
        case Mbc::Mbc5: bank = romBank_ & 0x1FFu; break;
        }
        return rom_[romOffset(bank, addr)];
    }
    if (addr >= 0xA000 && addr < 0xC000) {
        if (mbc_ == Mbc::None) return ram_.empty() ? 0xFF : ram_[ramOffset(addr)];
        if (!ramEnable_) return 0xFF;
        if (mbc_ == Mbc::Mbc2) return (uint8_t)(0xF0 | (ram_[addr & 0x1FFu] & 0x0F));
        if (mbc_ == Mbc::Mbc3 && ramBank_ >= 0x08) {
            switch (ramBank_) {
            case 0x08: return rtcLatched_.s;
            case 0x09: return rtcLatched_.m;
            case 0x0A: return rtcLatched_.h;
            case 0x0B: return rtcLatched_.dl;
            case 0x0C: return rtcLatched_.dh;
            default:   return 0xFF;
            }
        }
        return ram_.empty() ? 0xFF : ram_[ramOffset(addr)];
    }
    return 0xFF;
}

// ─── Запись: регистры MBC и ОЗУ ──────────────────────────────────────────────
void GbCart::write(uint16_t addr, uint8_t data)
{
    if (addr < 0x8000) {
        switch (mbc_) {
        case Mbc::None:
            break;
        case Mbc::Mbc1:
            if      (addr < 0x2000) ramEnable_ = (data & 0x0F) == 0x0A;
            else if (addr < 0x4000) { romBank_ = data & 0x1F; if (!romBank_) romBank_ = 1; }
            else if (addr < 0x6000) ramBank_ = data & 3;
            else                    mode_ = data & 1;
            break;
        case Mbc::Mbc2:
            // Один регистр на всю нижнюю половину: бит 8 адреса выбирает,
            // включение ОЗУ это или номер банка.
            if (addr < 0x4000) {
                if (addr & 0x100) { romBank_ = data & 0x0F; if (!romBank_) romBank_ = 1; }
                else              ramEnable_ = (data & 0x0F) == 0x0A;
            }
            break;
        case Mbc::Mbc3:
            if      (addr < 0x2000) ramEnable_ = (data & 0x0F) == 0x0A;
            else if (addr < 0x4000) { romBank_ = data & 0x7F; if (!romBank_) romBank_ = 1; }
            else if (addr < 0x6000) ramBank_ = data;
            else {
                // Запись 0, затем 1 защёлкивает показания часов.
                if (rtcLatchPrev_ == 0x00 && data == 0x01) rtcLatched_ = rtcLive_;
                rtcLatchPrev_ = data;
            }
            break;
        case Mbc::Mbc5:
            if      (addr < 0x2000) ramEnable_ = (data & 0x0F) == 0x0A;
            else if (addr < 0x3000) romBank_ = (uint16_t)((romBank_ & 0x100) | data);
            else if (addr < 0x4000) romBank_ = (uint16_t)((romBank_ & 0xFF) | ((data & 1) << 8));
            else if (addr < 0x6000) ramBank_ = data & 0x0F;
            break;
        }
        return;
    }

    if (addr >= 0xA000 && addr < 0xC000) {
        if (mbc_ != Mbc::None && !ramEnable_) return;
        if (mbc_ == Mbc::Mbc2) {
            ram_[addr & 0x1FFu] = data & 0x0F;
            ramDirty_ = true;
            return;
        }
        if (mbc_ == Mbc::Mbc3 && ramBank_ >= 0x08) {
            switch (ramBank_) {
            case 0x08: rtcLive_.s = data & 0x3F; break;
            case 0x09: rtcLive_.m = data & 0x3F; break;
            case 0x0A: rtcLive_.h = data & 0x1F; break;
            case 0x0B: rtcLive_.dl = data; break;
            case 0x0C: rtcLive_.dh = data & 0xC1; break;
            default: break;
            }
            ramDirty_ = true;
            return;
        }
        if (ram_.empty()) return;
        ram_[ramOffset(addr)] = data;
        ramDirty_ = true;
    }
}

// ─── Часы реального времени MBC3 ─────────────────────────────────────────────
void GbCart::rtcTickSecond()
{
    Rtc& r = rtcLive_;
    r.s = (uint8_t)((r.s + 1) & 0x3F);
    if (r.s != 60) return;
    r.s = 0;
    r.m = (uint8_t)((r.m + 1) & 0x3F);
    if (r.m != 60) return;
    r.m = 0;
    r.h = (uint8_t)((r.h + 1) & 0x1F);
    if (r.h != 24) return;
    r.h = 0;
    uint16_t day = (uint16_t)(r.dl | ((r.dh & 1) << 8));
    day = (uint16_t)(day + 1);
    if (day >= 512) { day = 0; r.dh |= 0x80; }        // перенос дня
    r.dl = (uint8_t)day;
    r.dh = (uint8_t)((r.dh & 0xFE) | ((day >> 8) & 1));
}

void GbCart::advanceRtc(uint64_t seconds)
{
    if (!rtc_ || (rtcLive_.dh & 0x40)) return;       // бит 6 — часы остановлены
    // Пока значения вне диапазона (игра могла записать что угодно) — по секунде.
    Rtc& r = rtcLive_;
    while (seconds && (r.s >= 60 || r.m >= 60 || r.h >= 24)) { rtcTickSecond(); --seconds; }
    if (!seconds) return;
    uint64_t day = (uint64_t)(r.dl | ((r.dh & 1) << 8));
    uint64_t total = r.s + 60 * (r.m + 60 * (r.h + 24 * day)) + seconds;
    r.s = (uint8_t)(total % 60); total /= 60;
    r.m = (uint8_t)(total % 60); total /= 60;
    r.h = (uint8_t)(total % 24); total /= 24;
    if (total >= 512) { r.dh |= 0x80; total %= 512; }
    r.dl = (uint8_t)total;
    r.dh = (uint8_t)((r.dh & 0xFE) | ((total >> 8) & 1));
    ramDirty_ = true;
}

// ─── Батарейка: ОЗУ + часы (48 байт, как у VBA/BGB) ──────────────────────────
std::vector<uint8_t> GbCart::saveData() const
{
    std::vector<uint8_t> out(ram_);
    if (rtc_) {
        for (const Rtc* r : { &rtcLive_, &rtcLatched_ }) {
            put32(out, r->s); put32(out, r->m); put32(out, r->h);
            put32(out, r->dl); put32(out, r->dh);
        }
        uint64_t now = (uint64_t)std::time(nullptr);
        put32(out, (uint32_t)now);
        put32(out, (uint32_t)(now >> 32));
    }
    return out;
}

bool GbCart::loadSaveData(const std::vector<uint8_t>& data, uint64_t nowUnix)
{
    if (data.size() < ram_.size()) return false;
    std::copy(data.begin(), data.begin() + (std::ptrdiff_t)ram_.size(), ram_.begin());
    size_t tail = data.size() - ram_.size();
    if (rtc_ && tail >= 44) {
        const uint8_t* p = data.data() + ram_.size();
        Rtc* regs[2] = { &rtcLive_, &rtcLatched_ };
        for (int k = 0; k < 2; ++k, p += 20) {
            regs[k]->s = (uint8_t)get32(p);      regs[k]->m = (uint8_t)get32(p + 4);
            regs[k]->h = (uint8_t)get32(p + 8);  regs[k]->dl = (uint8_t)get32(p + 12);
            regs[k]->dh = (uint8_t)get32(p + 16);
        }
        if (tail >= 48) {
            uint64_t saved = (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
            if (nowUnix > saved) advanceRtc(nowUnix - saved);   // часы шли, пока игра была выключена
        }
    }
    ramDirty_ = false;
    return true;
}

// ─── Save state ───────────────────────────────────────────────────────────────
template<class S> void GbCart::serialize(S& s)
{
    s.vec(ram_);
    s.io(ramEnable_); s.io(romBank_); s.io(ramBank_); s.io(mode_);
    s.io(rtcLive_); s.io(rtcLatched_); s.io(rtcLatchPrev_);
    if (S::reading && battery_) ramDirty_ = true;
}

template void GbCart::serialize<StateWriter>(StateWriter&);
template void GbCart::serialize<StateReader>(StateReader&);
