#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <iosfwd>

// ─── Абстрактный интерфейс консоли ───────────────────────────────────────────
// Скрывает детали железа от App. Реализуется NesConsole, SnesConsole и т.д.
class IConsole {
public:
    virtual ~IConsole() = default;

    // ─── Жизненный цикл ────────────────────────────────────────────────────
    // Загружает ROM и выполняет полный сброс (CPU+PPU+APU+Bus).
    // Возвращает false если формат не поддерживается.
    virtual bool loadROM(const std::string& path) = 0;

    // Мягкий сброс (без перезагрузки ROM).
    virtual void reset() = 0;

    // Выполнить ровно один кадр эмуляции.
    virtual void runFrame() = 0;

    // ─── Видео ─────────────────────────────────────────────────────────────
    // ARGB8888, размер getFrameWidth() × getFrameHeight() пикселей.
    virtual       uint32_t* getFramebuffer()       = 0;
    virtual const uint32_t* getFramebuffer() const = 0;
    virtual int getFrameWidth()  const = 0;
    virtual int getFrameHeight() const = 0;

    // ─── Ввод ──────────────────────────────────────────────────────────────
    // player = 0 или 1.
    // NES:  buttons = A|B|Sel|Sta|Up|Dn|L|R (биты 7–0, верхний байт не используется)
    // SNES: buttons = B|Y|Sel|Sta|Up|Dn|L|R|A|X|LSh|RSh|0|0|0|0 (биты 15–0)
    virtual void setInput(int player, uint16_t buttons) = 0;

    // ─── Аудио ─────────────────────────────────────────────────────────────
    // Буфер float-сэмплов, накопленных за runFrame().
    virtual const std::vector<float>& getAudioSamples() const = 0;
    virtual void clearAudioSamples() = 0;

    // ─── Идентификация ─────────────────────────────────────────────────────
    virtual std::string getConsoleName() const = 0;

    // ─── Battery SRAM (опционально) ────────────────────────────────────────
    virtual bool hasBattery()                              const { return false; }
    virtual bool isSramDirty()                             const { return false; }
    virtual void clearSramDirty()                                {}
    virtual bool saveSram(const std::string& /*path*/)     const { return false; }
    virtual bool loadSram(const std::string& /*path*/)           { return false; }

    // ─── Save state (опционально) ──────────────────────────────────────────
    virtual bool saveState(std::ostream& /*os*/) const { return false; }
    virtual bool loadState(std::istream& /*is*/)       { return false; }
};
