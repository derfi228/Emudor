#include "ui_i18n.h"

// Строки — прямой порт таблицы I18N из app.js (design_handoff_emudor_ui).
static const I18nStrings kEn = {
    "games in your library", "Search the library...", "Add ROM", "Rescan", "Settings",
    "UI Theme", "Light", "Dark",
    "Game Color Mode", "Normal", "Inverted", "B&W",
    "ROM Folders", "Folders are scanned automatically on startup and when you press rescan.",
    "+ Add folder...", "Language",
    "PAUSED", "Continue", "Save", "Load", "Exit to Menu",
    "Save Game", "Load Game", "Choose a slot:", "Cancel", "Save file", "empty",
};

static const I18nStrings kRu = {
    u8"игр в вашей библиотеке", u8"Поиск в библиотеке...", u8"Добавить ROM", u8"Обновить", u8"Настройки",
    u8"Тема", u8"Светлая", u8"Тёмная",
    u8"Цвет игр", u8"Норм.", u8"Инверт.", u8"Ч/Б",
    u8"Папки ROM", u8"Папки сканируются автоматически при запуске и по кнопке «Обновить».",
    u8"+ Добавить папку...", u8"Язык",
    u8"ПАУЗА", u8"Продолжить", u8"Сохранить", u8"Загрузить", u8"В меню",
    u8"Сохранение", u8"Загрузка", u8"Выберите слот:", u8"Отмена", u8"Слот", u8"пусто",
};

static const I18nStrings kEs = {
    u8"juegos en tu biblioteca", u8"Buscar en la biblioteca...", u8"Añadir ROM", u8"Reescanear", u8"Ajustes",
    u8"Tema", u8"Claro", u8"Oscuro",
    u8"Color del juego", u8"Normal", u8"Invertido", u8"B/N",
    u8"Carpetas ROM", u8"Las carpetas se escanean al iniciar y al presionar reescanear.",
    u8"+ Añadir carpeta...", u8"Idioma",
    u8"PAUSA", u8"Continuar", u8"Guardar", u8"Cargar", u8"Salir al menú",
    u8"Guardar partida", u8"Cargar partida", u8"Elige una ranura:", u8"Cancelar", u8"Ranura", u8"vacío",
};

static const I18nStrings kZh = {
    u8"个游戏在您的库中", u8"搜索游戏库...", u8"添加 ROM", u8"重新扫描", u8"设置",
    u8"界面主题", u8"浅色", u8"深色",
    u8"游戏颜色", u8"正常", u8"反色", u8"黑白",
    u8"ROM 文件夹", u8"启动时自动扫描，按重新扫描可手动刷新。",
    u8"+ 添加文件夹...", u8"语言",
    u8"已暂停", u8"继续", u8"保存", u8"加载", u8"返回菜单",
    u8"保存游戏", u8"加载游戏", u8"选择一个存档位:", u8"取消", u8"存档", u8"空",
};

static const I18nStrings kFr = {
    u8"jeux dans votre bibliothèque", u8"Rechercher dans la bibliothèque...", u8"Ajouter ROM", u8"Rescanner", u8"Paramètres",
    u8"Thème", u8"Clair", u8"Sombre",
    u8"Couleur du jeu", u8"Normal", u8"Inversé", u8"N&B",
    u8"Dossiers ROM", u8"Les dossiers sont scannés au démarrage et avec rescanner.",
    u8"+ Ajouter un dossier...", u8"Langue",
    u8"EN PAUSE", u8"Continuer", u8"Sauvegarder", u8"Charger", u8"Quitter",
    u8"Sauvegarder", u8"Charger", u8"Choisir un emplacement :", u8"Annuler", u8"Sauvegarde", u8"vide",
};

const I18nStrings& GetI18n(Lang lang) {
    switch (lang) {
        case Lang::RU: return kRu;
        case Lang::ES: return kEs;
        case Lang::ZH: return kZh;
        case Lang::FR: return kFr;
        default:       return kEn;
    }
}

const char* LangNativeName(Lang lang) {
    switch (lang) {
        case Lang::RU: return u8"Русский";
        case Lang::ES: return u8"Español";
        case Lang::ZH: return u8"中文";
        case Lang::FR: return u8"Français";
        default:       return "English";
    }
}
