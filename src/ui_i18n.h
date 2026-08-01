#pragma once

// ─── i18n (порт I18N-таблицы из app.js) ───────────────────────────────────────
// Покрытие соответствует оригиналу: переводятся только строки самого UI-каркаса
// (тулбар/секции настроек/пауза/save-load) — имена кнопок раскладок (Up/Down/...)
// и "Rebind"/"Remove" в оригинальном web-макете НЕ переводятся, здесь тоже.
enum class Lang { EN, RU, ES, ZH, FR };

struct I18nStrings {
    const char* gamesInLib;
    const char* search;
    const char* addRom;
    const char* rescan;
    const char* settingsTip;
    const char* uiTheme;
    const char* light;
    const char* dark;
    const char* colorMode;
    const char* normal;
    const char* inverted;
    const char* bw;
    const char* romFolders;
    const char* romHelper;
    const char* addFolder;
    const char* language;
    const char* paused;
    const char* continueBtn;
    const char* save;
    const char* load;
    const char* exit;
    const char* saveGame;
    const char* loadGame;
    const char* chooseSlot;
    const char* cancel;
    const char* slot;
    const char* empty;
};

const I18nStrings& GetI18n(Lang lang);
const char* LangNativeName(Lang lang);   // "English"/"Русский"/... для чипов
