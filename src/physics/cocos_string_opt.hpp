#pragma once

// Оптимизация CCString::initWithFormatAndValist (стековый буфер вместо
// 100 KB на куче за вызов). Устанавливается ЯВНО из main.cpp ПОСЛЕ
// offsets::logReport(): хук перезаписывает байты сайта, и сверка реестра
// после установки видела бы уже пропатченные байты.

namespace skk {
    bool installCocosStringOpt();
}
