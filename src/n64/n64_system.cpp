// n64_system.cpp — шина Nintendo 64: регистры RCP, PIF, картридж, загрузка.
#include "n64_system.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

uint32_t crc32(const uint8_t* p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}

// Частота видеоинтерфейса (от неё же считается частота звука)
double viClock(N64System::Tv tv)
{
    switch (tv) {
    case N64System::Tv::PAL:  return 49656530.0;
    case N64System::Tv::MPAL: return 48628316.0;
    default:                  return 48681812.0;
    }
}

constexpr uint32_t SRAM_SIZE = 0x8000;
constexpr uint32_t SI_DMA_CYCLES = 6000;      // обмен с PIF заметно медленный

} // namespace

N64System::N64System()
    : rdram_(RDRAM_SIZE), frame_(320 * 240, 0xFF000000u)
{
    cpu.connect(this);
    eventAt_.fill(NEVER);
}

// ─── Картридж ─────────────────────────────────────────────────────────────────
bool N64System::loadRom(std::vector<uint8_t> data)
{
    if (data.size() < 0x1000) return false;
    if (data[0] == 0x37 && data[1] == 0x80) {                // .v64: байты попарно
        for (size_t i = 0; i + 1 < data.size(); i += 2) std::swap(data[i], data[i + 1]);
    } else if (data[3] == 0x80 && data[0] != 0x80) {         // .n64: слова задом наперёд
        for (size_t i = 0; i + 3 < data.size(); i += 4) {
            std::swap(data[i], data[i + 3]);
            std::swap(data[i + 1], data[i + 2]);
        }
    } else if (data[0] != 0x80) {
        return false;
    }
    rom_ = std::move(data);
    detectCartridge();
    reset();
    return true;
}

void N64System::detectCartridge()
{
    title_.assign(reinterpret_cast<const char*>(&rom_[0x20]), 20);
    while (!title_.empty() && (title_.back() == ' ' || title_.back() == '\0')) title_.pop_back();
    gameCode_.assign(reinterpret_cast<const char*>(&rom_[0x3B]), 4);

    // Регион → телевизионный стандарт (от него зависят режимы VI у libultra)
    switch (rom_[0x3E]) {
    case 'D': case 'F': case 'H': case 'I': case 'L': case 'P':
    case 'S': case 'U': case 'W': case 'X': case 'Y': case 'Z':
        tv_ = Tv::PAL; break;
    case 'B': tv_ = Tv::MPAL; break;
    default:  tv_ = Tv::NTSC; break;
    }

    // CIC — по контрольной сумме IPL3 (у каждого чипа свой загрузчик)
    switch (crc32(&rom_[0x40], 0x1000 - 0x40)) {
    case 0x6170A4A1: cic_ = Cic::X101; break;
    case 0x0B050EE0: cic_ = Cic::X103; break;
    case 0x98BC2C86: cic_ = Cic::X105; break;
    case 0xACC8580A: cic_ = Cic::X106; break;
    case 0x009E9EA3: cic_ = Cic::X7102; break;
    default:         cic_ = Cic::X102; break;
    }

    // Тип сохранения. ponytail: короткий список; нет игры — EEPROM 4K, а если
    // игра пишет в SRAM, тип сам переключится на SRAM.
    struct Entry { const char* id; Save save; };
    static const Entry kSaves[] = {
        {"ZL", Save::Sram}, {"AL", Save::Sram}, {"FZ", Save::Sram}, {"TE", Save::Sram},
        {"MF", Save::Sram}, {"PO", Save::Sram}, {"KG", Save::Sram}, {"YW", Save::Sram},
        {"DO", Save::Eeprom16K}, {"B7", Save::Eeprom16K}, {"PD", Save::Eeprom16K},
        {"FU", Save::Eeprom16K}, {"YS", Save::Eeprom16K}, {"MX", Save::Eeprom16K},
        {"MV", Save::Eeprom16K}, {"UB", Save::Eeprom16K},
    };
    save_ = Save::Eeprom4K;
    for (const Entry& e : kSaves)
        if (gameCode_[1] == e.id[0] && gameCode_[2] == e.id[1]) save_ = e.save;
    eeprom_.assign(save_ == Save::Eeprom16K ? 2048 : 512, 0xFF);
    sram_.assign(SRAM_SIZE, 0xFF);
}

std::vector<uint8_t> N64System::saveData() const
{
    return save_ == Save::Sram ? sram_ : eeprom_;
}

bool N64System::loadSaveData(const std::vector<uint8_t>& data)
{
    if (data.size() == sram_.size())   { sram_ = data; save_ = Save::Sram; return true; }
    if (data.size() == eeprom_.size()) { eeprom_ = data; return true; }
    return false;
}

// ─── Включение ────────────────────────────────────────────────────────────────
void N64System::reset()
{
    std::fill(rdram_.begin(), rdram_.end(), 0);
    spMem_.fill(0);
    pifRam_.fill(0);
    isvBuf_.fill(0);
    eventAt_.fill(NEVER);
    miMode_ = miIntr_ = miMask_ = 0;
    vi_.fill(0);
    viLine_ = viField_ = 0;
    aiCount_ = 0;
    aiDramAddr_ = aiControl_ = aiDacRate_ = aiBitRate_ = 0;
    aiResample_ = 0.0;
    piDram_ = piCart_ = piStatus_ = 0;
    std::fill(std::begin(piBsd_), std::end(piBsd_), 0);
    piBusy_ = siBusy_ = false;
    siDram_ = 0;
    joybusArmed_ = false;
    ri_.fill(0);
    spMemAddr_ = spDramAddr_ = spRdLen_ = spWrLen_ = 0;
    spStatus_ = 1;
    spSemaphore_ = spPc_ = 0;
    dpStart_ = dpEnd_ = dpCurrent_ = 0;
    dpStatus_ = 0x80;
    samples_.clear();
    pads_[0].present = true;

    cpu.reset();
    updateIrq();
    bootHle();
    schedule(EV_VI_LINE, viCyclesPerLine());
}

// Что оставляет после себя загрузчик PIF (IPL1/IPL2): IPL3 в DMEM, регистры
// с типом ТВ и зерном CIC, стек в IMEM. Дальше работает IPL3 картриджа.
void N64System::bootHle()
{
    std::memcpy(spMem_.data(), rom_.data(), 0x1000);

    uint8_t seed = 0x3F;
    switch (cic_) {
    case Cic::X103: seed = 0x78; break;
    case Cic::X105: seed = 0x91; break;
    case Cic::X106: seed = 0x85; break;
    default: break;
    }
    // Так процессор оставляет PIF (замерено на железе тестами krom): CU1, FR,
    // SR и 64-битная адресация во всех режимах; адреса исключений — все единицы.
    cpu.cop0[Vr4300::C0_STATUS]   = 0x241000E0;
    cpu.cop0[Vr4300::C0_CAUSE]    = 0x30000000;
    cpu.cop0[Vr4300::C0_EPC]      = ~0ull;
    cpu.cop0[Vr4300::C0_ERROREPC] = ~0ull;
    cpu.cop0[Vr4300::C0_BADVADDR] = ~0ull;
    cpu.cop0[Vr4300::C0_CONTEXT]  = 0x7FFFF0;
    cpu.gpr[19] = 0;                               // s3: картридж, не 64DD
    cpu.gpr[20] = (uint64_t)tv_;                   // s4: ТВ-стандарт
    cpu.gpr[21] = 0;                               // s5: холодный старт
    cpu.gpr[22] = seed;                            // s6: зерно CIC
    cpu.gpr[23] = 0;                               // s7: версия
    cpu.gpr[11] = 0xFFFFFFFFA4000040ull;           // t3
    cpu.gpr[29] = 0xFFFFFFFFA4001FF0ull;           // sp
    cpu.gpr[31] = 0xFFFFFFFFA4001550ull;           // ra

    // Хвост IPL2 в IMEM — его проверяет IPL3 от CIC 6105
    static const uint32_t kIpl2Tail[8] = {
        0x3C0DBFC0, 0x8DA807FC, 0x25AD07C0, 0x31080080,
        0x5500FFFC, 0x3C0DBFC0, 0x8DA80024, 0x3C0BB000,
    };
    for (int i = 0; i < 8; ++i) put32(&spMem_[0x1000 + i * 4], kIpl2Tail[i]);

    // RDRAM уже «настроена»: увидев RI_SELECT ≠ 0, IPL3 пропускает инициализацию
    // модулей памяти. Размер памяти для libultra (osMemSize) кладём сами.
    ri_[0] = 0x0E;                                 // RI_MODE
    ri_[1] = 0x40;                                 // RI_CONFIG
    ri_[3] = 0x14;                                 // RI_SELECT
    ri_[4] = 0x00063634;                           // RI_REFRESH
    put32(&rdram_[cic_ == Cic::X105 ? 0x3F0 : 0x318], RDRAM_SIZE);

    // Тайминги домена 1 PI — из первого слова ПЗУ
    const uint32_t cfg = get32(rom_.data());
    piBsd_[0] = cfg & 0xFF;
    piBsd_[1] = (cfg >> 8) & 0xFF;
    piBsd_[2] = (cfg >> 16) & 0x0F;
    piBsd_[3] = (cfg >> 20) & 0x03;
    vi_[VI_V_INTR] = 1023;

    cpu.jump(0xFFFFFFFFA4000040ull);
}

// ─── Расписание ───────────────────────────────────────────────────────────────
void N64System::schedule(Event e, uint64_t delay)
{
    eventAt_[e] = cpu.cycles() + std::max<uint64_t>(delay, 1);
    cpu.stopAt(eventAt_[e]);
}

void N64System::handleEvent(Event e)
{
    switch (e) {
    case EV_VI_LINE: viLine(); break;
    case EV_PI_DMA:  piBusy_ = false; raiseMi(MI_PI); break;
    case EV_SI_DMA:  siBusy_ = false; raiseMi(MI_SI); break;
    case EV_AI_BUF:
        if (aiCount_ > 0) { aiFifo_[0] = aiFifo_[1]; --aiCount_; }
        if (aiCount_ > 0) aiStartBuffer();
        break;
    default: break;
    }
}

void N64System::runFrame()
{
    frameDone_ = false;
    while (!frameDone_) {
        const uint64_t next = *std::min_element(eventAt_.begin(), eventAt_.end());
        if (cpu.cycles() < next) cpu.runUntil(next);
        const uint64_t now = cpu.cycles();
        for (int e = 0; e < EV_COUNT; ++e) {
            if (eventAt_[e] <= now) {
                eventAt_[e] = NEVER;
                handleEvent((Event)e);
            }
        }
    }

    // Диагностика: состояние раз в кадр (EMUDOR_N64_TRACE=1)
    static const bool s_trace = std::getenv("EMUDOR_N64_TRACE") != nullptr;
    if (s_trace) {
        std::fprintf(stderr, "N64 f=%llu pc=%08X st=%08X cause=%08X mi=%02X/%02X vi_ctrl=%X org=%06X w=%u sp=%X\n",
                     (unsigned long long)frames_, (uint32_t)cpu.pc, (uint32_t)cpu.cop0[Vr4300::C0_STATUS],
                     (uint32_t)cpu.cop0[Vr4300::C0_CAUSE], miIntr_, miMask_, vi_[VI_CTRL],
                     vi_[VI_ORIGIN], vi_[VI_WIDTH], spStatus_);
    }
}

// ─── Память: байты и полуслова ────────────────────────────────────────────────
uint8_t N64System::read8(uint32_t pa)
{
    if (pa < RDRAM_SIZE) return rdram_[pa];
    return (uint8_t)(readIo(pa & ~3u) >> (8 * (3 - (pa & 3))));
}

uint16_t N64System::read16(uint32_t pa)
{
    if (pa < RDRAM_SIZE) return (uint16_t)((rdram_[pa] << 8) | rdram_[pa + 1]);
    return (uint16_t)(readIo(pa & ~3u) >> (16 * (1 - ((pa >> 1) & 1))));
}

void N64System::write8(uint32_t pa, uint8_t v)
{
    if (pa < RDRAM_SIZE) { rdram_[pa] = v; return; }
    const uint32_t sh = 8 * (3 - (pa & 3));
    writeIo(pa & ~3u, (uint32_t)v << sh, 0xFFu << sh);
}

void N64System::write16(uint32_t pa, uint16_t v)
{
    if (pa < RDRAM_SIZE) { rdram_[pa] = (uint8_t)(v >> 8); rdram_[pa + 1] = (uint8_t)v; return; }
    const uint32_t sh = 16 * (1 - ((pa >> 1) & 1));
    writeIo(pa & ~3u, (uint32_t)v << sh, 0xFFFFu << sh);
}

// ─── Регистры: чтение ─────────────────────────────────────────────────────────
uint32_t N64System::readIo(uint32_t pa)
{
    if (pa < 0x04000000) return 0;                                   // регистры RDRAM
    if (pa < 0x04040000) return get32(&spMem_[pa & 0x1FFC]);         // DMEM / IMEM
    if (pa < 0x04100000) {                                           // SP
        if (pa >= 0x04080000) return (pa & 4) ? 0 : spPc_;
        switch (pa & 0x1C) {
        case 0x00: return spMemAddr_;
        case 0x04: return spDramAddr_;
        case 0x08: return spRdLen_;
        case 0x0C: return spWrLen_;
        case 0x10: return spStatus_;
        case 0x1C: { const uint32_t v = spSemaphore_; spSemaphore_ = 1; return v; }
        default:   return 0;                                         // DMA_FULL, DMA_BUSY
        }
    }
    if (pa < 0x04200000) {                                           // DP (команды)
        switch (pa & 0x1C) {
        case 0x00: return dpStart_;
        case 0x04: return dpEnd_;
        case 0x08: return dpCurrent_;
        case 0x0C: return dpStatus_;
        default:   return 0;
        }
    }
    if (pa < 0x04300000) return 0;                                   // DP (span)
    if (pa < 0x04400000) {                                           // MI
        switch (pa & 0xC) {
        case 0x0: return miMode_;
        case 0x4: return 0x02020102;
        case 0x8: return miIntr_;
        default:  return miMask_;
        }
    }
    if (pa < 0x04500000) {                                           // VI
        const uint32_t r = (pa >> 2) & 15;
        if (r == VI_V_CURRENT) return (viLine_ << 1) | viField_;
        return r < 14 ? vi_[r] : 0;
    }
    if (pa < 0x04600000) {                                           // AI
        if ((pa & 0x1C) == 0x0C) {
            uint32_t st = 0;
            if (aiCount_ >= 2) st |= 0x80000001u;                    // очередь полна
            if (aiCount_ >= 1) st |= 0x40000000u;                    // играет
            return st;
        }
        if (aiCount_ == 0 || aiDuration_ == 0) return 0;             // остаток буфера
        const uint64_t played = cpu.cycles() - aiStart_;
        if (played >= aiDuration_) return 0;
        const uint64_t left = aiFifo_[0].len - aiFifo_[0].len * played / aiDuration_;
        return (uint32_t)left & ~7u;
    }
    if (pa < 0x04700000) {                                           // PI
        const uint32_t r = (pa >> 2) & 15;
        switch (r) {
        case 0: return piDram_;
        case 1: return piCart_;
        case 2: case 3: return 0x7F;
        case 4: return (piBusy_ ? 3u : 0u) | ((miIntr_ & MI_PI) ? 8u : 0u);
        default: return r <= 12 ? piBsd_[r - 5] : 0;
        }
    }
    if (pa < 0x04800000) return ri_[(pa >> 2) & 7];                  // RI
    if (pa < 0x04900000) {                                           // SI
        switch (pa & 0x1C) {
        case 0x00: return siDram_;
        case 0x18: return (siBusy_ ? 1u : 0u) | ((miIntr_ & MI_SI) ? 0x1000u : 0u);
        default:   return 0;
        }
    }
    if (pa < 0x08000000) return 0;                                   // 64DD — нет
    if (pa < 0x10000000) return get32(&sram_[(pa - 0x08000000) & (SRAM_SIZE - 4)]);
    if (pa < 0x1FC00000) {                                           // ПЗУ картриджа
        if (pa >= 0x13FF0000 && pa < 0x13FF0200) return get32(&isvBuf_[pa & 0x1FC]);
        const uint32_t off = pa - 0x10000000;
        if (off + 4 <= rom_.size()) return get32(&rom_[off]);
        return ((pa & 0xFFFF) << 16) | (pa & 0xFFFF);                // открытая шина PI
    }
    if (pa >= 0x1FC007C0 && pa < 0x1FC00800) return get32(&pifRam_[pa & 0x3C]);
    return 0;
}

// ─── Регистры: запись ─────────────────────────────────────────────────────────
void N64System::writeIo(uint32_t pa, uint32_t v, uint32_t mask)
{
    auto merge = [&](uint8_t* p) { put32(p, (get32(p) & ~mask) | (v & mask)); };

    if (pa < 0x04000000) return;
    if (pa < 0x04040000) { merge(&spMem_[pa & 0x1FFC]); return; }
    if (pa < 0x04100000) {                                           // SP
        if (pa >= 0x04080000) { if (!(pa & 4)) spPc_ = v & 0xFFC; return; }
        switch (pa & 0x1C) {
        case 0x00: spMemAddr_ = v & 0x1FF8; break;
        case 0x04: spDramAddr_ = v & 0xFFFFF8; break;
        case 0x08: spRdLen_ = v; spDma(false, v); break;
        case 0x0C: spWrLen_ = v; spDma(true, v); break;
        case 0x10: spStatusWrite(v); break;
        case 0x1C: spSemaphore_ = 0; break;
        default: break;
        }
        return;
    }
    if (pa < 0x04200000) {                                           // DP
        switch (pa & 0x1C) {
        case 0x00: dpStart_ = dpCurrent_ = v & 0xFFFFF8; break;
        // ponytail: RDP ещё нет — список команд «выполняется» мгновенно и без рисования
        case 0x04: dpEnd_ = v & 0xFFFFF8; dpCurrent_ = dpEnd_; break;
        case 0x0C: {
            auto pair = [&](int clr, int set, uint32_t flag) {
                if ((v >> clr & 1) && !(v >> set & 1)) dpStatus_ &= ~flag;
                if ((v >> set & 1) && !(v >> clr & 1)) dpStatus_ |= flag;
            };
            pair(0, 1, 0x1);                                         // XBUS (команды из DMEM)
            pair(2, 3, 0x2);                                         // freeze
            pair(4, 5, 0x4);                                         // flush
            break;
        }
        default: break;
        }
        return;
    }
    if (pa < 0x04300000) return;
    if (pa < 0x04400000) {                                           // MI
        if ((pa & 0xC) == 0x0) {
            miMode_ = (miMode_ & ~0x7Fu) | (v & 0x7F);
            if (v & 0x0080) miMode_ &= ~0x080u;
            if (v & 0x0100) miMode_ |=  0x080u;
            if (v & 0x0200) miMode_ &= ~0x100u;
            if (v & 0x0400) miMode_ |=  0x100u;
            if (v & 0x0800) clearMi(MI_DP);
            if (v & 0x1000) miMode_ &= ~0x200u;
            if (v & 0x2000) miMode_ |=  0x200u;
        } else if ((pa & 0xC) == 0xC) {
            for (int i = 0; i < 6; ++i) {
                if (v >> (2 * i) & 1)     miMask_ &= ~(1u << i);
                if (v >> (2 * i + 1) & 1) miMask_ |=  (1u << i);
            }
            updateIrq();
        }
        return;
    }
    if (pa < 0x04500000) {                                           // VI
        const uint32_t r = (pa >> 2) & 15;
        if (r == VI_V_CURRENT) clearMi(MI_VI);
        else if (r < 14) vi_[r] = v;
        return;
    }
    if (pa < 0x04600000) {                                           // AI
        switch (pa & 0x1C) {
        case 0x00: aiDramAddr_ = v & 0xFFFFF8; break;
        case 0x04: {
            const uint32_t len = v & 0x3FFF8;
            if (len && aiCount_ < 2) {
                aiFifo_[aiCount_++] = { aiDramAddr_, len };
                if (aiCount_ == 1) aiStartBuffer();
            }
            break;
        }
        case 0x08: aiControl_ = v & 1; break;
        case 0x0C: clearMi(MI_AI); break;
        case 0x10: aiDacRate_ = v & 0x3FFF; break;
        case 0x14: aiBitRate_ = v & 0xF; break;
        default: break;
        }
        return;
    }
    if (pa < 0x04700000) {                                           // PI
        const uint32_t r = (pa >> 2) & 15;
        switch (r) {
        case 0: piDram_ = v & 0xFFFFFE; break;
        case 1: piCart_ = v & ~1u; break;
        case 2: piDma(false, v); break;
        case 3: piDma(true, v); break;
        case 4:
            if (v & 2) clearMi(MI_PI);
            if (v & 1) { piBusy_ = false; eventAt_[EV_PI_DMA] = NEVER; }
            break;
        default:
            if (r <= 12) piBsd_[r - 5] = v & ((r == 7 || r == 11) ? 0xF : (r == 8 || r == 12) ? 0x3 : 0xFF);
            break;
        }
        return;
    }
    if (pa < 0x04800000) { ri_[(pa >> 2) & 7] = v; return; }        // RI
    if (pa < 0x04900000) {                                           // SI
        switch (pa & 0x1C) {
        case 0x00: siDram_ = v & 0xFFFFF8; break;
        case 0x04:                                                   // PIF → RDRAM
            joybusRun();
            for (uint32_t i = 0; i < 64; ++i) rdram_[(siDram_ + i) & (RDRAM_SIZE - 1)] = pifRam_[i];
            siBusy_ = true;
            schedule(EV_SI_DMA, SI_DMA_CYCLES);
            break;
        case 0x10:                                                   // RDRAM → PIF
            for (uint32_t i = 0; i < 64; ++i) pifRam_[i] = rdram_[(siDram_ + i) & (RDRAM_SIZE - 1)];
            pifCommand();
            siBusy_ = true;
            schedule(EV_SI_DMA, SI_DMA_CYCLES);
            break;
        case 0x18: clearMi(MI_SI); break;
        default: break;
        }
        return;
    }
    if (pa < 0x08000000) return;
    if (pa < 0x10000000) {                                           // SRAM
        merge(&sram_[(pa - 0x08000000) & (SRAM_SIZE - 4)]);
        save_ = Save::Sram;
        saveDirty_ = true;
        return;
    }
    if (pa < 0x1FC00000) {                                           // ПЗУ: только IS-Viewer
        if (pa < 0x13FF0000 || pa >= 0x13FF0200) return;
        merge(&isvBuf_[pa & 0x1FC]);
        if ((pa & 0x1FC) == 0x14) {                                  // длина строки → вывод
            const uint32_t len = std::min<uint32_t>(get32(&isvBuf_[0x14]), 0x200 - 0x20);
            const std::string text(reinterpret_cast<const char*>(&isvBuf_[0x20]), len);
            static const bool s_isv = std::getenv("EMUDOR_N64_ISV") != nullptr;
            if (s_isv) std::fputs(text.c_str(), stderr);
            isvLog_ += text;
            if (isvLog_.size() > 65536) isvLog_.erase(0, isvLog_.size() - 65536);
            put32(&isvBuf_[0x14], 0);
        }
        return;
    }
    if (pa >= 0x1FC007C0 && pa < 0x1FC00800) {                       // ОЗУ PIF
        merge(&pifRam_[pa & 0x3C]);
        pifCommand();
    }
}

// ─── SP: DMA и статус (сам RSP — позже) ───────────────────────────────────────
// Длина округляется до 8 байт; count строк по length байт, между строками в
// RDRAM пропускается skip байт. Адрес в DMEM/IMEM заворачивается внутри 4 КБ.
void N64System::spDma(bool toRdram, uint32_t reg)
{
    const uint32_t len   = (reg & 0xFFF) | 7;
    const uint32_t count = ((reg >> 12) & 0xFF) + 1;
    const uint32_t skip  = (reg >> 20) & 0xFFF;
    uint32_t mem  = spMemAddr_ & 0x1FF8;
    uint32_t dram = spDramAddr_ & 0xFFFFF8;
    for (uint32_t c = 0; c < count; ++c) {
        for (uint32_t i = 0; i <= len; ++i) {
            const uint32_t m = (mem & 0x1000) | ((mem + i) & 0xFFF);
            const uint32_t d = (dram + i) & 0xFFFFFF;
            if (toRdram) { if (d < RDRAM_SIZE) rdram_[d] = spMem_[m]; }
            else         spMem_[m] = d < RDRAM_SIZE ? rdram_[d] : 0;
        }
        mem  = (mem & 0x1000) | ((mem + len + 1) & 0xFFF);
        dram = (dram + len + 1 + skip) & 0xFFFFF8;
    }
    spMemAddr_ = mem;
    spDramAddr_ = dram;
    (toRdram ? spWrLen_ : spRdLen_) = (skip << 20) | 0xFF8;
}

void N64System::spStatusWrite(uint32_t v)
{
    auto pair = [&](int clr, int set, uint32_t flag) {
        if ((v >> clr & 1) && !(v >> set & 1)) spStatus_ &= ~flag;
        if ((v >> set & 1) && !(v >> clr & 1)) spStatus_ |= flag;
    };
    pair(0, 1, 0x001);                                               // halt
    if (v & 0x4) spStatus_ &= ~0x002u;                               // broke
    if ((v & 0x08) && !(v & 0x10)) clearMi(MI_SP);
    if ((v & 0x10) && !(v & 0x08)) raiseMi(MI_SP);
    pair(5, 6, 0x020);                                               // single step
    pair(7, 8, 0x040);                                               // прерывание по BREAK
    for (int i = 0; i < 8; ++i) pair(9 + 2 * i, 10 + 2 * i, 0x80u << i);   // сигналы 0-7
}

// ─── PI: DMA картриджа ────────────────────────────────────────────────────────
uint8_t N64System::cartRead8(uint32_t a) const
{
    if (a >= 0x10000000 && a < 0x1FC00000) {
        const uint32_t off = a - 0x10000000;
        if (off < rom_.size()) return rom_[off];
        return (uint8_t)((a & 2) ? a : a >> 8);                      // открытая шина
    }
    if (a >= 0x08000000 && a < 0x10000000) return sram_[(a - 0x08000000) & (SRAM_SIZE - 1)];
    return 0;
}

void N64System::cartWrite8(uint32_t a, uint8_t v)
{
    if (a >= 0x08000000 && a < 0x10000000) {
        sram_[(a - 0x08000000) & (SRAM_SIZE - 1)] = v;
        save_ = Save::Sram;
        saveDirty_ = true;
    }
}

// ponytail: скорость DMA условная (≈8 байт за такт) и без причуд невыровненных
// длин; точная модель — по таймингам BSD_DOM, если игры на это наткнутся.
void N64System::piDma(bool toRdram, uint32_t lenReg)
{
    const uint32_t len = (lenReg & 0x00FFFFFF) + 1;
    const uint32_t dram = piDram_ & 0xFFFFFE;
    const uint32_t cart = piCart_ & ~1u;
    for (uint32_t i = 0; i < len; ++i) {
        const uint32_t d = dram + i;
        if (toRdram) { if (d < RDRAM_SIZE) rdram_[d] = cartRead8(cart + i); }
        else         cartWrite8(cart + i, d < RDRAM_SIZE ? rdram_[d] : 0);
    }
    piDram_ = (dram + len + 7) & ~7u;
    piCart_ = (cart + len + 1) & ~1u;
    piBusy_ = true;
    schedule(EV_PI_DMA, len / 8 + 64);
}

// ─── AI: очередь из двух буферов, звук — по мере начала буфера ────────────────
void N64System::aiStartBuffer()
{
    const AiBuffer& b = aiFifo_[0];
    const double freq = viClock(tv_) / (double)(aiDacRate_ + 1);
    const uint32_t frames = b.len / 4;                               // стерео по 16 бит
    aiDuration_ = (uint64_t)(frames * (double)CPU_HZ / freq);
    aiStart_ = cpu.cycles();
    schedule(EV_AI_BUF, aiDuration_);
    raiseMi(MI_AI);                                                  // в очереди освободилось место

    // Ресэмплинг в 44100 Гц (линейный), каналы смешиваются в моно.
    const double step = freq / SAMPLE_RATE;
    auto sampleAt = [&](uint32_t i) -> float {
        const uint32_t a = (b.addr + i * 4) & (RDRAM_SIZE - 4);
        const int16_t l = (int16_t)((rdram_[a] << 8) | rdram_[a + 1]);
        const int16_t r = (int16_t)((rdram_[a + 2] << 8) | rdram_[a + 3]);
        return (l + r) / 65536.0f;
    };
    double pos = aiResample_;
    while (pos < frames) {
        const uint32_t i = (uint32_t)pos;
        const float frac = (float)(pos - i);
        const float s0 = sampleAt(i);
        const float s1 = i + 1 < frames ? sampleAt(i + 1) : s0;
        samples_.push_back(s0 + (s1 - s0) * frac);
        pos += step;
    }
    aiResample_ = pos - frames;
}

// ─── VI ───────────────────────────────────────────────────────────────────────
uint32_t N64System::viLinesPerField() const
{
    uint32_t vsync = vi_[VI_V_SYNC] & 0x3FF;
    if (vsync < 100) vsync = tv_ == Tv::PAL ? 625 : 525;
    return (vsync + 1) / 2;
}

uint32_t N64System::viCyclesPerLine() const
{
    uint32_t hsync = vi_[VI_H_SYNC] & 0xFFF;
    if (hsync < 1000) hsync = tv_ == Tv::PAL ? 3177 : 3093;
    return (uint32_t)((hsync + 1) * (double)CPU_HZ / viClock(tv_));
}

void N64System::viLine()
{
    if (++viLine_ >= viLinesPerField()) {
        viLine_ = 0;
        viField_ = (vi_[VI_CTRL] & 0x40) ? viField_ ^ 1 : 0;
        renderFrame();
        ++frames_;
        frameDone_ = true;
    }
    if ((viLine_ << 1) == (vi_[VI_V_INTR] & 0x3FE)) raiseMi(MI_VI);
    schedule(EV_VI_LINE, viCyclesPerLine());
}

// Кадр — область буфера, которую VI показывает: ширина и высота считаются из
// H/V_VIDEO и масштабов, как у настоящего VI; фильтры VI не эмулируются.
void N64System::renderFrame()
{
    const uint32_t type  = vi_[VI_CTRL] & 3;
    const uint32_t hs = (vi_[VI_H_VIDEO] >> 16) & 0x3FF, he = vi_[VI_H_VIDEO] & 0x3FF;
    const uint32_t vs = (vi_[VI_V_VIDEO] >> 16) & 0x3FF, ve = vi_[VI_V_VIDEO] & 0x3FF;
    const uint32_t xs = vi_[VI_X_SCALE] & 0xFFF, ys = vi_[VI_Y_SCALE] & 0xFFF;
    const uint32_t stride = vi_[VI_WIDTH] & 0xFFF;
    const int w = he > hs ? (int)(((he - hs) * xs) >> 10) : 0;
    const int h = ve > vs ? (int)((((ve - vs) >> 1) * ys) >> 10) : 0;

    if (type < 2 || w <= 0 || h <= 0 || stride == 0) {
        std::fill(frame_.begin(), frame_.end(), 0xFF000000u);
        return;
    }
    frameW_ = std::min(w, 1024);
    frameH_ = std::min(h, 1024);
    frame_.resize((size_t)frameW_ * frameH_);

    const uint32_t origin = vi_[VI_ORIGIN] & 0xFFFFFF;
    const uint32_t bpp = type == 3 ? 4 : 2;
    for (int y = 0; y < frameH_; ++y) {
        uint32_t* out = &frame_[(size_t)y * frameW_];
        for (int x = 0; x < frameW_; ++x) {
            const uint32_t a = (origin + ((uint32_t)y * stride + (uint32_t)x) * bpp) & (RDRAM_SIZE - 4);
            uint32_t r, g, b;
            if (bpp == 4) {
                r = rdram_[a]; g = rdram_[a + 1]; b = rdram_[a + 2];
            } else {
                const uint32_t p = (rdram_[a] << 8) | rdram_[a + 1];
                r = (p >> 11) & 31; g = (p >> 6) & 31; b = (p >> 1) & 31;
                r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2);
            }
            out[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
}

// ─── PIF: команды, джойбас, CIC ───────────────────────────────────────────────
void N64System::pifCommand()
{
    uint8_t& cmd = pifRam_[63];
    joybusArmed_ = cmd & 0x01;                    // команды джойбаса выполнятся при чтении
    cmd &= (uint8_t)~0x01;
    if (cmd & 0x02) { cicChallenge(); cmd &= (uint8_t)~0x02; }
    if (cmd & 0x08) cmd &= (uint8_t)~0x08;         // конец загрузки
    if (cmd & 0x10) cmd &= (uint8_t)~0x10;         // закрыть ПЗУ PIF
    if (cmd & 0x20) cmd = (uint8_t)((cmd & ~0x20) | 0x80);   // контрольная сумма «проверена»
    if (cmd & 0x40) pifRam_.fill(0);
}

void N64System::setController(int port, uint16_t buttons, int8_t x, int8_t y)
{
    if (port < 0 || port > 3) return;
    pads_[port].buttons = buttons;
    pads_[port].x = x;
    pads_[port].y = y;
}

// Блок команд: [tx][rx][tx байт команды][rx байт ответа] на каждый канал;
// 0x00 — пропустить канал, 0xFF/0xFD — заполнитель, 0xFE — конец.
void N64System::joybusRun()
{
    if (!joybusArmed_) return;
    int channel = 0;
    int i = 0;
    while (i < 63 && channel < 6) {
        const uint8_t txb = pifRam_[i];
        if (txb == 0xFE) break;
        if (txb == 0xFF || txb == 0xFD) { ++i; continue; }
        if (txb == 0x00) { ++i; ++channel; continue; }
        const uint8_t rxb = pifRam_[i + 1];
        if (rxb == 0xFE) break;
        const int txLen = txb & 0x3F, rxLen = rxb & 0x3F;
        const int txPos = i + 2, rxPos = txPos + txLen;
        if (rxPos + rxLen > 63) break;
        if (joybusDevice(channel, &pifRam_[txPos], txLen, &pifRam_[rxPos], rxLen))
            pifRam_[i + 1] = (uint8_t)rxLen;
        else
            pifRam_[i + 1] = (uint8_t)(rxLen | 0x80);                // устройства нет
        i = rxPos + rxLen;
        ++channel;
    }
}

bool N64System::joybusDevice(int channel, const uint8_t* tx, int txLen, uint8_t* rx, int rxLen)
{
    if (txLen < 1) return false;
    const uint8_t cmd = tx[0];
    if (channel < 4) {                                               // джойстики
        const Controller& c = pads_[channel];
        if (!c.present) return false;
        switch (cmd) {
        case 0x00: case 0xFF:                                        // тип: стандартный джойстик, без модуля
            if (rxLen >= 3) { rx[0] = 0x05; rx[1] = 0x00; rx[2] = 0x02; }
            return true;
        case 0x01:                                                   // кнопки и стик
            if (rxLen >= 4) {
                rx[0] = (uint8_t)(c.buttons >> 8);
                rx[1] = (uint8_t)c.buttons;
                rx[2] = (uint8_t)c.x;
                rx[3] = (uint8_t)c.y;
            }
            return true;
        default: return false;                                       // модуля памяти нет
        }
    }
    if (channel == 4 && save_ != Save::Sram) {                       // EEPROM картриджа
        switch (cmd) {
        case 0x00: case 0xFF:
            if (rxLen >= 3) { rx[0] = 0x00; rx[1] = eeprom_.size() > 512 ? 0xC0 : 0x80; rx[2] = 0x00; }
            return true;
        case 0x04: {                                                 // чтение блока 8 байт
            if (txLen < 2) return false;
            const size_t off = (size_t)tx[1] * 8;
            for (int k = 0; k < 8 && k < rxLen; ++k)
                rx[k] = off + k < eeprom_.size() ? eeprom_[off + k] : 0;
            return true;
        }
        case 0x05: {                                                 // запись блока
            if (txLen < 10) return false;
            const size_t off = (size_t)tx[1] * 8;
            for (int k = 0; k < 8; ++k)
                if (off + k < eeprom_.size()) eeprom_[off + k] = tx[2 + k];
            if (rxLen >= 1) rx[0] = 0x00;
            saveDirty_ = true;
            return true;
        }
        default: return false;
        }
    }
    return false;
}

// Ответ CIC 6105 на запрос IPL3 (алгоритм X-Scale): 30 полубайт запроса в
// байтах 48-62, ответ пишется на то же место.
void N64System::cicChallenge()
{
    static const uint8_t lut0[16] = { 0x4, 0x7, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1,
                                      0xC, 0xF, 0x8, 0xF, 0x6, 0x3, 0x6, 0x9 };
    static const uint8_t lut1[16] = { 0x4, 0x1, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1,
                                      0xC, 0x9, 0x8, 0x5, 0x6, 0x3, 0xC, 0x9 };
    uint8_t chl[30], rsp[30];
    for (int i = 0; i < 15; ++i) {
        chl[i * 2]     = pifRam_[48 + i] >> 4;
        chl[i * 2 + 1] = pifRam_[48 + i] & 0xF;
    }
    uint8_t key = 0xB;
    const uint8_t* lut = lut0;
    for (int i = 0; i < 30; ++i) {
        rsp[i] = (uint8_t)((key + 5 * chl[i]) & 0xF);
        key = lut[rsp[i]];
        const int sgn = (rsp[i] >> 3) & 1;
        const int mag = ((sgn == 1) ? ~rsp[i] : rsp[i]) & 7;
        int mod = (mag % 3 == 1) ? sgn : 1 - sgn;
        if (lut == lut1 && (rsp[i] == 0x1 || rsp[i] == 0x9)) mod = 1;
        if (lut == lut1 && (rsp[i] == 0xB || rsp[i] == 0xE)) mod = 0;
        lut = (mod == 1) ? lut1 : lut0;
    }
    pifRam_[46] = pifRam_[47] = 0;
    for (int i = 0; i < 15; ++i) pifRam_[48 + i] = (uint8_t)((rsp[i * 2] << 4) | rsp[i * 2 + 1]);
}
