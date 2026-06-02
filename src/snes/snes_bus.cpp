// snes_bus.cpp — реализация шины памяти SNES
#include <cstdlib>
// 24-битное адресное пространство, поддержка LoROM / HiROM / ExHiROM.
#include "snes_bus.h"
#include "snes_ppu.h"
#include "snes_apu.h"

#include <fstream>
#include <cstring>
#include <algorithm>
#include <cassert>

// ─── Вспомогательные константы ───────────────────────────────────────────────
// Смещения заголовка ROM внутри банка (без учёта базы банка)
static constexpr uint32_t LOROM_HEADER = 0x7FC0;  // банк $00 → абс. $007FC0
static constexpr uint32_t HIROM_HEADER = 0xFFC0;  // банк $00 → абс. $00FFC0

// ─── Загрузка ROM ─────────────────────────────────────────────────────────────
bool SnesBus::loadROM(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;

    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0);

    std::vector<uint8_t> raw(size);
    f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(size));
    if (!f) return false;

    // Убираем SMC-заголовок (512 байт), если он есть (размер файла mod 1024 == 512)
    if ((size & 0x3FF) == 0x200) {
        raw.erase(raw.begin(), raw.begin() + 0x200);
    }

    // Определяем тип маппинга
    MapMode mode = detectMapMode(raw);
    if (mode == MapMode::Unknown) return false;

    // Читаем информацию о SRAM из заголовка
    uint32_t hdrBase = (mode == MapMode::LoROM) ? LOROM_HEADER : HIROM_HEADER;
    uint8_t  romType  = raw[hdrBase + 0x16];   // ROM type byte
    uint8_t  sramSize = raw[hdrBase + 0x18];   // SRAM size (1 << n KB), 0 = нет

    bool battery = (romType & 0x02) != 0;      // бит 1 = battery

    // Размер SRAM
    uint32_t sramBytes = sramSize ? (1u << sramSize) * 1024u : 0u;
    sramBytes = std::min(sramBytes, (uint32_t)0x20000);  // макс. 128 KB

    loadROMDirect(std::move(raw), mode, battery);

    // Инициализируем SRAM нулями (или оставляем пустой)
    if (sramBytes > 0) {
        sram_.assign(sramBytes, 0x00);
    }

    return true;
}

// Прямая инъекция ROM (для тестов)
void SnesBus::loadROMDirect(std::vector<uint8_t> data, MapMode mode, bool battery)
{
    rom_        = std::move(data);
    mapMode_    = mode;
    hasBattery_ = battery;
    sramDirty_  = false;
    reset();
}

// ─── Сброс ────────────────────────────────────────────────────────────────────
void SnesBus::reset()
{
    wram_.fill(0);
    ctrlStrobe_ = 0;
    ctrlShift_[0] = ctrlShift_[1] = 0;
    openBus_ = 0;
    mdmaen_       = 0;
    hdmaen_       = 0;
    hdmaInit_     = false;
    memsel_       = 0;
    nmitimen_     = 0;
    nmiFlag_      = false;
    vblankActive_ = false;
    wramPort_     = 0;
    hTarget_      = 0x1FF;
    vTarget_      = 0x1FF;
    irqPending_   = false;
    for (auto& d : dma_) d = DmaChannel{};
}

// ─── Детект типа маппинга ─────────────────────────────────────────────────────
// Проверяем контрольную сумму в заголовке, чтобы определить LoROM/HiROM.
bool SnesBus::verifyHeader(const std::vector<uint8_t>& rom, uint32_t base)
{
    if (base + 0x30 > rom.size()) return false;

    // Поле «Complement Check» ($FFDCh) + «Checksum» ($FFDEh)
    uint16_t complement = (uint16_t)(rom[base + 0x1C] | (rom[base + 0x1D] << 8));
    uint16_t checksum   = (uint16_t)(rom[base + 0x1E] | (rom[base + 0x1F] << 8));

    if ((complement ^ checksum) != 0xFFFF) return false;

    // Байт типа маппинга (0x15)
    uint8_t mapByte = rom[base + 0x15];
    (void)mapByte;  // дополнительная проверка при желании

    return true;
}

SnesBus::MapMode SnesBus::detectMapMode(const std::vector<uint8_t>& rom)
{
    bool loOk = verifyHeader(rom, LOROM_HEADER);
    bool hiOk = verifyHeader(rom, HIROM_HEADER);

    if (loOk && !hiOk) return MapMode::LoROM;
    if (hiOk && !loOk) return MapMode::HiROM;

    // Оба совпали или оба нет — смотрим на байт $7FD5/$FFD5
    if (rom.size() > LOROM_HEADER + 0x15) {
        uint8_t lo = rom[LOROM_HEADER + 0x15];
        uint8_t hi = (rom.size() > HIROM_HEADER + 0x15) ? rom[HIROM_HEADER + 0x15] : 0xFF;

        if ((lo & 0xEF) == 0x20) return MapMode::LoROM;
        if ((hi & 0xEF) == 0x21) return MapMode::HiROM;
        if ((hi & 0xEF) == 0x25) return MapMode::ExHiROM;
    }

    // Запасной вариант: ROM ≤ 2 MB → LoROM, иначе HiROM
    return (rom.size() <= 0x200000) ? MapMode::LoROM : MapMode::HiROM;
}

// ─── Главный диспетчер чтения ─────────────────────────────────────────────────
uint8_t SnesBus::read(uint32_t addr)
{
    uint8_t  bank = (uint8_t)(addr >> 16);
    uint16_t off  = (uint16_t)(addr & 0xFFFF);

    // ── WRAM $7E/$7F ──────────────────────────────────────────────────────────
    if (bank == 0x7E || bank == 0x7F) {
        uint32_t waddr = (uint32_t)((bank & 1) << 16) | off;
        return wram_[waddr];
    }

    // ── Системные регистры I/O ($2000–$5FFF в банках $00–$3F/$80–$BF) ─────────
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && off >= 0x2000 && off <= 0x5FFF) {
        return readIO(off);
    }

    // ── Зеркало WRAM $0000–$1FFF в банках $00–$3F/$80–$BF ────────────────────
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && off < 0x2000) {
        return wram_[off];
    }

    // ── ROM / SRAM — зависит от типа маппинга ────────────────────────────────
    switch (mapMode_) {
        case MapMode::LoROM:  return readLoROM(bank, off);
        case MapMode::HiROM:  return readHiROM(bank, off);
        case MapMode::ExHiROM: return readExHiROM(bank, off);
        default: break;
    }

    return openBus_;
}

// ─── Главный диспетчер записи ─────────────────────────────────────────────────
void SnesBus::write(uint32_t addr, uint8_t data)
{
    uint8_t  bank = (uint8_t)(addr >> 16);
    uint16_t off  = (uint16_t)(addr & 0xFFFF);

    openBus_ = data;

    // ── WRAM $7E/$7F ──────────────────────────────────────────────────────────
    if (bank == 0x7E || bank == 0x7F) {
        uint32_t waddr = (uint32_t)((bank & 1) << 16) | off;
        wram_[waddr] = data;
        return;
    }

    // ── Системные регистры I/O ────────────────────────────────────────────────
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && off >= 0x2000 && off <= 0x5FFF) {
        writeIO(off, data);
        return;
    }

    // ── Зеркало WRAM $0000–$1FFF ──────────────────────────────────────────────
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && off < 0x2000) {
        wram_[off] = data;
        return;
    }

    // ── SRAM / ROM ────────────────────────────────────────────────────────────
    switch (mapMode_) {
        case MapMode::LoROM:   writeLoROM(bank, off, data);  break;
        case MapMode::HiROM:   writeHiROM(bank, off, data);  break;
        case MapMode::ExHiROM: writeExHiROM(bank, off, data); break;
        default: break;
    }
}

// ─── LoROM ────────────────────────────────────────────────────────────────────
// Банки $00–$7D, $80–$FF; ROM в $8000–$FFFF каждого банка.
// Смещение в ROM = (bank & 0x7F) * 0x8000 + (addr - 0x8000)
uint8_t SnesBus::readLoROM(uint8_t bank, uint16_t addr)
{
    // SRAM: банки $70–$7D, $F0–$FF; addr $0000–$7FFF
    if ((bank >= 0x70 && bank <= 0x7D) || (bank >= 0xF0)) {
        if (addr < 0x8000 && !sram_.empty()) {
            uint32_t soff = (uint32_t)((bank & 0x0F) * 0x8000 + addr);
            return sram_[soff % sram_.size()];
        }
    }

    if (addr < 0x8000) return openBus_;
    if (rom_.empty()) return openBus_;

    // LoROM: банки $00-$7F (и зеркала $80-$FF), каждый занимает 32 KB ROM.
    // Малые ROM (< 2 MB) зеркалятся — берём остаток от размера.
    uint32_t romOff = (uint32_t)((bank & 0x7F) * 0x8000u + (addr - 0x8000u));
    romOff %= (uint32_t)rom_.size();
    return rom_[romOff];
}

void SnesBus::writeLoROM(uint8_t bank, uint16_t addr, uint8_t data)
{
    if ((bank >= 0x70 && bank <= 0x7D) || (bank >= 0xF0)) {
        if (addr < 0x8000 && !sram_.empty()) {
            uint32_t soff = (uint32_t)((bank & 0x0F) * 0x8000 + addr);
            sram_[soff % sram_.size()] = data;
            sramDirty_ = true;
        }
    }
    // ROM не записывается
}

// ─── HiROM ────────────────────────────────────────────────────────────────────
// Банки $C0–$FF: ROM полный 64 KB.
// Банки $00–$3F/$80–$BF: ROM в $8000–$FFFF.
uint8_t SnesBus::readHiROM(uint8_t bank, uint16_t addr)
{
    // SRAM: банки $20–$3F, $A0–$BF; addr $6000–$7FFF
    if (((bank >= 0x20 && bank <= 0x3F) || (bank >= 0xA0 && bank <= 0xBF))
        && addr >= 0x6000 && addr <= 0x7FFF)
    {
        if (!sram_.empty()) {
            uint32_t soff = (uint32_t)((bank & 0x1F) * 0x2000u + (addr - 0x6000u));
            return sram_[soff % sram_.size()];
        }
        return openBus_;
    }

    if (rom_.empty()) return openBus_;

    // ROM в банках $C0–$FF (весь диапазон $0000–$FFFF)
    // Зеркалируется для ROM < 4 MB
    if (bank >= 0xC0) {
        uint32_t romOff = (uint32_t)((bank - 0xC0) * 0x10000u + addr);
        romOff %= (uint32_t)rom_.size();
        return rom_[romOff];
    }

    // ROM в банках $00–$3F/$80–$BF, $8000–$FFFF
    if (addr >= 0x8000) {
        uint32_t romOff = (uint32_t)((bank & 0x3F) * 0x10000u + addr);
        romOff %= (uint32_t)rom_.size();
        return rom_[romOff];
    }

    return openBus_;
}

void SnesBus::writeHiROM(uint8_t bank, uint16_t addr, uint8_t data)
{
    if (((bank >= 0x20 && bank <= 0x3F) || (bank >= 0xA0 && bank <= 0xBF))
        && addr >= 0x6000 && addr <= 0x7FFF)
    {
        if (!sram_.empty()) {
            uint32_t soff = (uint32_t)((bank & 0x1F) * 0x2000u + (addr - 0x6000u));
            sram_[soff % sram_.size()] = data;
            sramDirty_ = true;
        }
    }
}

// ─── ExHiROM ─────────────────────────────────────────────────────────────────
// Расширенный HiROM (до 64 MB). Банки $00–$3F/$80–$BF/$C0–$FF.
uint8_t SnesBus::readExHiROM(uint8_t bank, uint16_t addr)
{
    // SRAM: банки $20–$3F, addr $6000–$7FFF
    if (bank >= 0x20 && bank <= 0x3F && addr >= 0x6000 && addr <= 0x7FFF) {
        if (!sram_.empty()) {
            uint32_t soff = (uint32_t)((bank - 0x20) * 0x2000u + (addr - 0x6000u));
            return sram_[soff % sram_.size()];
        }
        return openBus_;
    }

    // ROM: банки $C0–$FF → первые 256×64KB
    if (bank >= 0xC0) {
        uint32_t romOff = (uint32_t)((bank - 0xC0) * 0x10000u + addr);
        if (romOff >= rom_.size()) return openBus_;
        return rom_[romOff];
    }

    // ROM: банки $80–$BF → следующие 64 банка × $8000–$FFFF
    if (bank >= 0x80 && bank <= 0xBF && addr >= 0x8000) {
        uint32_t romOff = (uint32_t)(0x400000u + (bank - 0x80) * 0x8000u + (addr - 0x8000u));
        if (romOff >= rom_.size()) return openBus_;
        return rom_[romOff];
    }

    // ROM: банки $00–$3F → addr $8000–$FFFF
    if (bank <= 0x3F && addr >= 0x8000) {
        uint32_t romOff = (uint32_t)(0x600000u + bank * 0x8000u + (addr - 0x8000u));
        if (romOff >= rom_.size()) return openBus_;
        return rom_[romOff];
    }

    return openBus_;
}

void SnesBus::writeExHiROM(uint8_t bank, uint16_t addr, uint8_t data)
{
    if (bank >= 0x20 && bank <= 0x3F && addr >= 0x6000 && addr <= 0x7FFF) {
        if (!sram_.empty()) {
            uint32_t soff = (uint32_t)((bank - 0x20) * 0x2000u + (addr - 0x6000u));
            sram_[soff % sram_.size()] = data;
            sramDirty_ = true;
        }
    }
}

// ─── Регистры ввода-вывода ────────────────────────────────────────────────────
uint8_t SnesBus::readIO(uint16_t addr)
{
    // WRAM data port ($2180): читаем байт и автоинкремент адреса
    if (addr == 0x2180) {
        uint8_t v = wram_[wramPort_ & (WRAM_SIZE - 1)];
        wramPort_ = (wramPort_ + 1) & (WRAM_SIZE - 1);
        return v;
    }

    // PPU-регистры: $2134–$213F, читаемые $2138–$213F
    if (ppu_ && addr >= 0x2134 && addr <= 0x213F) {
        return ppu_->readReg(addr);
    }

    // APU: $2140–$2143 (SPC700 communication ports)
    if (apu_ && addr >= 0x2140 && addr <= 0x2143) {
        return apu_->readPort((uint8_t)(addr - 0x2140));
    }

    // Контроллеры $4016/$4017 (16 бит для SNES-геймпада)
    if (addr == 0x4016) {
        uint8_t bit = (uint8_t)(ctrlShift_[0] & 1);
        ctrlShift_[0] = (uint16_t)(ctrlShift_[0] >> 1);
        return (uint8_t)((openBus_ & 0xFC) | bit);
    }
    if (addr == 0x4017) {
        uint8_t bit = (uint8_t)(ctrlShift_[1] & 1);
        ctrlShift_[1] = (uint16_t)(ctrlShift_[1] >> 1);
        return (uint8_t)((openBus_ & 0xFC) | bit);
    }

    // ── CPU I/O регистры ─────────────────────────────────────────────────────
    // $4200 NMITIMEN read-back
    if (addr == 0x4200) return nmitimen_;
    // $4210 RDNMI: NMI-флаг (bit7) + версия CPU (0x02); чтение сбрасывает флаг
    if (addr == 0x4210) {
        uint8_t v = (uint8_t)((nmiFlag_ ? 0x80 : 0) | 0x02);
        nmiFlag_ = false;
        return v;
    }
    // $4211 TIMEUP: IRQ-флаг H/V-таймера; чтение возвращает и сбрасывает
    if (addr == 0x4211) {
        uint8_t v = (uint8_t)(irqPending_ ? 0x80 : 0x00);
        irqPending_ = false;
        return v;
    }
    // $4212 HVBJOY: VBlank (bit7) + HBlank (bit6) + joypad busy (bit0)
    if (addr == 0x4212) {
        // HVBJOY: bit7=VBlank, bit6=HBlank, bit0=auto-joypad busy.
        // Многие игры (SMW и др.) опрашивают bit6 в tight-loop для синхронизации
        // по строкам — без него CPU зависает навсегда.
        uint8_t v = 0;
        if (ppu_) {
            if (ppu_->curScanline() >= 225) v |= 0x80;          // VBlank (строки 225-261)
            uint16_t d = ppu_->curDot();
            if (d >= 274 || d < 2)          v |= 0x40;          // HBlank (≈ dot 274-340 + 0-1)
        } else {
            v = vblankActive_ ? 0x80 : 0x00;
        }
        return v;   // bit0 auto-joypad: у нас мгновенный → 0
    }
    // $4213 I/O port input
    if (addr == 0x4213) return 0xFF;
    // $4214–$4217: результаты деления/умножения
    if (addr == 0x4214) return (uint8_t)(rddiv_ & 0xFF);
    if (addr == 0x4215) return (uint8_t)(rddiv_ >> 8);
    if (addr == 0x4216) return (uint8_t)(rdmpy_ & 0xFF);
    if (addr == 0x4217) return (uint8_t)(rdmpy_ >> 8);
    // $4218–$421B: авто-опрос джойпада (результат latchAutoJoy)
    if (addr == 0x4218) return (uint8_t)(autoJoy_[0] & 0xFF);
    if (addr == 0x4219) return (uint8_t)(autoJoy_[0] >> 8);
    if (addr == 0x421A) return (uint8_t)(autoJoy_[1] & 0xFF);
    if (addr == 0x421B) return (uint8_t)(autoJoy_[1] >> 8);
    if (addr >= 0x421C && addr <= 0x421F) return 0;

    // $420D MEMSEL — readback (bit 0 = последнее записанное значение)
    if (addr == 0x420D) return memsel_;

    // DMA-регистры (read-back)
    if (addr >= 0x4300 && addr <= 0x437F) {
        int ch  = (addr - 0x4300) >> 4;
        int reg = (addr - 0x4300) & 0x0F;
        switch (reg) {
        case 0x0: return dma_[ch].dmap;
        case 0x1: return dma_[ch].bbad;
        case 0x2: return (uint8_t)(dma_[ch].a1t);
        case 0x3: return (uint8_t)(dma_[ch].a1t >> 8);
        case 0x4: return dma_[ch].a1b;
        case 0x5: return (uint8_t)(dma_[ch].das);
        case 0x6: return (uint8_t)(dma_[ch].das >> 8);
        case 0x7: return dma_[ch].dasb;
        case 0x8: return (uint8_t)(dma_[ch].a2a);
        case 0x9: return (uint8_t)(dma_[ch].a2a >> 8);
        case 0xA: return dma_[ch].ntrl;
        default:  return openBus_;
        }
    }
    if (addr == 0x420B) return mdmaen_;
    if (addr == 0x420C) return hdmaen_;

    return openBus_;
}

void SnesBus::writeIO(uint16_t addr, uint8_t data)
{
    // WRAM data port ($2180–$2183)
    if (addr == 0x2180) {
        wram_[wramPort_ & (WRAM_SIZE - 1)] = data;
        wramPort_ = (wramPort_ + 1) & (WRAM_SIZE - 1);
        return;
    }
    if (addr == 0x2181) {
        wramPort_ = (wramPort_ & 0x1FF00u) | data;
        return;
    }
    if (addr == 0x2182) {
        wramPort_ = (wramPort_ & 0x100FFu) | ((uint32_t)data << 8);
        return;
    }
    if (addr == 0x2183) {
        wramPort_ = (wramPort_ & 0x0FFFFu) | ((uint32_t)(data & 1) << 16);
        return;
    }

    // PPU-регистры: $2100–$2133
    if (ppu_ && addr >= 0x2100 && addr <= 0x2133) {
        ppu_->writeReg(addr, data);
        return;
    }

    // APU: $2140–$2143
    if (apu_ && addr >= 0x2140 && addr <= 0x2143) {
        apu_->writePort((uint8_t)(addr - 0x2140), data);
        return;
    }

    // Строб контроллера $4016 (16-бит: кнопки B,Y,Sel,Start,Up,Down,Left,Right,A,X,L,R,0,0,0,0)
    if (addr == 0x4016) {
        uint8_t old = ctrlStrobe_;
        ctrlStrobe_ = data & 1;
        if (old && !ctrlStrobe_) {
            // Загружаем 16-битный сдвиговый регистр (MSB первым: бит15 выходит первым)
            ctrlShift_[0] = controller[0];
            ctrlShift_[1] = controller[1];
        }
        return;
    }

    // ─── DMA-регистры ────────────────────────────────────────────────────────
    // $4300–$437F: 8 каналов × 16 байт
    if (addr >= 0x4300 && addr <= 0x437F) {
        int ch  = (addr - 0x4300) >> 4;
        int reg = (addr - 0x4300) & 0x0F;
        switch (reg) {
        case 0x0: dma_[ch].dmap  = data; break;
        case 0x1: dma_[ch].bbad  = data; break;
        case 0x2: dma_[ch].a1t   = (uint16_t)((dma_[ch].a1t  & 0xFF00) | data); break;
        case 0x3: dma_[ch].a1t   = (uint16_t)((dma_[ch].a1t  & 0x00FF) | (data << 8)); break;
        case 0x4: dma_[ch].a1b   = data; break;
        case 0x5: dma_[ch].das   = (uint16_t)((dma_[ch].das  & 0xFF00) | data); break;
        case 0x6: dma_[ch].das   = (uint16_t)((dma_[ch].das  & 0x00FF) | (data << 8)); break;
        case 0x7: dma_[ch].dasb  = data; break;
        case 0x8: dma_[ch].a2a   = (uint16_t)((dma_[ch].a2a  & 0xFF00) | data); break;
        case 0x9: dma_[ch].a2a   = (uint16_t)((dma_[ch].a2a  & 0x00FF) | (data << 8)); break;
        case 0xA: dma_[ch].ntrl  = data; break;
        default: break;
        }
        return;
    }

    // ── CPU I/O регистры ─────────────────────────────────────────────────────
    // $4200 NMITIMEN: NMI/IRQ/joypad enable
    if (addr == 0x4200) { nmitimen_ = data; return; }
    // $4201 I/O port direction — игнорируем
    if (addr == 0x4201) return;
    // $4202 WRMPYA: множитель A
    if (addr == 0x4202) { wrmpya_ = data; return; }
    // $4203 WRMPYB: множитель B, запуск умножения
    if (addr == 0x4203) { rdmpy_ = (uint16_t)((uint16_t)wrmpya_ * data); rddiv_ = 0; return; }
    // $4204/$4205 WRDIVL/WRDIVH: делимое
    if (addr == 0x4204) { wrdiv_ = (uint16_t)((wrdiv_ & 0xFF00) | data); return; }
    if (addr == 0x4205) { wrdiv_ = (uint16_t)((wrdiv_ & 0x00FF) | ((uint16_t)data << 8)); return; }
    // $4206 WRDIVB: делитель, запуск деления
    if (addr == 0x4206) {
        if (data == 0) { rddiv_ = 0xFFFF; rdmpy_ = wrdiv_; }
        else           { rddiv_ = (uint16_t)(wrdiv_ / data); rdmpy_ = (uint16_t)(wrdiv_ % data); }
        return;
    }
    // $4207/$4208 HTIME (9 бит) и $4209/$420A VTIME (9 бит)
    if (addr == 0x4207) { hTarget_ = (uint16_t)((hTarget_ & 0x0100) | data); return; }
    if (addr == 0x4208) { hTarget_ = (uint16_t)((hTarget_ & 0x00FF) | ((uint16_t)(data & 1) << 8)); return; }
    if (addr == 0x4209) { vTarget_ = (uint16_t)((vTarget_ & 0x0100) | data); return; }
    if (addr == 0x420A) { vTarget_ = (uint16_t)((vTarget_ & 0x00FF) | ((uint16_t)(data & 1) << 8)); return; }
    // $420D MEMSEL — скорость ROM. Сохраняем для readback (тайминги не эмулируем —
    // игры всё равно работают, но некоторые читают регистр для само-проверки).
    if (addr == 0x420D) { memsel_ = (uint8_t)(data & 1); return; }

    // $420B: MDMAEN — запуск GPDMA
    if (addr == 0x420B) {
        runGDMA(data);
        return;
    }
    // $420C: HDMAEN — инициализация HDMA
    if (addr == 0x420C) {
        hdmaen_  = data;
        hdmaInit_= (data != 0);
        return;
    }
}

// ─── GPDMA ────────────────────────────────────────────────────────────────────
// Выполняет DMA-передачи для всех указанных каналов немедленно.
void SnesBus::runGDMA(uint8_t channels)
{
    for (int ch = 0; ch < 8; ++ch) {
        if (channels & (1 << ch)) execGDMACh(ch);
    }
}

// Режимы передачи DMA:
//   0: 1 байт  → 1 регистр
//   1: 2 байта → 2 регистра (B+0, B+1)
//   2: 2 байта → 1 регистр (дважды)
//   3: 4 байта → 2 регистра (по 2)
//   7: 4 байта → 4 регистра
static const int kDmaPatternLen[] = {1, 2, 2, 4, 4, 4, 2, 4};
static const int kDmaPattern[][4] = {
    {0, 0, 0, 0},  // mode 0
    {0, 1, 0, 0},  // mode 1
    {0, 0, 0, 0},  // mode 2 (same reg twice)
    {0, 0, 1, 1},  // mode 3
    {0, 1, 2, 3},  // mode 4
    {0, 1, 0, 1},  // mode 5
    {0, 0, 0, 0},  // mode 6 (= mode 2)
    {0, 1, 2, 3},  // mode 7
};

void SnesBus::execGDMACh(int ch)
{
    DmaChannel& d = dma_[ch];
    bool    toB   = !(d.dmap & 0x80);   // true = A→B (CPU→PPU), false = B→A
    uint8_t mode  = d.dmap & 0x07;
    int     plen  = kDmaPatternLen[mode];

    uint32_t aAddr  = (uint32_t)((d.a1b << 16) | d.a1t);
    uint32_t count  = (d.das == 0) ? 0x10000u : (uint32_t)d.das;  // 0 → 65536

    int step = 0;
    while (count > 0) {
        int regOff = kDmaPattern[mode][step % plen];
        uint16_t bAddr = (uint16_t)(0x2100 + d.bbad + regOff);

        if (toB) {
            uint8_t val = read(aAddr);
            writeIO(bAddr, val);
        } else {
            uint8_t val = readIO(bAddr);
            write(aAddr, val);
        }

        // A-шина: инкремент, декремент или фиксированная
        uint8_t aStep = (d.dmap >> 3) & 3;
        if      (aStep == 0) ++aAddr;
        else if (aStep == 2) --aAddr;
        // aStep 1 = fixed, 3 = fixed

        --count;
        ++step;
    }
    d.das = 0;  // DMA завершён
}

// ─── HDMA ─────────────────────────────────────────────────────────────────────

// Реинициализация HDMA: вызывается в начале каждого VBlank-а.
// Сбрасывает a2a в a1t, загружает первый ntrl (и для косвенного — das).
void SnesBus::resetHDMA()
{
    if (!hdmaen_) return;
    hdmaInit_ = true;
}

// Вызывается один раз в начале каждого сканлайна (HBlank).
void SnesBus::runHDMA()
{
    if (!hdmaen_) return;

    if (hdmaInit_) {
        // Загружаем начальное состояние для каждого активного канала
        for (int ch = 0; ch < 8; ++ch) {
            if (!(hdmaen_ & (1 << ch))) continue;
            DmaChannel& d = dma_[ch];
            d.a2a         = d.a1t;
            d.hdmaFinished= false;
            bool indirect = (d.dmap & 0x40) != 0;
            // Читаем первый NTRL из таблицы
            d.ntrl = read((uint32_t)((d.a1b << 16) | d.a2a++));
            if (d.ntrl == 0) { d.hdmaFinished = true; continue; }
            if (indirect) {
                // Косвенный режим: после NTRL следует 2-байтовый адрес данных
                uint8_t lo = read((uint32_t)((d.a1b << 16) | d.a2a++));
                uint8_t hi = read((uint32_t)((d.a1b << 16) | d.a2a++));
                d.das = (uint16_t)(lo | (hi << 8));
            }
        }
        hdmaInit_ = false;
    }

    // Один шаг HDMA для каждого активного канала
    for (int ch = 0; ch < 8; ++ch) {
        if (!(hdmaen_ & (1 << ch))) continue;
        execHDMACh(ch);
    }
}

void SnesBus::execHDMACh(int ch)
{
    DmaChannel& d = dma_[ch];
    if (d.hdmaFinished) return;

    bool indirect = (d.dmap & 0x40) != 0;
    bool doTransfer = false;

    // ── Начало новой группы: счётчик исчерпан ───────────────────────────────
    // Загружаем NTRL (line counter + mode bit), и для indirect — новый das.
    // Первая строка группы ВСЕГДА получает transfer.
    if ((d.ntrl & 0x7F) == 0) {
        d.ntrl = read((uint32_t)((d.a1b << 16) | d.a2a++));
        if (d.ntrl == 0) { d.hdmaFinished = true; return; }
        if (indirect) {
            uint8_t lo = read((uint32_t)((d.a1b << 16) | d.a2a++));
            uint8_t hi = read((uint32_t)((d.a1b << 16) | d.a2a++));
            d.das = (uint16_t)(lo | (hi << 8));
        }
        doTransfer = true;
    } else {
        // Внутри группы. Бит 7 NTRL:
        //   1 = Continue mode — каждая строка получает СВЕЖИЕ байты из source
        //   0 = Same mode — байты были прочитаны в первой строке группы,
        //                   следующие N-1 строк просто пропускаем (для PPU
        //                   регистры уже установлены)
        doTransfer = (d.ntrl & 0x80) != 0;
    }

    if (doTransfer) {
        uint8_t mode = d.dmap & 0x07;
        int     plen = kDmaPatternLen[mode];
        uint32_t src = indirect
            ? (uint32_t)((d.dasb << 16) | d.das)
            : (uint32_t)((d.a1b  << 16) | d.a2a);

        for (int i = 0; i < plen; ++i) {
            int regOff = kDmaPattern[mode][i];
            uint16_t bAddr = (uint16_t)(0x2100 + d.bbad + regOff);
            writeIO(bAddr, read((uint32_t)(src + (uint32_t)i)));
        }

        // Двигаем указатель источника ТОЛЬКО при реальном transfer
        if (indirect) d.das = (uint16_t)(d.das + (uint16_t)plen);
        else          d.a2a = (uint16_t)(d.a2a + (uint16_t)plen);
    }

    // ВСЕГДА уменьшаем счётчик строк (бит 7 не трогаем — он уцелеет
    // пока биты 6:0 не достигнут 0, после чего загрузим новый NTRL).
    d.ntrl = (uint8_t)((d.ntrl & 0x80) | ((d.ntrl - 1) & 0x7F));
}

// ─── Battery SRAM ─────────────────────────────────────────────────────────────
bool SnesBus::saveSram(const std::string& path) const
{
    if (sram_.empty()) return true;
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(sram_.data()),
            static_cast<std::streamsize>(sram_.size()));
    return f.good();
}

bool SnesBus::loadSram(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    if (sz != sram_.size()) {
        sram_.resize(sz);
    }
    f.read(reinterpret_cast<char*>(sram_.data()),
           static_cast<std::streamsize>(sz));
    sramDirty_ = false;
    return f.good();
}
