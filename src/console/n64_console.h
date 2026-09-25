#pragma once
#include "iconsole.h"
#include "n64/n64_system.h"
#include <cstdint>
#include <string>
#include <vector>

// ─── N64Console ──────────────────────────────────────────────────────────────
// Nintendo 64. Кадр — одно поле видеоинтерфейса (60 Гц NTSC, 50 Гц PAL);
// размер картинки задаёт игра через регистры VI (обычно 320×240).
class N64Console : public IConsole {
public:
    bool loadROM(const std::string& path) override;
    bool loadROMData(std::vector<uint8_t> rom);      // образ из памяти (тесты)
    void reset()    override;
    void runFrame() override { sys_.runFrame(); }

    uint32_t*       getFramebuffer()       override { return sys_.frame(); }
    const uint32_t* getFramebuffer() const override { return sys_.frame(); }
    int getFrameWidth()  const override { return sys_.frameWidth();  }
    int getFrameHeight() const override { return sys_.frameHeight(); }

    // Раскладка SNES (B|Y|Sel|Sta|Up|Dn|L|R|A|X|LSh|RSh) плюс биты 3-0 — C-кнопки
    // (вверх, вниз, влево, вправо). Крестовина SNES — аналоговый стик.
    void setInput(int player, uint16_t buttons) override;

    const std::vector<float>& getAudioSamples() const override { return sys_.samples(); }
    void clearAudioSamples() override { sys_.clearSamples(); }

    std::string getConsoleName() const override { return "N64"; }

    bool hasBattery()                      const override { return sys_.hasSave(); }
    bool isSramDirty()                     const override { return sys_.saveDirty(); }
    void clearSramDirty()                        override { sys_.clearSaveDirty(); }
    bool saveSram(const std::string& path) const override;
    bool loadSram(const std::string& path)       override;

    N64System& system() { return sys_; }

private:
    N64System sys_;
};
