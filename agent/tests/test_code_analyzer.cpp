// ---------------------------------------------------------------------------
// test_code_analyzer.cpp — Stage 6.22 §7
//
// Проверяет классификатор потоков управления и BFS анализатора на синтетике,
// где цель условного перехода лежит в ОТДЕЛЬНОМ блоке.  Именно этот случай
// молча терялся: маска (opcode & 0xC7) == 0xC0 подходила только под Rcc,
// поэтому JCC/CALLCC/RST попадали в Sequential и их цели не разбирались.
//
// Фикстуры — маленький образ RAM на 0x1000 байт; функции задаются байтами,
// чтобы тест не зависел от ROM-файлов.
// ---------------------------------------------------------------------------

#include "code_analyzer.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_testFailures = 0;
int g_testCount    = 0;

void check(bool condition, const std::string &testName, const std::string &msg)
{
    ++g_testCount;
    if (!condition) {
        ++g_testFailures;
        std::fprintf(stderr, "FAIL: %s — %s\n", testName.c_str(), msg.c_str());
    }
}

// --- образ памяти ----------------------------------------------------------

constexpr size_t kImageSize = 0x1000;

struct Image
{
    std::vector<uint8_t> bytes = std::vector<uint8_t>(kImageSize, 0x00);   // NOP

    void put(size_t addr, std::initializer_list<uint8_t> data)
    {
        size_t a = addr;
        for (uint8_t b : data) bytes[a++ % kImageSize] = b;
    }
};

// --- помощники над результатом ---------------------------------------------

bool hasInstrAt(const CodeAnalysisResult &r, uint16_t addr)
{
    for (const auto &i : r.instructions)
        if (i.address == addr) return true;
    return false;
}

bool hasRef(const CodeAnalysisResult &r, uint16_t from, uint16_t to,
            const char *type)
{
    for (const auto &ref : r.references)
        if (ref.from == from && ref.to == to && ref.type == type) return true;
    return false;
}

bool hasAnyRefFrom(const CodeAnalysisResult &r, uint16_t from)
{
    for (const auto &ref : r.references)
        if (ref.from == from) return true;
    return false;
}

bool rangeCovers(const CodeAnalysisResult &r, uint16_t addr)
{
    for (const auto &rg : r.ranges)
        if (addr >= rg.start && addr <= rg.end) return true;
    return false;
}

int rangeCount(const CodeAnalysisResult &r)
{
    return static_cast<int>(r.ranges.size());
}

// --- §7.1 классификатор ----------------------------------------------------

// Ожидания для всей страницы 0xC0..0xFF: bits 5-3 = условие, bits 2-0 = порода
// 000 Rcc / 010 Jcc / 100 Ccc / 111 RST, плюс безусловные C3/CD/C9 из той же
// страницы.  Прочие (001/003/005/006/007-не-RST...) — не ветвления.
void test_classifier_page_c0()
{
    const char *name = "classifier_page_c0";
    int wrong = 0;
    for (int op = 0xC0; op <= 0xFF; ++op) {
        ControlFlowType got = classifyControlFlow(static_cast<uint8_t>(op));
        ControlFlowType want;
        switch (op) {
        case 0xC3: want = ControlFlowType::UnconditionalJmp; break;
        case 0xCD: want = ControlFlowType::UnconditionalCall; break;
        case 0xC9: want = ControlFlowType::UnconditionalRet; break;
        case 0xE9: want = ControlFlowType::IndirectJump; break;   // PCHL
        default:
            switch (op & 0x07) {
            case 0x00: want = ControlFlowType::ConditionalRet; break;
            case 0x02: want = ControlFlowType::ConditionalJmp; break;
            case 0x04: want = ControlFlowType::ConditionalCall; break;
            case 0x07: want = ControlFlowType::Restart; break;
            default:   want = ControlFlowType::Sequential; break;
            }
            break;
        }
        if (got != want) {
            if (wrong < 8)
                std::fprintf(stderr, "  op %02X: got %d want %d\n",
                             op, static_cast<int>(got), static_cast<int>(want));
            ++wrong;
        }
    }
    check(wrong == 0, name, "часть страницы 0xC0..0xFF классифицирована неверно");
}

// Условные формы, которые должны попадать в свои классы (выборочно, по породе).
void test_classifier_representatives()
{
    const char *name = "classifier_representatives";
    check(classifyControlFlow(0xC2) == ControlFlowType::ConditionalJmp, name, "JNZ 0xC2");
    check(classifyControlFlow(0xDA) == ControlFlowType::ConditionalJmp, name, "JPC 0xDA");
    check(classifyControlFlow(0xFA) == ControlFlowType::ConditionalJmp, name, "JM 0xFA");
    check(classifyControlFlow(0xC4) == ControlFlowType::ConditionalCall, name, "CNZ 0xC4");
    check(classifyControlFlow(0xEC) == ControlFlowType::ConditionalCall, name, "CPE 0xEC");
    check(classifyControlFlow(0xFC) == ControlFlowType::ConditionalCall, name, "CM 0xFC");
    check(classifyControlFlow(0xC0) == ControlFlowType::ConditionalRet, name, "RNZ 0xC0");
    check(classifyControlFlow(0xF8) == ControlFlowType::ConditionalRet, name, "RM 0xF8");
    check(classifyControlFlow(0xC7) == ControlFlowType::Restart, name, "RST 0 0xC7");
    check(classifyControlFlow(0xFF) == ControlFlowType::Restart, name, "RST 7 0xFF");

    // Безусловные и «не ветвления» той же страницы
    check(classifyControlFlow(0xC3) == ControlFlowType::UnconditionalJmp, name, "JMP 0xC3");
    check(classifyControlFlow(0xCD) == ControlFlowType::UnconditionalCall, name, "CALL 0xCD");
    check(classifyControlFlow(0xC9) == ControlFlowType::UnconditionalRet, name, "RET 0xC9");
    check(classifyControlFlow(0xC1) == ControlFlowType::Sequential, name, "POP B 0xC1");
    check(classifyControlFlow(0xC5) == ControlFlowType::Sequential, name, "PUSH B 0xC5");
    check(classifyControlFlow(0xC6) == ControlFlowType::Sequential, name, "SUI 0xC6");
    check(classifyControlFlow(0xCE) == ControlFlowType::Sequential, name, "CPI 0xCE");
    check(classifyControlFlow(0xD3) == ControlFlowType::Sequential, name, "OUT 0xD3");
    check(classifyControlFlow(0xDD) == ControlFlowType::Sequential, name, "DD-префикс 0xDD");
    check(classifyControlFlow(0xF3) == ControlFlowType::Sequential, name, "DI 0xF3");

    // Страница 0x00..0xBF — вообще не ветвления (кроме обрабатываемых отдельно)
    check(classifyControlFlow(0x76) == ControlFlowType::Halt, name, "HLT 0x76");
    check(classifyControlFlow(0xE9) == ControlFlowType::IndirectJump, name, "PCHL 0xE9");
    check(classifyControlFlow(0x00) == ControlFlowType::Sequential, name, "NOP 0x00");
    check(classifyControlFlow(0x37) == ControlFlowType::Sequential, name, "CMC 0x37");

    // Дно: ни один opcode 0x00..0xBF не должен получить класс ветвления —
    // иначе маска «слишком широкая» и Ordinary-инструкции уходят в BFS как переходы
    int stray = 0;
    for (int op = 0x00; op <= 0xBF; ++op) {
        if (op == 0x76 || op == 0xE9) continue;
        if (classifyControlFlow(static_cast<uint8_t>(op)) != ControlFlowType::Sequential) {
            if (stray < 4) std::fprintf(stderr, "  stray %02X\n", op);
            ++stray;
        }
    }
    check(stray == 0, name, "нижняя страница дала класс ветвления");
}

// --- §7.2 JCC в несмежный блок ---------------------------------------------

void test_jcc_target_is_analyzed()
{
    const char *name = "jcc_target_block";
    Image img;
    // 0x0100: JPC 0x0200 ; 0x0103: RET
    img.put(0x0100, {0xDA, 0x00, 0x02});
    img.put(0x0103, {0xC9});
    // 0x0200: блок, до которого «обычным» разбором не дойти: предшествующая
    // инструкция заканчивается на 0x01FF, переход в него только условный.
    img.put(0x0200, {0x2F, 0xC9});   // CMA ; RET

    DisasmReadFn read = [&img](uint16_t a) { return img.bytes[a % kImageSize]; };
    CodeAnalysisResult r = analyzeCode(0x0100, read, 200);

    check(hasRef(r, 0x0100, 0x0200, "JCC"), name, "нет ссылки JCC 0x0100->0x0200");
    check(hasInstrAt(r, 0x0200), name, "блок 0x0200 не разобран");
    check(hasInstrAt(r, 0x0201), name, "после CMA в 0x0200 разбор оборвался");
    check(hasInstrAt(r, 0x0103), name, "потерян fall-through после JCC");
    check(rangeCovers(r, 0x0200), name, "0x0200 не попал ни в один range");
    check(rangeCount(r) == 2, name, "ожидалось два несмежных range");
}

// --- §7.3 CALLCC и RST -----------------------------------------------------

void test_callcc_and_rst_targets()
{
    const char *name = "callcc_rst_targets";
    Image img;
    img.put(0x0100, {0xCC, 0x00, 0x03});   // CZ 0x0300
    img.put(0x0103, {0xCF});               // RST 1 -> 0x0008
    img.put(0x0104, {0xC9});               // RET
    img.put(0x0008, {0x3E, 0x00, 0xC9});   // MVI A,0 ; RET
    img.put(0x0300, {0xC9});               // RET

    DisasmReadFn read = [&img](uint16_t a) { return img.bytes[a % kImageSize]; };
    CodeAnalysisResult r = analyzeCode(0x0100, read, 200);

    check(hasRef(r, 0x0100, 0x0300, "CALLCC"), name, "нет ссылки CALLCC");
    check(hasInstrAt(r, 0x0300), name, "цель CZ 0x0300 не разобрана");
    check(hasRef(r, 0x0103, 0x0008, "RST"), name, "нет ссылки RST");
    check(hasInstrAt(r, 0x0008), name, "страница RST 0x0008 не разобрана");
    check(hasInstrAt(r, 0x0104), name, "потерян fall-through после RST");
}

// --- регрессия: Rcc и безусловные формы не сломаны -------------------------

void test_rcc_and_unconditional_still_work()
{
    const char *name = "rcc_and_unconditional";
    Image img;
    img.put(0x0100, {0xC0});               // RNZ — only fall-through
    img.put(0x0101, {0xC3, 0x40, 0x01});   // JMP 0x0140
    img.put(0x0140, {0xCD, 0x60, 0x01});   // CALL 0x0160
    img.put(0x0143, {0xC9});               // RET
    img.put(0x0160, {0xE9});               // PCHL — цель статически неизвестна

    DisasmReadFn read = [&img](uint16_t a) { return img.bytes[a % kImageSize]; };
    CodeAnalysisResult r = analyzeCode(0x0100, read, 200);

    check(hasInstrAt(r, 0x0101), name, "потерян fall-through после RNZ");
    check(!hasAnyRefFrom(r, 0x0100), name, "RNZ дал ссылку перехода");
    check(hasRef(r, 0x0101, 0x0140, "JMP"), name, "нет ссылки JMP");
    check(hasRef(r, 0x0140, 0x0160, "CALL"), name, "нет ссылки CALL");
    check(hasInstrAt(r, 0x0160), name, "тело 0x0160 не разобрано");
    check(hasInstrAt(r, 0x0143), name, "потерян fall-through после CALL");
    // PCHL: цель статически неизвестна, ложных ссылок быть не должно
    check(!hasAnyRefFrom(r, 0x0160), name, "PCHL породил ссылку с выдуманной целью");
}

// --- многократный вход: цель JCC из второго сида ---------------------------

void test_multi_entry_jcc()
{
    const char *name = "multi_entry_jcc";
    Image img;
    img.put(0x0100, {0xE2, 0x80, 0x01});   // JPO 0x0180
    img.put(0x0103, {0xC9});
    img.put(0x0180, {0xC9});
    img.put(0x0400, {0xF2, 0x90, 0x04});   // JPM 0x0490
    img.put(0x0403, {0xC9});
    img.put(0x0490, {0xC9});

    DisasmReadFn read = [&img](uint16_t a) { return img.bytes[a % kImageSize]; };
    CodeAnalysisResult r = analyzeCodeMulti({0x0100, 0x0400}, read, 200);

    check(hasRef(r, 0x0100, 0x0180, "JCC"), name, "сид 0x0100: нет JCC");
    check(hasRef(r, 0x0400, 0x0490, "JCC"), name, "сид 0x0400: нет JCC");
    check(hasInstrAt(r, 0x0180) && hasInstrAt(r, 0x0490), name, "цели не разобраны");
    check(r.entryPoints.size() == 2, name, "потеряны точки входа");
}

} // namespace

int main()
{
    test_classifier_page_c0();
    test_classifier_representatives();
    test_jcc_target_is_analyzed();
    test_callcc_and_rst_targets();
    test_rcc_and_unconditional_still_work();
    test_multi_entry_jcc();

    std::fprintf(stderr, "[code_analyzer] %d/%d checks passed\n",
                 g_testCount - g_testFailures, g_testCount);
    return g_testFailures == 0 ? 0 : 1;
}
