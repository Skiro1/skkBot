#pragma once

// BUG-105b v3 (05.11.2026): raw-хуки UILayer::keyDown/handleAction.
//
// $modify-хуки Geode передают оригиналу неверный this: UILayer 2.2091 —
// ромб (`UILayer : CCLayerColor, CCKeyboardDelegate`, CCLayer уже наследует
// CCKeyboardDelegate), Geode подставляет прямой базовый подобъект (+0x220),
// а ванильные реализации ожидают подобъект CCLayer-цепочки (+0x158) — краш
// на любом игровом keybind-действии и на Escape (docs/Баги.md, BUG-105b).
// Поэтому keyDown/handleAction хукаются напрямую через safetyhook: detour
// получает raw this от диспетчера, оригинал зовётся через trampoline без
// поправок. Устанавливать СТРОГО после skk::offsets::logReport() (реестр
// сверяет байты сайтов до установки хуков).
namespace skk {
bool installUiInputWallHooks();
} // namespace skk
