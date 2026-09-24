#pragma once
#include <cstdint>
#include <string>
#include <vector>

// ─── Картридж Game Boy ───────────────────────────────────────────────────────
// Заголовок ($0100-$014F), переключение банков (MBC1, MBC2, MBC3 с часами
// реального времени, MBC5) и ОЗУ картриджа с батарейкой.
//
// Карта: $0000-$3FFF — банк ПЗУ 0 (у MBC1 в режиме 1 может быть другой),
//        $4000-$7FFF — переключаемый банк ПЗУ,
//        $A000-$BFFF — ОЗУ картриджа или регистр часов (MBC3).
class GbCart {
public:
    enum class Mbc : uint8_t { None, Mbc1, Mbc2, Mbc3, Mbc5 };

    // Разобрать образ ПЗУ. false — образ слишком мал или тип не поддержан.
    bool load(std::vector<uint8_t> rom);

    void    resetMapper();                      // регистры MBC как после включения
    uint8_t read(uint16_t addr) const;          // $0000-$7FFF и $A000-$BFFF
    void    write(uint16_t addr, uint8_t data);

    // Время для часов MBC3: эмулятор сообщает, сколько секунд прошло.
    void    advanceRtc(uint64_t seconds);

    // ── Заголовок ─────────────────────────────────────────────────────────────
    const std::string& title() const { return title_; }
    bool     cgbSupported() const { return (cgbFlag_ & 0x80) != 0; }
    Mbc      mbc()          const { return mbc_; }
    bool     hasBattery()   const { return battery_; }
    bool     hasRtc()       const { return rtc_; }
    uint32_t romHash()      const { return romHash_; }

    // ── Батарейное ОЗУ: сырой образ ОЗУ + 48 байт часов (формат VBA/BGB) ─────
    bool isRamDirty() const { return ramDirty_; }
    void clearRamDirty()    { ramDirty_ = false; }
    std::vector<uint8_t> saveData() const;
    bool loadSaveData(const std::vector<uint8_t>& data, uint64_t nowUnix);

    template<class S> void serialize(S& s);

private:
    std::vector<uint8_t> rom_;
    std::vector<uint8_t> ram_;
    std::string title_;
    Mbc      mbc_       = Mbc::None;
    uint8_t  cgbFlag_   = 0;
    bool     battery_   = false;
    bool     rtc_       = false;
    bool     ramDirty_  = false;
    uint32_t romHash_   = 0;
    uint32_t romBanks_  = 2;         // банков по 16 КБ
    uint32_t ramBanks_  = 0;         // банков по 8 КБ

    // Регистры MBC
    bool     ramEnable_ = false;
    uint16_t romBank_   = 1;         // MBC1: 5 бит, MBC3: 7, MBC5: 9
    uint8_t  ramBank_   = 0;         // MBC1: 2 бита (или старшие биты ПЗУ)
    uint8_t  mode_      = 0;         // MBC1: режим банков

    // Часы MBC3: секунды, минуты, часы, день (9 бит), halt и перенос дня
    struct Rtc {
        uint8_t s = 0, m = 0, h = 0, dl = 0, dh = 0;
    };
    Rtc      rtcLive_;
    Rtc      rtcLatched_;
    uint8_t  rtcLatchPrev_ = 0xFF;

    uint32_t romOffset(uint32_t bank, uint16_t addr) const;
    uint32_t ramOffset(uint16_t addr) const;
    void     rtcTickSecond();
};
