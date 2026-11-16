#include "skc_rng.hpp"

#include "skc_bot.hpp"
#include "skc_frame.hpp"
#include "skc_range.hpp"
#include "core/rng_lock.hpp"

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <logger/logger.hpp>

#include <algorithm>
#include <cstring>

namespace skk::skc {

namespace {

// Хвост: 7-байтный магик + версия, u32 count, затем count * (u32 frame +
// u64 primary + u64 teleport + u64 shake), little-endian. 28 байт на якорь.
constexpr uint8_t kTailMagic[7] = {'S', 'K', 'C', 'R', 'N', 'G', 0x01u};
// On-disk header tail: magic(7) + reserved u32(0) + version u32 + count u32.
// Замер 07.11 ('Mk3_autosave' 4476 B: tailSize=3967 = 19+141·28, version на
// +11 читает 1; 'OKAK_autosave' 342 B: 47 = 19+1·28). Исторически reserved
// появлялся как незаписанный остаток resize()'а в писателе (putU32 после
// resize+memcpy пишется на +11, а не +7) — расклад закреплён канонично,
// reserved пишется ЯВНО и равен 0.
constexpr size_t kTailFixed =
    sizeof(kTailMagic) + sizeof(uint32_t) * 3;
constexpr size_t kEntryBytes = sizeof(uint32_t) + sizeof(uint64_t) * 3;

// Версии хвоста: 1 — сырой расклад (28 Б/якорь, старые файлы), 2 —
// range-сжатый (тег same/changed + 64 бита изменившихся), 3 —
// advance-кодирование (см. encodeRngJournalBodyV3). Читатель поддерживает все.
constexpr uint32_t kTailVersionV1 = kRngTailVersion;
constexpr uint32_t kTailVersionV2 = 2u;
constexpr uint32_t kTailVersionV3 = 3u;
constexpr uint32_t kTailVersionV4 = 4u;
// v5 (12.11): канонизация shake до 31 бита. Вывод цепочки (lcgOutput = биты
// 16..30) зависит ТОЛЬКО от младших 31 бита состояния: переносы LCG
// распространяются вверх, старшие биты на младшие не влияют, а читает
// состояние только наш midhook через lcgOutput. Хранение state & 0x7FFFFFFF
// даёт все будущие выводы бит-в-бит (lossless по поведению) и резко дешевит
// shake-поток: raw-блок 31 бит вместо 64, а почти все якоря уходят в
// advance/pin по рекуррентности mod 2^31.
constexpr uint32_t kTailVersionV5 = 5u;
constexpr uint64_t kShakeMask = 0x7FFFFFFFULL;
inline uint64_t shakeCanon(uint64_t v) { return v & kShakeMask; }
// Рекуррентность shake в каноническом домене: младшие 31 бит эволюционируют
// автономно, поэтому lcg^k по модулю 2^31 достижим так же, как в 2^64.
inline uint64_t shakeAdv31(uint64_t s) {
    return (214013ULL * s + 2531011ULL) & kShakeMask;
}
// База shake-цепочки при ресете попытки: pinTripleSeedAtReset ставит
// g_shakeState = primary & 0x7FFF, primary пинится в kSkcLayerSeed.
constexpr uint64_t kShakePinBase = kSkcLayerSeed & 0x7FFFULL;

void putU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

void putU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// --- сжатый поток якорей (v2) ----------------------------------------------

// Символ-тег на поток: 0 = не изменился с предыдущего якоря, 1 = изменился.
constexpr uint32_t kTagSame = 0u;
constexpr uint32_t kTagChanged = 1u;

// Энкод: один ValueModel на дельты кадров, отдельный 2-символьный
// SymbolModel на тег каждого потока (контекст = индекс потока), и
// бинарная модель (2 символа, контекст = поток*8 + позиция бита) для
// изменившихся 64-битных значений. Обе стороны выполняют идентичную
// последовательность вызовов — детерминизм гарантирован.
void encodeRngJournalBody(std::vector<uint8_t>& bytes,
                          const std::vector<RngJournalEntry>& journal) {
    RangeEncoder enc;
    ValueModel frameDelta;
    // Теги: контекст = индекс потока (0..2).
    SymbolModel tag(2u, 3u);
    // Биты изменившихся значений: контекст = stream*8 + (bitIdx & 7).
    SymbolModel bits(2u, 24u);

    uint32_t prevFrame = 0;
    uint64_t prev[3] = {0, 0, 0};
    for (const auto& entry : journal) {
        frameDelta.encode(enc, entry.frame - prevFrame);
        prevFrame = entry.frame;
        const uint64_t cur[3] = {entry.primary, entry.teleport, entry.shake};
        for (uint32_t s = 0; s < 3; ++s) {
            if (cur[s] == prev[s]) {
                tag.encode(enc, kTagSame, s);
                continue;
            }
            tag.encode(enc, kTagChanged, s);
            for (uint32_t i = 0; i < 64; ++i)
                bits.encode(enc, static_cast<uint32_t>((cur[s] >> i) & 1u),
                            s * 8u + (i & 7u));
        }
        prev[0] = cur[0]; prev[1] = cur[1]; prev[2] = cur[2];
    }

    auto packed = enc.finish();
    bytes.insert(bytes.end(), packed.begin(), packed.end());
}

bool decodeRngJournalBody(const uint8_t* data, size_t size, uint32_t count,
                          std::vector<RngJournalEntry>& out) {
    RangeDecoder dec(data, size);
    ValueModel frameDelta;
    SymbolModel tag(2u, 3u);
    SymbolModel bits(2u, 24u);

    out.clear();
    out.reserve(count);
    uint32_t prevFrame = 0;
    uint64_t prev[3] = {0, 0, 0};
    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t frame = frameDelta.decode(dec) + prevFrame;
        prevFrame = frame;
        RngJournalEntry entry;
        entry.frame = frame;
        for (uint32_t s = 0; s < 3; ++s) {
            if (tag.decode(dec, s) == kTagSame) {
                // Значение можно не читать: streamId 0/1/2 → своё поле ниже.
                continue;
            }
            uint64_t v = 0;
            for (uint32_t i = 0; i < 64; ++i)
                v |= static_cast<uint64_t>(bits.decode(dec, s * 8u + (i & 7u))) << i;
            prev[s] = v;
        }
        entry.primary = prev[0];
        entry.teleport = prev[1];
        entry.shake = prev[2];
        out.push_back(entry);
    }
    return true;
}

// --- сжатый поток якорей (v3) ------------------------------------------------
// Математический минимум (пользователь 10.11): ВСЕ ТРИ потока живут
// рекуррентностью MSVC LCG state = 214013*state + 2531011 (rng_lock.hpp:34) -
// primary (0x90BF68), teleport (0x90BFF0), shake (своя цепочка бота,
// rng_lock.cpp:154). Значит между двумя якорями записанное состояние = lcg^k
// от предыдущего, и вместо 64 сырых бит достаточно ЧИСЛА ШАГОВ k (обычно
// 0..5 - пара бит). Теги на поток: 0 = same (k=0), 1 = advance (k шагов,
// значение восстанавливается возведением), 2 = raw (64 бита, форс-ресид
// между якорями - редчайший случай). Потеря нуля: любой файл декодируется
// бит-в-бит, raw-фоллбэк гарантирует lossless на любом потоке.
constexpr uint32_t kTag3Same = 0u;
constexpr uint32_t kTag3Advance = 1u;
constexpr uint32_t kTag3Raw = 2u;
// v5 (12.11): четвёртый тег ТОЛЬКО в v4-потоке (v3 читатель ждёт 3 символа
// на этот SymbolModel и не должен видеть 4). Шовные якоря (ресид/стичь)
// прыгают не по рекуррентности от предыдущего значения, а от ПИНА:
// forceLayerSeeds выставляет оба сида константой kSkcLayerSeed, shake-цепочка
// пинится на ресете. Состояние после шва = lcg^j(pin) - вместо 192 сырых бит
// (raw x3 потока, ~24 Б/якорь; живой замер 01:29: 16 якорей = 182 B, почти
// весь хвост) пишется тег + число шагов j.
constexpr uint32_t kTag4Pin = 3u;
// Потолок поиска шага: якоря плотные (каждый кадр/ввод), расход рандома
// между якорями - единицы; даже 240-кадровое окно простоя даёт k порядка
// сотен. Не нашли k за 16384 шагов - пишем raw.
constexpr uint32_t kMaxAdvanceSearch = 16384u;

// k такой, что lcg^k(prev) == cur; false, если рекуррентность нарушена.
bool lcgReachable(uint64_t prev, uint64_t cur, uint32_t& kOut) {
    uint64_t s = prev;
    for (uint32_t k = 1; k <= kMaxAdvanceSearch; ++k) {
        s = lcgAdvance(s);
        if (s == cur) {
            kOut = k;
            return true;
        }
    }
    return false;
}

// Достижимость в каноническом домене shake (v5): рекуррентность mod 2^31.
bool shakeReachable31(uint64_t prev, uint64_t cur, uint32_t& kOut) {
    uint64_t s = prev;
    for (uint32_t k = 1; k <= kMaxAdvanceSearch; ++k) {
        s = shakeAdv31(s);
        if (s == cur) {
            kOut = k;
            return true;
        }
    }
    return false;
}

void encodeRngJournalBodyV3(std::vector<uint8_t>& bytes,
                            const std::vector<RngJournalEntry>& journal) {
    RangeEncoder enc;
    ValueModel frameDelta;
    // Тег потока: same / advance / raw, контекст = индекс потока (0..2).
    SymbolModel tag(3u, 3u);
    // k шага: малые значения напрямую (0..15), хвост ValueModel'ом,
    // контекст = поток.
    DirectValueModel kModel(16u);
    // Биты raw-значений: контекст = stream*8 + (bitIdx & 7) - как в v2.
    SymbolModel bits(2u, 24u);

    uint32_t prevFrame = 0;
    uint64_t prev[3] = {0, 0, 0};
    for (const auto& entry : journal) {
        frameDelta.encode(enc, entry.frame - prevFrame);
        prevFrame = entry.frame;
        const uint64_t cur[3] = {entry.primary, entry.teleport, entry.shake};
        for (uint32_t s = 0; s < 3; ++s) {
            if (cur[s] == prev[s]) {
                tag.encode(enc, kTag3Same, s);
                continue;
            }
            uint32_t k = 0;
            if (lcgReachable(prev[s], cur[s], k)) {
                tag.encode(enc, kTag3Advance, s);
                kModel.encode(enc, k);
            } else {
                tag.encode(enc, kTag3Raw, s);
                for (uint32_t i = 0; i < 64; ++i)
                    bits.encode(enc, static_cast<uint32_t>((cur[s] >> i) & 1u),
                                s * 8u + (i & 7u));
            }
            prev[s] = cur[s];
        }
    }

    auto packed = enc.finish();
    bytes.insert(bytes.end(), packed.begin(), packed.end());
}

bool decodeRngJournalBodyV3(const uint8_t* data, size_t size, uint32_t count,
                            std::vector<RngJournalEntry>& out) {
    RangeDecoder dec(data, size);
    ValueModel frameDelta;
    SymbolModel tag(3u, 3u);
    DirectValueModel kModel(16u);
    SymbolModel bits(2u, 24u);

    out.clear();
    out.reserve(count);
    uint32_t prevFrame = 0;
    uint64_t prev[3] = {0, 0, 0};
    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t frame = frameDelta.decode(dec) + prevFrame;
        prevFrame = frame;
        RngJournalEntry entry;
        entry.frame = frame;
        for (uint32_t s = 0; s < 3; ++s) {
            const uint32_t t = tag.decode(dec, s);
            if (t == kTag3Same) {
                // prev[s] остаётся.
            } else if (t == kTag3Advance) {
                const uint32_t k = kModel.decode(dec);
                for (uint32_t i = 0; i < k; ++i) prev[s] = lcgAdvance(prev[s]);
            } else {
                uint64_t v = 0;
                for (uint32_t i = 0; i < 64; ++i)
                    v |= static_cast<uint64_t>(bits.decode(dec, s * 8u + (i & 7u))) << i;
                prev[s] = v;
            }
        }
        entry.primary = prev[0];
        entry.teleport = prev[1];
        entry.shake = prev[2];
        out.push_back(entry);
    }
    return true;
}

// --- сжатый поток якорей (v4) ------------------------------------------------// Run-length поверх v3 (пользователь 10.11): реальная информация журнала -
// точки СМЕНЫ паттерна расхода рандома, а не якоря. Пока у всех трёх потоков
// шаг k между соседними якорями не меняется (стабильные анимации уровня),
// якорь кодируется одним битом "прогон" - декодер сам применяет предыдущий
// кортеж (kP, kT, kS) к предыдущим состояниям. Любой raw в кортеже ломает
// повтор (состояние пришло не из рекуррентности) до следующего чистого
// якоря. Плюс "выжать до бита": контекст модели k = поток*8 + min(prevK,7) -
// стабильный паттерн "всегда 2" кодируется почти бесплатно.
constexpr uint32_t kTag4Repeat = 0u;
constexpr uint32_t kTag4Anchor = 1u;
constexpr uint32_t kKContexts = 24u;  // 3 потока * 8 контекстов prevK

inline uint32_t kContextOf(uint32_t stream, uint32_t prevK) {
    return stream * 8u + (prevK < 8u ? prevK : 7u);
}

void encodeRngJournalBodyV4(std::vector<uint8_t>& bytes,
                            const std::vector<RngJournalEntry>& journal,
                            bool shake31) {
    RangeEncoder enc;
    ValueModel frameDelta;
    SymbolModel runTag(2u, 1u);          // repeat / полный якорь
    SymbolModel tag(4u, 3u);             // same/advance/raw/pin (контекст=поток)
    DirectContextValueModel kModel(kKContexts, 16u);
    SymbolModel bits(2u, 24u);

    uint32_t prevFrame = 0;
    uint64_t prev[3] = {0, 0, 0};
    uint32_t prevK[3] = {0, 0, 0};
    bool havePattern = false;   // есть кортеж для повтора
    bool rawInPrev = false;     // в прошлом кортеже был raw
    for (const auto& entry : journal) {
        frameDelta.encode(enc, entry.frame - prevFrame);
        prevFrame = entry.frame;
        // v5 shake31: shake живёт в каноническом домене mod 2^31.
        const uint64_t curShake =
            shake31 ? shakeCanon(entry.shake) : entry.shake;
        const uint64_t cur[3] = {entry.primary, entry.teleport, curShake};
        uint32_t k[3] = {0, 0, 0};
        uint32_t tagId[3] = {kTag3Raw, kTag3Raw, kTag3Raw};
        for (uint32_t s = 0; s < 3; ++s) {
            const bool dom31 = shake31 && s == 2u;
            if (cur[s] == prev[s]) {
                tagId[s] = kTag3Same;
            } else if (dom31 ? shakeReachable31(prev[s], cur[s], k[s])
                             : lcgReachable(prev[s], cur[s], k[s])) {
                tagId[s] = kTag3Advance;
            } else if (dom31
                           ? shakeReachable31(kShakePinBase, cur[s], k[s])
                           : lcgReachable(kSkcLayerSeed, cur[s], k[s])) {
                // Шов: состояние пришло от пина, а не от предыдущего якоря.
                tagId[s] = kTag4Pin;
            }
        }
        const bool clean = tagId[0] != kTag3Raw && tagId[1] != kTag3Raw &&
                           tagId[2] != kTag3Raw;
        // Repeat декодируется ТОЛЬКО как advance от prev. Pin с тем же k
        // начинается от константы и не может использовать этот opcode.
        const bool advancesOnly = tagId[0] != kTag4Pin &&
                                  tagId[1] != kTag4Pin && tagId[2] != kTag4Pin;
        const bool repeat = havePattern && !rawInPrev && clean && advancesOnly &&
                            k[0] == prevK[0] && k[1] == prevK[1] &&
                            k[2] == prevK[2];
        if (repeat) {
            runTag.encode(enc, kTag4Repeat, 0u);
            // Состояния уезжают вперёд тем же кортежом шагов.
            for (uint32_t s = 0; s < 3; ++s) {
                const bool dom31r = shake31 && s == 2u;
                for (uint32_t i = 0; i < prevK[s]; ++i)
                    prev[s] = dom31r ? shakeAdv31(prev[s]) : lcgAdvance(prev[s]);
            }
        } else {
            runTag.encode(enc, kTag4Anchor, 0u);
            for (uint32_t s = 0; s < 3; ++s) {
                tag.encode(enc, tagId[s], s);
                if (tagId[s] == kTag3Same) {
                    prevK[s] = 0;
                    continue;
                }
                if (tagId[s] != kTag3Raw) {
                    // Advance и Pin кодируются числом шагов k.
                    kModel.encode(enc, k[s], kContextOf(s, prevK[s]));
                    prevK[s] = k[s];
                } else {
                    // v5 shake31: raw shake - 31 бит вместо 64.
                    const uint32_t nbits =
                        (shake31 && s == 2u) ? 31u : 64u;
                    for (uint32_t i = 0; i < nbits; ++i)
                        bits.encode(enc,
                                    static_cast<uint32_t>((cur[s] >> i) & 1u),
                                    s * 8u + (i & 7u));
                    prevK[s] = 0;
                }
                prev[s] = cur[s];
            }
        }
        havePattern = true;
        rawInPrev = !clean;
    }
    auto packed = enc.finish();
    bytes.insert(bytes.end(), packed.begin(), packed.end());
}

bool decodeRngJournalBodyV4(const uint8_t* data, size_t size, uint32_t count,
                            std::vector<RngJournalEntry>& out,
                            bool shake31) {
    RangeDecoder dec(data, size);
    ValueModel frameDelta;
    SymbolModel runTag(2u, 1u);
    SymbolModel tag(4u, 3u);             // same/advance/raw/pin (контекст=поток)
    DirectContextValueModel kModel(kKContexts, 16u);
    SymbolModel bits(2u, 24u);

    out.clear();
    out.reserve(count);
    uint32_t prevFrame = 0;
    uint64_t prev[3] = {0, 0, 0};
    uint32_t prevK[3] = {0, 0, 0};
    for (uint32_t n = 0; n < count; ++n) {
        const uint32_t frame = frameDelta.decode(dec) + prevFrame;
        prevFrame = frame;
        RngJournalEntry entry;
        entry.frame = frame;
        // ВНИМАНИЕ: бит runTag кодируется на КАЖДЫЙ якорь, включая #0 -
        // декодер обязан прочитать его всегда, иначе поток десинхронизируется
        // с первого же якоря (self-test 23:20: entry #0 mismatch).
        if (runTag.decode(dec, 0u) == kTag4Repeat) {
            // Прогон: тот же кортеж шагов к предыдущим состояниям.
            for (uint32_t s = 0; s < 3; ++s) {
                const bool dom31d = shake31 && s == 2u;
                for (uint32_t i = 0; i < prevK[s]; ++i)
                    prev[s] = dom31d ? shakeAdv31(prev[s]) : lcgAdvance(prev[s]);
            }
        } else {
            for (uint32_t s = 0; s < 3; ++s) {
                const bool dom31d = shake31 && s == 2u;
                const uint32_t t = tag.decode(dec, s);
                if (t == kTag3Same) {
                    prevK[s] = 0;
                } else if (t == kTag3Advance) {
                    const uint32_t k = kModel.decode(dec, kContextOf(s, prevK[s]));
                    for (uint32_t i = 0; i < k; ++i)
                        prev[s] = dom31d ? shakeAdv31(prev[s]) : lcgAdvance(prev[s]);
                    prevK[s] = k;
                } else if (t == kTag4Pin) {
                    // Шов: состояние = lcg^k(пин), НЕ продолжение prev.
                    // Контекст k-модели тот же, что у кодера: kContextOf(s,
                    // prevK[s]) - prevK здесь ещё держит k ПРЕДЫДУЩЕГО якоря.
                    const uint32_t k = kModel.decode(dec, kContextOf(s, prevK[s]));
                    prev[s] = dom31d ? kShakePinBase : kSkcLayerSeed;
                    for (uint32_t i = 0; i < k; ++i)
                        prev[s] = dom31d ? shakeAdv31(prev[s]) : lcgAdvance(prev[s]);
                    prevK[s] = k;
                } else {
                    // v5 shake31: raw shake - 31 бит вместо 64.
                    const uint32_t nbits = dom31d ? 31u : 64u;
                    uint64_t v = 0;
                    for (uint32_t i = 0; i < nbits; ++i)
                        v |= static_cast<uint64_t>(bits.decode(dec, s * 8u + (i & 7u))) << i;
                    prev[s] = v;
                    prevK[s] = 0;
                }
            }
        }
        entry.primary = prev[0];
        entry.teleport = prev[1];
        entry.shake = prev[2];
        out.push_back(entry);
    }
    return true;
}

uint32_t getU32(const uint8_t* d) {
    return static_cast<uint32_t>(d[0]) | (static_cast<uint32_t>(d[1]) << 8) |
           (static_cast<uint32_t>(d[2]) << 16) | (static_cast<uint32_t>(d[3]) << 24);
}

uint64_t getU64(const uint8_t* d) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | static_cast<uint64_t>(d[i]);
    return v;
}

// --- движок ----

// Курсор плейбэка по журналу.
size_t g_playCursor = 0;
// Метрики: сколько якорей применилось с фактическим восстановлением / сколько
// совпало по факту (второе — живое доказательство, что живой мир сам идёт по
// записанным цепочкам).
uint64_t g_rellocks = 0;
uint64_t g_microRelocks = 0;
bool g_macroLoadedCursorClean = false;
// Анонс числа якорей на старте плейбэка — один раз за процесс-play сессию.
bool g_journalAnnounceDone = false;

struct LiveTriple {
    uint64_t primary;
    uint64_t teleport;
    uint64_t shake;
};

LiveTriple captureLive() {
    const auto triple = skk::captureTripleRng();
    // v5: shake канонизируется до 31 бита (домен mod 2^31 - все будущие
    // 15-битные выводы идентичны, старшие биты никто не читает).
    return {triple.primary, triple.teleport,
            shakeCanon(skk::shakeStateGet())};
}

} // namespace

// --- хвост формата ---------------------------------------------------------

void appendRngJournal(std::vector<uint8_t>& bytes,
                      const std::vector<RngJournalEntry>& journal) {
    if (journal.empty())
        return;

    const size_t start = bytes.size();
    // Поля пишутся ЯВНО в порядке магия → reserved(0) → version → count.
    // (Прежний resize(+kTailFixed) + putU32 оставлял незаписанные нули на
    // +7..+10 и сдвигал version на +11 — расхождение читателя «version 0».)
    for (const uint8_t b : kTailMagic)
        bytes.push_back(b);
    putU32(bytes, 0u); // reserved (канонический ноль)
    putU32(bytes, kTailVersionV5);
    putU32(bytes, static_cast<uint32_t>(journal.size()));
    // Поток якорей v5: v4-машина (run-length + теги same/advance/raw/pin) +
    // канонизация shake до 31 бита (домен mod 2^31). Полный lossless:
    // декодер разворачивает прогоны в те же якоря бит-в-бит.
    encodeRngJournalBodyV4(bytes, journal, true);

    // Реальный размер хвоста = header(19) + фактическая длина сжатого
    // потока; считаем после записи.
    const size_t tailBytes = bytes.size() - start;
    skkbot2::log::info(
        "[SKC] rng journal: {} anchor(s) attached to the file "
        "(+{} B tail, v{}, raw {} B)",
        journal.size(), tailBytes, kTailVersionV5,
        kTailFixed + journal.size() * kEntryBytes);
}

// Канонический парсер хвоста (замер 07.11, 'Mk3_autosave' 4476 B /
// 'OKAK_autosave' 342 B):
//
//   magic(7) | u32 reserved(0) | u32 version | u32 count | entries * 28 B
//
// header = 19 Б, хвост = 19 + count·28 ровно. Писатель пишет reserved
// ЯВНО нулём; reader требует его ==0. Хвост должен заканчиваться ровно
// на конце окна (хвост в SKC — последняя секция файла).
bool parseRngJournal(const uint8_t* data, size_t size,
                     std::vector<RngJournalEntry>& out) {
    if (size < kTailFixed || std::memcmp(data, kTailMagic, sizeof(kTailMagic)) != 0)
        return false;
    const uint32_t reserved = getU32(data + sizeof(kTailMagic));
    const uint32_t version = getU32(data + sizeof(kTailMagic) + 4);
    const uint32_t count = getU32(data + sizeof(kTailMagic) + 8);
    if (version != kTailVersionV1 && version != kTailVersionV2 &&
        version != kTailVersionV3 && version != kTailVersionV4 &&
        version != kTailVersionV5) {
        skkbot2::log::warn(
            "[SKC] rng journal tail has version {} (expected 1..5) - "
            "journal dropped, inputs are still loaded",
            version);
        return false;
    }
    if (reserved != 0) {
        skkbot2::log::warn(
            "[SKC] rng journal tail reserved word = {} (expected 0) - "
            "journal dropped, inputs are still loaded",
            reserved);
        return false;
    }
    if (version == kTailVersionV1 &&
        static_cast<uint64_t>(count) * kEntryBytes + kTailFixed != size) {
        skkbot2::log::warn(
            "[SKC] rng journal tail size {} does not match {} anchor(s) "
            "({}) - journal dropped, inputs are still loaded",
            size, count, kTailFixed + static_cast<uint64_t>(count) * kEntryBytes);
        return false;
    }

    out.clear();
    out.reserve(count);
    if (version == kTailVersionV1) {
        const uint8_t* it = data + kTailFixed;
        for (uint32_t i = 0; i < count; ++i) {
            RngJournalEntry entry;
            entry.frame = getU32(it);
            entry.primary = getU64(it + 4);
            entry.teleport = getU64(it + 12);
            entry.shake = getU64(it + 20);
            out.push_back(entry);
            it += kEntryBytes;
        }
        return true;
    }
    if (version == kTailVersionV2) {
        if (!decodeRngJournalBody(data + kTailFixed, size - kTailFixed, count, out)) {
            skkbot2::log::warn(
                "[SKC] rng journal tail stream decode failed - journal dropped, "
                "inputs are still loaded");
            return false;
        }
        return true;
    }
    if (version == kTailVersionV3) {
        if (!decodeRngJournalBodyV3(data + kTailFixed, size - kTailFixed, count, out)) {
            skkbot2::log::warn(
                "[SKC] rng journal tail stream decode failed - journal dropped, "
                "inputs are still loaded");
            return false;
        }
        return true;
    }
    if (version == kTailVersionV5) {
        if (!decodeRngJournalBodyV4(data + kTailFixed, size - kTailFixed, count,
                                    out, true)) {
            skkbot2::log::warn(
                "[SKC] rng journal tail stream decode failed - journal dropped, "
                "inputs are still loaded");
            return false;
        }
        return true;
    }
    if (!decodeRngJournalBodyV4(data + kTailFixed, size - kTailFixed, count,
                                out, false)) {
        skkbot2::log::warn(
            "[SKC] rng journal tail stream decode failed - journal dropped, "
            "inputs are still loaded");
        return false;
    }
    return true;
}

// Журнал хранится ПОСЛЕ самозавершающегося range-потока, но граница потока
// «плывёт» на read-ahead байтах клиочного декодера: точечный memcmp на
// streamBytes промахивался МОЛЧА (18:04, 'Mk3') — и журнал дропался без
// единой строки в логе. Локатор: парс на границе окна → скан магии от
// конца среза → парс на найденном смещении → только тогда «not found».
// Все ветви печатаются в лог, тихий дроп невозможен.
bool locateAndParseRngJournal(const uint8_t* data, size_t size,
                              std::vector<RngJournalEntry>& out,
                              size_t back) {
    size_t offset = 0;
    if (parseRngJournal(data, size, out)) {
        skkbot2::log::info(
            "[SKC] rng journal tail loaded at window+{} ({}) - "
            "{} anchor(s)", offset,
            offset == back ? "exact tail head" : "read-ahead reserve",
            out.size());
        return true;
    }
    // Scan: last occurrence of the magic inside the window.
    bool found = false;
    for (size_t lo = 0; lo + sizeof(kTailMagic) <= size; ++lo) {
        if (std::memcmp(data + lo, kTailMagic, sizeof(kTailMagic)) != 0)
            continue;
        offset = lo;
        found = true;
    }
    if (!found) {
        skkbot2::log::info(
            "[SKC] rng journal tail not found in {} trailing byte(s) - "
            "file carries none (legacy/import/payload-only)", size);
        return false;
    }
    if (!parseRngJournal(data + offset, size - offset, out)) {
        skkbot2::log::warn(
            "[SKC] rng journal tail SKCRNG found at window+{} but the tail "
            "does not match the canonical layout ({} byte(s) left)",
            offset, size - offset);
        return false;
    }
    skkbot2::log::info(
        "[SKC] rng journal tail loaded after edge rescan (window+{}, "
        "declared boundary had {}-byte read-ahead slack) - {} anchor(s)",
        offset, back, out.size());
    return true;
}

// --- forceLayerSeeds -------------------------------------------------------

// SKK-порт (skkBot.hpp:2390) без activeSk: сид один и константный. Вызывается
// в resetLevel ПОСЛЕ ванильного тела, рядом с пином — как SKK, который гоняет
// forceLayerSeeds сразу после refreshRngState.
void forceLayerSeeds() {
    // Хук resetLevel общий для всей игры: вне SKC нельзя менять сиды SKK
    // или обычной попытки, независимо от порядка выполнения Geode-хуков.
    const auto& bot = Bot::get();
    if (!bot.isRecording() && !bot.isPlaying()) return;
    auto* playLayer = PlayLayer::get();
    if (!playLayer)
        return;

    playLayer->m_randomSeed = kSkcLayerSeed;
    playLayer->m_replayRandSeed = kSkcLayerSeed;
}

// --- движок ----------------------------------------------------------------

// v3 "урок Silicate" (сетка + только швы) ДИСКВАЛИФИЦИРОВАН живым прогоном
// 20:52–20:53 ('Mk3', 07.11): на RNG-тяжёлом уровне расход между якорями
// развивается быстрее, чем 240-кадровая сетка — смерть f=26047 при якорях
// 23040/26160 (2 600 кадров без точки сравнения). Политика возврат НА
// ПЛОТНЫЕ якоря: сетка + каждый тик ввода + структурный якорь после шва.
// Размер держит v2-сжатие (дельта+теги), не прореживание.
bool g_anchorPending = false;

void journalRecordTick(bool typedInput) {
    auto& bot = Bot::get();
    if (!bot.isRecording())
        return;

    const uint32_t frame = FrameClock::get().getFrame();
    const bool idleAnchor = (frame % kRngIdleInterval) == 0;
    if (!idleAnchor && !g_anchorPending && !typedInput)
        return;

    auto& journal = bot.macroData().rngJournal;
    const LiveTriple live = captureLive();
    if (!journal.empty() &&
        journal.back().frame != frame &&
        journal.back().primary == live.primary &&
        journal.back().teleport == live.teleport &&
        journal.back().shake == live.shake)
        return;
    // Отмот часов швом практики (BUG-xxx, живой кейс 23:07): каждый
    // CheckpointRestore возвращает FrameClock НАЗАД (715 -> 649), рестор на
    // тот же шов даёт frame == back.frame. Прежняя политика journal.clear()
    // стирала ВЕСЬ журнал на каждом шве - в файл уходил 1 якорь (+44 B tail),
    // и плейбэк ехал без RNG-синхронизации (дрейф -> смерть). Корректная
    // политика, как у инпутов: отрезанное будущее (frame > f) выпадает,
    // якорь на том же кадре перезаписывается, история до отмота остаётся -
    // прешовный путь финальной попытки совпадает с записанным бит-в-бит
    // (стичь реплеит те же инпуты), так что прешовные якоря валидны.
    if (!journal.empty() && frame <= journal.back().frame) {
        skkbot2::log::info(
            "[SKC] rng journal: clock rewind f={} <= {} - dropping the cut "
            "future, {} anchor(s) kept",
            frame, journal.back().frame,
            std::count_if(journal.begin(), journal.end(),
                          [&](const auto& e) { return e.frame <= frame; }));
        journal.erase(std::remove_if(journal.begin(), journal.end(),
                                     [&](const auto& e) {
                                         return e.frame > frame;
                                     }),
                      journal.end());
        if (!journal.empty() && journal.back().frame == frame)
            journal.pop_back(); // перезапись значения того же кадра ниже
    }

    journal.push_back({frame, live.primary, live.teleport, live.shake});
    const bool structural = g_anchorPending;
    g_anchorPending = false;
    if (journal.size() % 64 == 1 || structural) {
        skkbot2::log::debug(
            "[SKC] rng journal anchor #{} f={}{} primary={:#x} teleport={:#x} "
            "shake={:#x}",
            journal.size(), frame,
            structural ? " (structural, post-seam)" : "",
            live.primary, live.teleport, live.shake);
    }
}

void journalPlaybackTick() {
    auto& bot = Bot::get();
    if (!bot.isPlaying())
        return;

    const auto& journal = bot.macroData().rngJournal;
    if (journal.empty()) {
        if (!g_macroLoadedCursorClean) {
            skkbot2::log::info(
                "[SKC] rng journal: macro carries no journal - replay runs "
                "vanilla RNG streams (old file or imported macro)");
            g_macroLoadedCursorClean = true;
        }
        return;
    }
    if (!g_journalAnnounceDone) {
        g_journalAnnounceDone = true;
        skkbot2::log::info(
            "[SKC] rng journal: playing with {} anchor(s) - relocks will "
            "only be printed on mismatch (silent matches stay in counters)",
            journal.size());
    }

    const uint32_t frame = FrameClock::get().getFrame();
    while (g_playCursor < journal.size() && journal[g_playCursor].frame <= frame) {
        const auto& entry = journal[g_playCursor++];
        const LiveTriple live = captureLive();
        // Сравнение shake по канону: старые файлы (v1..v4) несут полные
        // 64-битные состояния - маскируем и живое, и якорное значение.
        if (live.primary == entry.primary && live.teleport == entry.teleport &&
            live.shake == shakeCanon(entry.shake)) {
            ++g_microRelocks;
            continue;
        }
        skk::restoreTripleRng({entry.primary, entry.teleport});
        skk::shakeStateSet(entry.shake);
        ++g_rellocks;
        skkbot2::log::info(
            "[SKC] rng relock f={} anchor f={} primary {:#x} -> {:#x} "
            "teleport {:#x} -> {:#x} shake {:#x} -> {:#x}",
            frame, entry.frame, live.primary, entry.primary, live.teleport,
            entry.teleport, live.shake, entry.shake);
    }
}

void journalPlaybackCounters(const char* reason) {
    auto& bot = Bot::get();
    const auto& journal = bot.macroData().rngJournal;
    if (journal.empty()) {
        skkbot2::log::info(
            "[SKC] rng journal: {} - macro carries no journal, counters "
            "empty", reason);
        return;
    }
    skkbot2::log::info(
        "[SKC] rng journal: {} - anchors={} walked={} rellocks={} "
        "silent-match={}", reason, journal.size(), g_playCursor, g_rellocks,
        g_microRelocks);
}

void journalResetCursor() {
    g_playCursor = 0;
    g_macroLoadedCursorClean = false;
    // Анонс — на каждую сессию плейбэка (загрузку макроса), не на каждую попытку.
    g_journalAnnounceDone = false;
}

void journalResetAttempt(uint32_t boundaryFrame, bool wipe) {
    auto& bot = Bot::get();
    if (bot.isRecording()) {
        auto& journal = bot.macroData().rngJournal;
        const size_t before = journal.size();
        if (wipe) {
            // Все якоря принадлежали мёртвой попытке.
            journal.clear();
        } else {
            // Стичь хранит префикс [0..boundary-1] (инпуты префикса kept),
            // значит якоря префикса - валидная история выжившего пути и
            // остаются. Выпадает МЁРТВОЕ БУДУЩЕЕ: якоря (boundary, кадр
            // смерти], записанные в погибшем сегменте - игрок с рестора
            // перепишет этот диапазон заново (BUG-129: предикат был вывернут
            // наоборот - стирал префикс, на каждом шве оставался 1 якорь,
            // и плейбэк ехал без RNG-синхронизации). Остатки будущего выше
            // нового якоря, если вдруг есть, подстраховно дорезает
            // rewind-логика journalRecordTick.
            journal.erase(std::remove_if(journal.begin(), journal.end(),
                                         [boundaryFrame](const auto& entry) {
                                             return entry.frame > boundaryFrame;
                                         }),
                          journal.end());
        }
        if (journal.size() != before) {
            skkbot2::log::info(
                "[SKC] rng journal: {} -> {} anchor(s) after {} at f={}",
                before, journal.size(), wipe ? "wipe" : "stitch", boundaryFrame);
        }
        // Структурный якорь ( урок Silicate): состояние принудительно
        // снимается на первом тике ПОСЛЕ бара/рестора — там GD гарантированно
        // возвращает сиды записанного мира (wipe = пин сида, stitch =
        // loadFromCheckpoint). До первого grid-якоря иначе могло пройти
        // до 240 кадров без точки сравнения.
        g_anchorPending = true;
    }

    g_playCursor = 0;
    g_macroLoadedCursorClean = false;
}



// Startup self-test (main.cpp, рядом со skc::selfTest): синтетический журнал,
// повторяющий живой профиль 'Mk3' - primary шагает LCG по 0..5 за якорь,
// teleport константен, shake через якорь, длинные стабильные прогоны
// (постоянный паттерн k), raw-выбросы и лёгкие ресиды. Проверяет: round-trip
// бит-в-бит (v3 И v5) и метрику сжатия v5 к сырым 28 Б и к v3.
bool selfTestRngJournal() {
    std::vector<RngJournalEntry> journal;
    uint64_t primary = 0x12345678ULL, teleport = 0xA5A5A5A5ULL, shake = 0x7FFFULL;
    uint32_t frame = 0;
    auto push = [&](uint32_t steps, bool shakeStep) {
        ++frame;
        for (uint32_t s = 0; s < steps; ++s) primary = lcgAdvance(primary);
        if (shakeStep) shake = lcgAdvance(shake);
        // v5: captureLive канонизирует shake - журнал всегда mod 2^31.
        journal.push_back({frame, primary, teleport, shakeCanon(shake)});
    };
    // 1) Переменный паттерн (вход/выход из зоны кликов): 256 якорей.
    for (uint32_t i = 0; i < 256; ++i) push((i * 7u) % 6u, i % 2 == 1);
    // 2) Длинный стабильный прогон: kP=2, kT=0, kS=0 на 400 кадров.
    for (uint32_t i = 0; i < 400; ++i) push(2u, false);
    // 3) Шовный профиль (живой кейс 01:29): все три потока прыгают к пин-
    // цепочке (ресид/стичь) - раньше это был raw x3 (~24 Б/якорь), теперь
    // тег pin + число шагов.
    {
        // Реальная база shake-цепочки: pinTripleSeedAtReset ставит
        // g_shakeState = (пин primary) & 0x7FFF = kShakePinBase.
        uint64_t p = kSkcLayerSeed, t = kSkcLayerSeed, sh = kShakePinBase;
        for (uint32_t j = 0; j < 7; ++j) {
            p = lcgAdvance(p);
            t = lcgAdvance(t);
            sh = shakeAdv31(sh);
        }
        primary = p;
        teleport = t;
        shake = shakeCanon(sh);
        ++frame;
        journal.push_back({frame, primary, teleport, shake});
        // После шва обычный ход: 40 кадров стабильного паттерна.
        for (uint32_t i = 0; i < 40; ++i) push(2u, false);
        // Ещё один шов.
        primary = lcgAdvance(kSkcLayerSeed);
        teleport = lcgAdvance(kSkcLayerSeed);
        shake = shakeCanon(shakeAdv31(kShakePinBase));
        ++frame;
        journal.push_back({frame, primary, teleport, shake});
        for (uint32_t i = 0; i < 40; ++i) push(2u, false);
    }
    // 4) Raw-выброс (ресид shake с ГАРБИДЖНЫМИ старшими битами - v5 обязателен
    // замаскировать их при захвате/кодировании) и ресид primary.
    {
        ++frame;
        shake = 0xDEADBEEFCAFEBABEULL;
        journal.push_back({frame, primary, teleport, shakeCanon(shake)});
        for (uint32_t i = 0; i < 64; ++i) push(2u, false);
        ++frame;
        primary = 0x2545F49101ULL;
        journal.push_back({frame, primary, teleport, shakeCanon(shake)});
    }
    // 5) Стабильный паттерн после ресида: ещё 300 кадров.
    for (uint32_t i = 0; i < 300; ++i) push(2u, false);
    // 6) Переменный хвост.
    for (uint32_t i = 0; i < 128; ++i) push((i * 5u) % 4u, i % 3 == 0);
    // Регрессия pin-repeat: одинаковое k не означает одинаковую базу.
    // После advance(1) pin(1) обязан остаться полным якорем.
    push(1u, false);
    primary = lcgAdvance(kSkcLayerSeed);
    journal.push_back({++frame, primary, teleport, shakeCanon(shake)});
    push(1u, false);

    const size_t raw = kTailFixed + journal.size() * kEntryBytes;

    // v4: round-trip + метрика.
    std::vector<uint8_t> bytes;
    appendRngJournal(bytes, journal);
    if (bytes.empty()) {
        skkbot2::log::error("[SKC] rng-journal self-test: nothing written");
        return false;
    }
    std::vector<RngJournalEntry> back;
    if (!locateAndParseRngJournal(bytes.data(), bytes.size(), back)) {
        skkbot2::log::error("[SKC] rng-journal self-test: tail not located");
        return false;
    }
    if (back.size() != journal.size()) {
        skkbot2::log::error("[SKC] rng-journal self-test: count {} != {}",
                            back.size(), journal.size());
        return false;
    }
    for (size_t i = 0; i < journal.size(); ++i) {
        if (!(back[i] == journal[i])) {
            skkbot2::log::error(
                "[SKC] rng-journal self-test: entry #{} mismatch "
                "(f={} vs {}, p={:#x} vs {:#x})",
                i, back[i].frame, journal[i].frame, back[i].primary,
                journal[i].primary);
            return false;
        }
    }

    // v3 для сравнения (та же тройка моделей, без прогонов).
    std::vector<uint8_t> v3;
    v3.insert(v3.end(), kTailMagic, kTailMagic + sizeof(kTailMagic));
    putU32(v3, 0u);
    putU32(v3, kTailVersionV3);
    putU32(v3, static_cast<uint32_t>(journal.size()));
    encodeRngJournalBodyV3(v3, journal);
    std::vector<RngJournalEntry> back3;
    if (!parseRngJournal(v3.data(), v3.size(), back3) || back3 != journal) {
        skkbot2::log::error("[SKC] rng-journal self-test: v3 round-trip failed");
        return false;
    }

    skkbot2::log::info(
        "[SKC] rng-journal self-test: ok ({} anchors | raw {} B | v3 {} B "
        "({:.2f} B/a) | v5 {} B ({:.2f} B/a, {:.1f}x vs v3, {:.0f}x vs raw))",
        journal.size(), raw, v3.size(),
        static_cast<double>(v3.size()) / journal.size(), bytes.size(),
        static_cast<double>(bytes.size()) / journal.size(),
        static_cast<double>(v3.size()) / static_cast<double>(bytes.size()),
        static_cast<double>(raw) / static_cast<double>(bytes.size()));
    return true;
}

} // namespace skk::skc
