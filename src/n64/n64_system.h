#pragma once
#include "n64_cpu.h"
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ─── Nintendo 64: шина, регистры RCP, PIF и картридж ─────────────────────────
// Физические адреса:
//   0x00000000 RDRAM (8 МБ — с Expansion Pak)      0x04300000 MI  прерывания
//   0x04000000 SP DMEM, 0x04001000 SP IMEM         0x04400000 VI  видео
//   0x04040000 регистры SP, 0x04080000 SP_PC       0x04500000 AI  звук
//   0x04100000 регистры DP (RDP)                   0x04600000 PI  картридж
//   0x08000000 SRAM картриджа                      0x04700000 RI  интерфейс RDRAM
//   0x10000000 ПЗУ картриджа                       0x04800000 SI  PIF/джойстики
//   0x1FC007C0 64 байта ОЗУ PIF
// Вся память хранится в порядке байтов N64 (big-endian).
//
// Загрузка: PIF эмулируется на высоком уровне — кладёт IPL3 картриджа в DMEM
// и ставит регистры, как настоящий загрузчик. Дальше исполняется IPL3 из
// картриджа: грузит игру, проверяет контрольную сумму и прыгает в неё.
//
// Время — такты процессора (93.75 МГц). События (строка VI, конец DMA,
// буфер звука) стоят в маленьком расписании.
class N64System {
public:
    static constexpr uint32_t RDRAM_SIZE = 8 * 1024 * 1024;
    static constexpr uint32_t CPU_HZ = 93750000;

    enum class Tv   : uint8_t { PAL = 0, NTSC = 1, MPAL = 2 };
    enum class Cic  : uint8_t { X101, X102, X103, X105, X106, X7102 };
    enum class Save : uint8_t { None, Eeprom4K, Eeprom16K, Sram };

    N64System();

    // Образ .z64 (big-endian), .v64 (байты попарно) или .n64 (little-endian).
    bool loadRom(std::vector<uint8_t> data);
    void reset();                     // включение питания
    void runFrame();                  // одно поле VI (1/60 или 1/50 секунды)

    Vr4300 cpu;

    // ── Картридж ─────────────────────────────────────────────────────────────
    const std::string& title() const { return title_; }
    const std::string& gameCode() const { return gameCode_; }   // «NSMP» и т.п.
    Tv   tvType() const { return tv_; }
    Cic  cic() const { return cic_; }
    Save saveType() const { return save_; }

    // ── Память для процессора (физические адреса) ────────────────────────────
    uint8_t  read8(uint32_t pa);
    uint16_t read16(uint32_t pa);
    uint32_t read32(uint32_t pa)
    {
        if (pa < RDRAM_SIZE) return get32(&rdram_[pa]);
        return readIo(pa);
    }
    uint64_t read64(uint32_t pa) { return ((uint64_t)read32(pa) << 32) | read32(pa + 4); }
    void write8(uint32_t pa, uint8_t v);
    void write16(uint32_t pa, uint16_t v);
    void write32(uint32_t pa, uint32_t v, uint32_t mask = 0xFFFFFFFFu)
    {
        if (pa < RDRAM_SIZE) {
            uint8_t* p = &rdram_[pa];
            put32(p, mask == 0xFFFFFFFFu ? v : (get32(p) & ~mask) | (v & mask));
            return;
        }
        writeIo(pa, v, mask);
    }
    void write64(uint32_t pa, uint64_t v, uint64_t mask = ~0ull)
    {
        write32(pa, (uint32_t)(v >> 32), (uint32_t)(mask >> 32));
        write32(pa + 4, (uint32_t)v, (uint32_t)mask);
    }

    uint8_t*       rdram()       { return rdram_.data(); }
    const uint8_t* rdram() const { return rdram_.data(); }
    uint8_t*       spMem()       { return spMem_.data(); }      // DMEM (0x000) + IMEM (0x1000)

    // ── Видео: кадр ARGB8888 ─────────────────────────────────────────────────
    const uint32_t* frame() const { return frame_.data(); }
    uint32_t*       frame()       { return frame_.data(); }
    int frameWidth()  const { return frameW_; }
    int frameHeight() const { return frameH_; }

    // ── Ввод: кнопки в формате джойстика N64 и аналоговый стик ───────────────
    void setController(int port, uint16_t buttons, int8_t x, int8_t y);

    // ── Звук: моно 44100 Гц ──────────────────────────────────────────────────
    static constexpr int SAMPLE_RATE = 44100;
    const std::vector<float>& samples() const { return samples_; }
    void clearSamples() { samples_.clear(); }

    // ── Сохранения картриджа (EEPROM или SRAM) ───────────────────────────────
    bool hasSave() const { return save_ != Save::None; }
    bool saveDirty() const { return saveDirty_; }
    void clearSaveDirty() { saveDirty_ = false; }
    std::vector<uint8_t> saveData() const;
    bool loadSaveData(const std::vector<uint8_t>& data);

    // ── Диагностика ──────────────────────────────────────────────────────────
    uint64_t frameCount() const { return frames_; }
    const std::string& isViewerLog() const { return isvLog_; }   // вывод отладочного порта IS-Viewer
    uint32_t viRegister(int index) const { return vi_[index & 15]; }

    // ── Прерывания MI: 0 SP, 1 SI, 2 AI, 3 VI, 4 PI, 5 DP ────────────────────
    enum : uint32_t { MI_SP = 1, MI_SI = 2, MI_AI = 4, MI_VI = 8, MI_PI = 16, MI_DP = 32 };
    void raiseMi(uint32_t bits) { miIntr_ |= bits; updateIrq(); }
    void clearMi(uint32_t bits) { miIntr_ &= ~bits; updateIrq(); }

    static uint32_t get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
    static void     put32(uint8_t* p, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(p, &v, 4); }

private:
    std::vector<uint8_t> rdram_;
    std::array<uint8_t, 0x2000> spMem_{};
    std::vector<uint8_t> rom_;
    std::array<uint8_t, 64> pifRam_{};
    std::vector<uint8_t> eeprom_;
    std::vector<uint8_t> sram_;
    bool saveDirty_ = false;

    std::string title_, gameCode_;
    Tv   tv_ = Tv::NTSC;
    Cic  cic_ = Cic::X102;
    Save save_ = Save::None;

    // ── Расписание событий ───────────────────────────────────────────────────
    enum Event { EV_VI_LINE, EV_PI_DMA, EV_SI_DMA, EV_AI_BUF, EV_COUNT };
    static constexpr uint64_t NEVER = ~0ull;
    std::array<uint64_t, EV_COUNT> eventAt_{};
    void schedule(Event e, uint64_t delay);
    void handleEvent(Event e);

    // ── MI ───────────────────────────────────────────────────────────────────
    uint32_t miMode_ = 0, miIntr_ = 0, miMask_ = 0;
    void updateIrq() { cpu.setRcpInterrupt((miIntr_ & miMask_) != 0); }

    // ── VI ───────────────────────────────────────────────────────────────────
    enum { VI_CTRL, VI_ORIGIN, VI_WIDTH, VI_V_INTR, VI_V_CURRENT, VI_BURST, VI_V_SYNC,
           VI_H_SYNC, VI_LEAP, VI_H_VIDEO, VI_V_VIDEO, VI_V_BURST, VI_X_SCALE, VI_Y_SCALE };
    std::array<uint32_t, 16> vi_{};
    uint32_t viLine_ = 0;             // текущая строка поля
    uint32_t viField_ = 0;            // поле (для чересстрочного режима)
    bool     frameDone_ = false;
    uint64_t frames_ = 0;
    std::vector<uint32_t> frame_;
    int      frameW_ = 320, frameH_ = 240;
    uint32_t viLinesPerField() const;
    uint32_t viCyclesPerLine() const;
    void     viLine();
    void     renderFrame();

    // ── AI ───────────────────────────────────────────────────────────────────
    struct AiBuffer { uint32_t addr = 0, len = 0; };
    AiBuffer aiFifo_[2];
    int      aiCount_ = 0;            // буферов в очереди (0..2), [0] играет
    uint32_t aiDramAddr_ = 0, aiControl_ = 0, aiDacRate_ = 0, aiBitRate_ = 0;
    uint64_t aiStart_ = 0;            // такт начала текущего буфера
    uint64_t aiDuration_ = 0;
    double   aiResample_ = 0.0;       // дробная позиция ресэмплера
    std::vector<float> samples_;
    void     aiStartBuffer();

    // ── PI ───────────────────────────────────────────────────────────────────
    uint32_t piDram_ = 0, piCart_ = 0, piStatus_ = 0;
    uint32_t piBsd_[8]{};             // DOM1 LAT/PWD/PGS/RLS, DOM2 LAT/PWD/PGS/RLS
    bool     piBusy_ = false;
    void     piDma(bool toRdram, uint32_t len);
    uint8_t  cartRead8(uint32_t addr) const;
    void     cartWrite8(uint32_t addr, uint8_t v);

    // ── SI и PIF ─────────────────────────────────────────────────────────────
    uint32_t siDram_ = 0;
    bool     siBusy_ = false;
    bool     joybusArmed_ = false;    // последняя запись в PIF просила выполнить джойбас
    struct Controller { uint16_t buttons = 0; int8_t x = 0, y = 0; bool present = false; };
    std::array<Controller, 4> pads_{};
    void     pifCommand();            // байт 63 ОЗУ PIF после записи
    void     joybusRun();             // выполнить команды джойбаса в ОЗУ PIF
    bool     joybusDevice(int channel, const uint8_t* tx, int txLen, uint8_t* rx, int rxLen);
    void     cicChallenge();

    // ── RI, SP, DP (пока — регистры и DMA) ───────────────────────────────────
    std::array<uint32_t, 8> ri_{};
    uint32_t spMemAddr_ = 0, spDramAddr_ = 0, spRdLen_ = 0, spWrLen_ = 0;
    uint32_t spStatus_ = 1;           // остановлен
    uint32_t spSemaphore_ = 0, spPc_ = 0;
    void     spDma(bool toRdram, uint32_t reg);
    void     spStatusWrite(uint32_t v);
    uint32_t dpStart_ = 0, dpEnd_ = 0, dpCurrent_ = 0, dpStatus_ = 0x80;

    // ── IS-Viewer: отладочный вывод в область ПЗУ 0x13FF0000 ─────────────────
    std::array<uint8_t, 0x200> isvBuf_{};
    std::string isvLog_;

    uint32_t readIo(uint32_t pa);
    void     writeIo(uint32_t pa, uint32_t v, uint32_t mask);
    void     bootHle();
    void     detectCartridge();
};
