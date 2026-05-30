#include "app.h"

int main(int argc, char** argv) {
    App app;
    // Необязательный аргумент: путь к ROM для автозапуска
    if (argc > 1) app.pendingRomPath_ = argv[1];
    app.run();
    return 0;
}
