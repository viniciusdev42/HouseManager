// =============================================================================
// Normalizer.hpp
//
// Normalização de texto UTF-8 para que textos semanticamente equivalentes
// tenham uma representação consistente, independentemente de capitalização,
// acentuação e formatação.
//
// Operações (cada uma é um passo independente do pipeline):
//   1. Normalização Unicode  (NFC, NFD, NFKC ou NFKD)
//   2. Case Folding          (versão "full" de CaseFolding.txt, status C + F)
//   3. Remoção de Diacríticos (NFD -> remove marcas combinantes -> recompõe)
//   4. Normalização de espaços/formatação (opcional, complementar)
//
// Características:
//   - Header-only, C++17, sem dependências externas (nem ICU).
//   - Thread-safe: após a construção, Normalizer é imutável; as tabelas
//     internas são inicializadas uma única vez (static local).
//
// Cobertura das tabelas internas:
//   As tabelas embutidas cobrem Latim (Latin-1, Extended-A, parte do
//   Extended-B e vietnamita), Grego, Cirílico (básico
//   e estendido), Armênio e Hangul (algorítmico). Para cobertura completa do
//   Unicode, implemente INormalizationStep sobre ICU e use-a no lugar dos
//   passos embutidos.
//
// Uso:
//   text::Normalizer normalizer;
//   std::string key = normalizer.normalize("  Ação   ÁGUA  ");  // "acao agua"
// =============================================================================
#ifndef STRING_NORMALIZER
#define STRING_NORMALIZER

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace text {

// -----------------------------------------------------------------------------
// Erros
// -----------------------------------------------------------------------------

/// Lançada quando a entrada não é UTF-8 válido (ou um code point é inválido).
/// Falhar explicitamente evita que bytes corrompidos produzam chaves de
/// comparação silenciosamente erradas.
class InvalidUtf8Error : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

// -----------------------------------------------------------------------------
// Codec UTF-8 <-> UTF-32
// -----------------------------------------------------------------------------
namespace utf8 {

inline std::u32string decode(std::string_view input) {
    std::u32string output;
    output.reserve(input.size());

    static constexpr char32_t kMinCodePointByLength[] = {0, 0, 0x80, 0x800, 0x10000};

    std::size_t i = 0;
    const std::size_t size = input.size();
    while (i < size) {
        const auto lead = static_cast<unsigned char>(input[i]);
        char32_t codePoint = 0;
        std::size_t length = 0;

        if (lead < 0x80) {
            codePoint = lead;
            length = 1;
        } else if (lead >= 0xC2 && lead <= 0xDF) {
            codePoint = lead & 0x1Fu;
            length = 2;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            codePoint = lead & 0x0Fu;
            length = 3;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            codePoint = lead & 0x07u;
            length = 4;
        } else {
            throw InvalidUtf8Error("UTF-8 inválido: byte inicial inesperado na posição " +
                                   std::to_string(i));
        }

        if (i + length > size) {
            throw InvalidUtf8Error("UTF-8 inválido: sequência truncada na posição " +
                                   std::to_string(i));
        }

        for (std::size_t k = 1; k < length; ++k) {
            const auto continuation = static_cast<unsigned char>(input[i + k]);
            if ((continuation & 0xC0u) != 0x80u) {
                throw InvalidUtf8Error("UTF-8 inválido: byte de continuação esperado na posição " +
                                       std::to_string(i + k));
            }
            codePoint = (codePoint << 6) | (continuation & 0x3Fu);
        }

        const bool overlong = codePoint < kMinCodePointByLength[length];
        const bool surrogate = codePoint >= 0xD800 && codePoint <= 0xDFFF;
        if (overlong || surrogate || codePoint > 0x10FFFF) {
            throw InvalidUtf8Error("UTF-8 inválido: code point fora do permitido na posição " +
                                   std::to_string(i));
        }

        output.push_back(codePoint);
        i += length;
    }
    return output;
}

inline std::string encode(std::u32string_view input) {
    std::string output;
    output.reserve(input.size());

    for (const char32_t cp : input) {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            throw InvalidUtf8Error("Code point Unicode inválido: " + std::to_string(static_cast<std::uint32_t>(cp)));
        }
        if (cp < 0x80) {
            output.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            output.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            output.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            output.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            output.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return output;
}

}  // namespace utf8

// -----------------------------------------------------------------------------
// Dados Unicode (detalhe de implementação)
// -----------------------------------------------------------------------------
namespace detail {

constexpr bool inRange(char32_t c, char32_t low, char32_t high) noexcept {
    return c >= low && c <= high;
}

// ---- Hangul (decomposição/composição algorítmica) ---------------------------
inline constexpr char32_t kSBase = 0xAC00;
inline constexpr char32_t kLBase = 0x1100;
inline constexpr char32_t kVBase = 0x1161;
inline constexpr char32_t kTBase = 0x11A7;
inline constexpr char32_t kLCount = 19;
inline constexpr char32_t kVCount = 21;
inline constexpr char32_t kTCount = 28;
inline constexpr char32_t kNCount = kVCount * kTCount;
inline constexpr char32_t kSCount = kLCount * kNCount;

// ---- Decomposições canônicas (um nível; a recursão resolve o restante) ------
struct CanonicalDecomposition {
    char32_t composed;
    char32_t first;
    char32_t second;
};

inline constexpr CanonicalDecomposition kCanonicalDecompositions[] = {
    // Latin-1 Supplement (maiúsculas)
    {0x00C0, 0x0041, 0x0300}, {0x00C1, 0x0041, 0x0301}, {0x00C2, 0x0041, 0x0302},
    {0x00C3, 0x0041, 0x0303}, {0x00C4, 0x0041, 0x0308}, {0x00C5, 0x0041, 0x030A},
    {0x00C7, 0x0043, 0x0327}, {0x00C8, 0x0045, 0x0300}, {0x00C9, 0x0045, 0x0301},
    {0x00CA, 0x0045, 0x0302}, {0x00CB, 0x0045, 0x0308}, {0x00CC, 0x0049, 0x0300},
    {0x00CD, 0x0049, 0x0301}, {0x00CE, 0x0049, 0x0302}, {0x00CF, 0x0049, 0x0308},
    {0x00D1, 0x004E, 0x0303}, {0x00D2, 0x004F, 0x0300}, {0x00D3, 0x004F, 0x0301},
    {0x00D4, 0x004F, 0x0302}, {0x00D5, 0x004F, 0x0303}, {0x00D6, 0x004F, 0x0308},
    {0x00D9, 0x0055, 0x0300}, {0x00DA, 0x0055, 0x0301}, {0x00DB, 0x0055, 0x0302},
    {0x00DC, 0x0055, 0x0308}, {0x00DD, 0x0059, 0x0301},
    // Latin-1 Supplement (minúsculas)
    {0x00E0, 0x0061, 0x0300}, {0x00E1, 0x0061, 0x0301}, {0x00E2, 0x0061, 0x0302},
    {0x00E3, 0x0061, 0x0303}, {0x00E4, 0x0061, 0x0308}, {0x00E5, 0x0061, 0x030A},
    {0x00E7, 0x0063, 0x0327}, {0x00E8, 0x0065, 0x0300}, {0x00E9, 0x0065, 0x0301},
    {0x00EA, 0x0065, 0x0302}, {0x00EB, 0x0065, 0x0308}, {0x00EC, 0x0069, 0x0300},
    {0x00ED, 0x0069, 0x0301}, {0x00EE, 0x0069, 0x0302}, {0x00EF, 0x0069, 0x0308},
    {0x00F1, 0x006E, 0x0303}, {0x00F2, 0x006F, 0x0300}, {0x00F3, 0x006F, 0x0301},
    {0x00F4, 0x006F, 0x0302}, {0x00F5, 0x006F, 0x0303}, {0x00F6, 0x006F, 0x0308},
    {0x00F9, 0x0075, 0x0300}, {0x00FA, 0x0075, 0x0301}, {0x00FB, 0x0075, 0x0302},
    {0x00FC, 0x0075, 0x0308}, {0x00FD, 0x0079, 0x0301}, {0x00FF, 0x0079, 0x0308},

    // Latin Extended-A
    {0x0100, 0x0041, 0x0304}, {0x0101, 0x0061, 0x0304}, {0x0102, 0x0041, 0x0306},
    {0x0103, 0x0061, 0x0306}, {0x0104, 0x0041, 0x0328}, {0x0105, 0x0061, 0x0328},
    {0x0106, 0x0043, 0x0301}, {0x0107, 0x0063, 0x0301}, {0x0108, 0x0043, 0x0302},
    {0x0109, 0x0063, 0x0302}, {0x010A, 0x0043, 0x0307}, {0x010B, 0x0063, 0x0307},
    {0x010C, 0x0043, 0x030C}, {0x010D, 0x0063, 0x030C}, {0x010E, 0x0044, 0x030C},
    {0x010F, 0x0064, 0x030C}, {0x0112, 0x0045, 0x0304}, {0x0113, 0x0065, 0x0304},
    {0x0114, 0x0045, 0x0306}, {0x0115, 0x0065, 0x0306}, {0x0116, 0x0045, 0x0307},
    {0x0117, 0x0065, 0x0307}, {0x0118, 0x0045, 0x0328}, {0x0119, 0x0065, 0x0328},
    {0x011A, 0x0045, 0x030C}, {0x011B, 0x0065, 0x030C}, {0x011C, 0x0047, 0x0302},
    {0x011D, 0x0067, 0x0302}, {0x011E, 0x0047, 0x0306}, {0x011F, 0x0067, 0x0306},
    {0x0120, 0x0047, 0x0307}, {0x0121, 0x0067, 0x0307}, {0x0122, 0x0047, 0x0327},
    {0x0123, 0x0067, 0x0327}, {0x0124, 0x0048, 0x0302}, {0x0125, 0x0068, 0x0302},
    {0x0128, 0x0049, 0x0303}, {0x0129, 0x0069, 0x0303}, {0x012A, 0x0049, 0x0304},
    {0x012B, 0x0069, 0x0304}, {0x012C, 0x0049, 0x0306}, {0x012D, 0x0069, 0x0306},
    {0x012E, 0x0049, 0x0328}, {0x012F, 0x0069, 0x0328}, {0x0130, 0x0049, 0x0307},
    {0x0134, 0x004A, 0x0302}, {0x0135, 0x006A, 0x0302}, {0x0136, 0x004B, 0x0327},
    {0x0137, 0x006B, 0x0327}, {0x0139, 0x004C, 0x0301}, {0x013A, 0x006C, 0x0301},
    {0x013B, 0x004C, 0x0327}, {0x013C, 0x006C, 0x0327}, {0x013D, 0x004C, 0x030C},
    {0x013E, 0x006C, 0x030C}, {0x0143, 0x004E, 0x0301}, {0x0144, 0x006E, 0x0301},
    {0x0145, 0x004E, 0x0327}, {0x0146, 0x006E, 0x0327}, {0x0147, 0x004E, 0x030C},
    {0x0148, 0x006E, 0x030C}, {0x014C, 0x004F, 0x0304}, {0x014D, 0x006F, 0x0304},
    {0x014E, 0x004F, 0x0306}, {0x014F, 0x006F, 0x0306}, {0x0150, 0x004F, 0x030B},
    {0x0151, 0x006F, 0x030B}, {0x0154, 0x0052, 0x0301}, {0x0155, 0x0072, 0x0301},
    {0x0156, 0x0052, 0x0327}, {0x0157, 0x0072, 0x0327}, {0x0158, 0x0052, 0x030C},
    {0x0159, 0x0072, 0x030C}, {0x015A, 0x0053, 0x0301}, {0x015B, 0x0073, 0x0301},
    {0x015C, 0x0053, 0x0302}, {0x015D, 0x0073, 0x0302}, {0x015E, 0x0053, 0x0327},
    {0x015F, 0x0073, 0x0327}, {0x0160, 0x0053, 0x030C}, {0x0161, 0x0073, 0x030C},
    {0x0162, 0x0054, 0x0327}, {0x0163, 0x0074, 0x0327}, {0x0164, 0x0054, 0x030C},
    {0x0165, 0x0074, 0x030C}, {0x0168, 0x0055, 0x0303}, {0x0169, 0x0075, 0x0303},
    {0x016A, 0x0055, 0x0304}, {0x016B, 0x0075, 0x0304}, {0x016C, 0x0055, 0x0306},
    {0x016D, 0x0075, 0x0306}, {0x016E, 0x0055, 0x030A}, {0x016F, 0x0075, 0x030A},
    {0x0170, 0x0055, 0x030B}, {0x0171, 0x0075, 0x030B}, {0x0172, 0x0055, 0x0328},
    {0x0173, 0x0075, 0x0328}, {0x0174, 0x0057, 0x0302}, {0x0175, 0x0077, 0x0302},
    {0x0176, 0x0059, 0x0302}, {0x0177, 0x0079, 0x0302}, {0x0178, 0x0059, 0x0308},
    {0x0179, 0x005A, 0x0301}, {0x017A, 0x007A, 0x0301}, {0x017B, 0x005A, 0x0307},
    {0x017C, 0x007A, 0x0307}, {0x017D, 0x005A, 0x030C}, {0x017E, 0x007A, 0x030C},

    // Latin Extended-B (pinyin, romeno e algumas letras comuns)
    {0x01CD, 0x0041, 0x030C}, {0x01CE, 0x0061, 0x030C}, {0x01CF, 0x0049, 0x030C},
    {0x01D0, 0x0069, 0x030C}, {0x01D1, 0x004F, 0x030C}, {0x01D2, 0x006F, 0x030C},
    {0x01D3, 0x0055, 0x030C}, {0x01D4, 0x0075, 0x030C}, {0x01D5, 0x00DC, 0x0304},
    {0x01D6, 0x00FC, 0x0304}, {0x01D7, 0x00DC, 0x0301}, {0x01D8, 0x00FC, 0x0301},
    {0x01D9, 0x00DC, 0x030C}, {0x01DA, 0x00FC, 0x030C}, {0x01DB, 0x00DC, 0x0300},
    {0x01DC, 0x00FC, 0x0300}, {0x01E6, 0x0047, 0x030C}, {0x01E7, 0x0067, 0x030C},
    {0x01E8, 0x004B, 0x030C}, {0x01E9, 0x006B, 0x030C}, {0x01EA, 0x004F, 0x0328},
    {0x01EB, 0x006F, 0x0328}, {0x01F4, 0x0047, 0x0301}, {0x01F5, 0x0067, 0x0301},
    {0x01F8, 0x004E, 0x0300}, {0x01F9, 0x006E, 0x0300}, {0x0218, 0x0053, 0x0326},
    {0x0219, 0x0073, 0x0326}, {0x021A, 0x0054, 0x0326}, {0x021B, 0x0074, 0x0326},

    // Vietnamita: Ơ/Ư (chifre) e Latin Extended Additional
    {0x01A0, 0x004F, 0x031B}, {0x01A1, 0x006F, 0x031B}, {0x01AF, 0x0055, 0x031B},
    {0x01B0, 0x0075, 0x031B},
    {0x1EA0, 0x0041, 0x0323}, {0x1EA1, 0x0061, 0x0323}, {0x1EA2, 0x0041, 0x0309},
    {0x1EA3, 0x0061, 0x0309}, {0x1EA4, 0x00C2, 0x0301}, {0x1EA5, 0x00E2, 0x0301},
    {0x1EA6, 0x00C2, 0x0300}, {0x1EA7, 0x00E2, 0x0300}, {0x1EA8, 0x00C2, 0x0309},
    {0x1EA9, 0x00E2, 0x0309}, {0x1EAA, 0x00C2, 0x0303}, {0x1EAB, 0x00E2, 0x0303},
    {0x1EAC, 0x1EA0, 0x0302}, {0x1EAD, 0x1EA1, 0x0302}, {0x1EAE, 0x0102, 0x0301},
    {0x1EAF, 0x0103, 0x0301}, {0x1EB0, 0x0102, 0x0300}, {0x1EB1, 0x0103, 0x0300},
    {0x1EB2, 0x0102, 0x0309}, {0x1EB3, 0x0103, 0x0309}, {0x1EB4, 0x0102, 0x0303},
    {0x1EB5, 0x0103, 0x0303}, {0x1EB6, 0x1EA0, 0x0306}, {0x1EB7, 0x1EA1, 0x0306},
    {0x1EB8, 0x0045, 0x0323}, {0x1EB9, 0x0065, 0x0323}, {0x1EBA, 0x0045, 0x0309},
    {0x1EBB, 0x0065, 0x0309}, {0x1EBC, 0x0045, 0x0303}, {0x1EBD, 0x0065, 0x0303},
    {0x1EBE, 0x00CA, 0x0301}, {0x1EBF, 0x00EA, 0x0301}, {0x1EC0, 0x00CA, 0x0300},
    {0x1EC1, 0x00EA, 0x0300}, {0x1EC2, 0x00CA, 0x0309}, {0x1EC3, 0x00EA, 0x0309},
    {0x1EC4, 0x00CA, 0x0303}, {0x1EC5, 0x00EA, 0x0303}, {0x1EC6, 0x1EB8, 0x0302},
    {0x1EC7, 0x1EB9, 0x0302}, {0x1EC8, 0x0049, 0x0309}, {0x1EC9, 0x0069, 0x0309},
    {0x1ECA, 0x0049, 0x0323}, {0x1ECB, 0x0069, 0x0323}, {0x1ECC, 0x004F, 0x0323},
    {0x1ECD, 0x006F, 0x0323}, {0x1ECE, 0x004F, 0x0309}, {0x1ECF, 0x006F, 0x0309},
    {0x1ED0, 0x00D4, 0x0301}, {0x1ED1, 0x00F4, 0x0301}, {0x1ED2, 0x00D4, 0x0300},
    {0x1ED3, 0x00F4, 0x0300}, {0x1ED4, 0x00D4, 0x0309}, {0x1ED5, 0x00F4, 0x0309},
    {0x1ED6, 0x00D4, 0x0303}, {0x1ED7, 0x00F4, 0x0303}, {0x1ED8, 0x1ECC, 0x0302},
    {0x1ED9, 0x1ECD, 0x0302}, {0x1EDA, 0x01A0, 0x0301}, {0x1EDB, 0x01A1, 0x0301},
    {0x1EDC, 0x01A0, 0x0300}, {0x1EDD, 0x01A1, 0x0300}, {0x1EDE, 0x01A0, 0x0309},
    {0x1EDF, 0x01A1, 0x0309}, {0x1EE0, 0x01A0, 0x0303}, {0x1EE1, 0x01A1, 0x0303},
    {0x1EE2, 0x01A0, 0x0323}, {0x1EE3, 0x01A1, 0x0323}, {0x1EE4, 0x0055, 0x0323},
    {0x1EE5, 0x0075, 0x0323}, {0x1EE6, 0x0055, 0x0309}, {0x1EE7, 0x0075, 0x0309},
    {0x1EE8, 0x01AF, 0x0301}, {0x1EE9, 0x01B0, 0x0301}, {0x1EEA, 0x01AF, 0x0300},
    {0x1EEB, 0x01B0, 0x0300}, {0x1EEC, 0x01AF, 0x0309}, {0x1EED, 0x01B0, 0x0309},
    {0x1EEE, 0x01AF, 0x0303}, {0x1EEF, 0x01B0, 0x0303}, {0x1EF0, 0x01AF, 0x0323},
    {0x1EF1, 0x01B0, 0x0323}, {0x1EF2, 0x0059, 0x0300}, {0x1EF3, 0x0079, 0x0300},
    {0x1EF4, 0x0059, 0x0323}, {0x1EF5, 0x0079, 0x0323}, {0x1EF6, 0x0059, 0x0309},
    {0x1EF7, 0x0079, 0x0309}, {0x1EF8, 0x0059, 0x0303}, {0x1EF9, 0x0079, 0x0303},

    // Grego
    {0x0386, 0x0391, 0x0301}, {0x0388, 0x0395, 0x0301}, {0x0389, 0x0397, 0x0301},
    {0x038A, 0x0399, 0x0301}, {0x038C, 0x039F, 0x0301}, {0x038E, 0x03A5, 0x0301},
    {0x038F, 0x03A9, 0x0301}, {0x0390, 0x03CA, 0x0301}, {0x03AA, 0x0399, 0x0308},
    {0x03AB, 0x03A5, 0x0308}, {0x03AC, 0x03B1, 0x0301}, {0x03AD, 0x03B5, 0x0301},
    {0x03AE, 0x03B7, 0x0301}, {0x03AF, 0x03B9, 0x0301}, {0x03B0, 0x03CB, 0x0301},
    {0x03CA, 0x03B9, 0x0308}, {0x03CB, 0x03C5, 0x0308}, {0x03CC, 0x03BF, 0x0301},
    {0x03CD, 0x03C5, 0x0301}, {0x03CE, 0x03C9, 0x0301},

    // Cirílico
    {0x0401, 0x0415, 0x0308}, {0x0419, 0x0418, 0x0306}, {0x0439, 0x0438, 0x0306},
    {0x0451, 0x0435, 0x0308},
};

// ---- Decomposições de compatibilidade (usadas apenas em NFKC/NFKD) ----------
struct CompatibilityMapping {
    char32_t source;
    std::u32string_view target;
};

inline constexpr CompatibilityMapping kCompatibilityMappings[] = {
    {0x00A0, U" "},   {0x00AA, U"a"},   {0x00B2, U"2"},   {0x00B3, U"3"},
    {0x00B5, U"\u03BC"}, {0x00B9, U"1"}, {0x00BA, U"o"},
    {0x2002, U" "},   {0x2003, U" "},   {0x2004, U" "},   {0x2005, U" "},
    {0x2006, U" "},   {0x2007, U" "},   {0x2008, U" "},   {0x2009, U" "},
    {0x200A, U" "},   {0x202F, U" "},   {0x205F, U" "},   {0x3000, U" "},
    {0x2026, U"..."}, {0x2122, U"TM"},
    {0xFB00, U"ff"},  {0xFB01, U"fi"},  {0xFB02, U"fl"},  {0xFB03, U"ffi"},
    {0xFB04, U"ffl"},
};

// Formas de largura total (FF01..FF5E) mapeiam linearmente para ASCII (21..7E).
inline constexpr char32_t kFullwidthFirst = 0xFF01;
inline constexpr char32_t kFullwidthLast = 0xFF5E;
inline constexpr char32_t kFullwidthOffset = 0xFF01 - 0x0021;

// ---- Tabelas de consulta (inicializadas uma única vez, thread-safe) ---------
inline const std::unordered_map<char32_t, std::pair<char32_t, char32_t>>& decompositionMap() {
    static const auto map = [] {
        std::unordered_map<char32_t, std::pair<char32_t, char32_t>> m;
        for (const auto& entry : kCanonicalDecompositions) {
            m.emplace(entry.composed, std::make_pair(entry.first, entry.second));
        }
        return m;
    }();
    return map;
}

inline const std::unordered_map<std::uint64_t, char32_t>& compositionMap() {
    static const auto map = [] {
        std::unordered_map<std::uint64_t, char32_t> m;
        for (const auto& entry : kCanonicalDecompositions) {
            // O ponto de código U+0130 (İ) é uma decomposição canônica válida
            // (I + U+0307) e também é recomposto; nenhuma exclusão de composição
            // se aplica às entradas desta tabela.
            const std::uint64_t key = (static_cast<std::uint64_t>(entry.first) << 32) | entry.second;
            m.emplace(key, entry.composed);
        }
        return m;
    }();
    return map;
}

inline const std::unordered_map<char32_t, std::u32string_view>& compatibilityMap() {
    static const auto map = [] {
        std::unordered_map<char32_t, std::u32string_view> m;
        for (const auto& entry : kCompatibilityMappings) {
            m.emplace(entry.source, entry.target);
        }
        return m;
    }();
    return map;
}

// ---- Combining class (ccc) e marcas combinantes -----------------------------

/// Canonical Combining Class para o bloco Combining Diacritical Marks
/// (U+0300..U+036F). Outros blocos retornam 0 (tratados como "starters"), o
/// que é suficiente para as tabelas embutidas, pois nenhuma delas os compõe.
inline int combiningClass(char32_t c) noexcept {
    if (c < 0x0300 || c > 0x036F) return 0;
    if (c <= 0x0314) return 230;
    if (c == 0x0315) return 232;
    if (c <= 0x0319) return 220;
    if (c == 0x031A) return 232;
    if (c == 0x031B) return 216;
    if (c <= 0x0320) return 220;
    if (c <= 0x0322) return 202;
    if (c <= 0x0326) return 220;
    if (c <= 0x0328) return 202;
    if (c <= 0x0333) return 220;
    if (c <= 0x0338) return 1;
    if (c <= 0x033C) return 220;
    if (c <= 0x0344) return 230;
    if (c == 0x0345) return 240;
    if (c == 0x0346) return 230;
    if (c <= 0x0349) return 220;
    if (c <= 0x034C) return 230;
    if (c <= 0x034E) return 220;
    if (c == 0x034F) return 0;
    if (c <= 0x0352) return 230;
    if (c <= 0x0356) return 220;
    if (c == 0x0357) return 230;
    if (c == 0x0358) return 232;
    if (c <= 0x035A) return 220;
    if (c == 0x035B) return 230;
    if (c == 0x035C) return 233;
    if (c <= 0x035E) return 234;
    if (c == 0x035F) return 233;
    if (c <= 0x0361) return 234;
    if (c == 0x0362) return 233;
    return 230;
}

/// Marcas combinantes de diacríticos (não inclui sinais vocálicos de escritas
/// índicas etc., que fazem parte da letra e não devem ser removidos).
inline bool isDiacriticMark(char32_t c) noexcept {
    return inRange(c, 0x0300, 0x036F) ||  // Combining Diacritical Marks
           inRange(c, 0x1AB0, 0x1AFF) ||  // Combining Diacritical Marks Extended
           inRange(c, 0x1DC0, 0x1DFF) ||  // Combining Diacritical Marks Supplement
           inRange(c, 0x20D0, 0x20FF) ||  // Combining Diacritical Marks for Symbols
           inRange(c, 0xFE20, 0xFE2F);    // Combining Half Marks
}

// ---- Decomposição / reordenação / composição --------------------------------

inline void decomposeInto(char32_t c, bool compatibility, std::u32string& out) {
    // Hangul: algorítmico.
    if (c >= kSBase && c < kSBase + kSCount) {
        const char32_t index = c - kSBase;
        out.push_back(kLBase + index / kNCount);
        out.push_back(kVBase + (index % kNCount) / kTCount);
        const char32_t trailing = index % kTCount;
        if (trailing != 0) out.push_back(kTBase + trailing);
        return;
    }

    if (compatibility) {
        if (inRange(c, kFullwidthFirst, kFullwidthLast)) {
            out.push_back(c - kFullwidthOffset);
            return;
        }
        const auto& compat = compatibilityMap();
        if (const auto it = compat.find(c); it != compat.end()) {
            for (const char32_t target : it->second) decomposeInto(target, compatibility, out);
            return;
        }
    }

    const auto& canonical = decompositionMap();
    if (const auto it = canonical.find(c); it != canonical.end()) {
        decomposeInto(it->second.first, compatibility, out);
        decomposeInto(it->second.second, compatibility, out);
        return;
    }

    out.push_back(c);
}

/// Ordena cada sequência de marcas combinantes por combining class (estável),
/// requisito para que NFD/NFC sejam únicos.
inline void canonicalReorder(std::u32string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        if (combiningClass(text[i]) == 0) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < text.size() && combiningClass(text[j]) != 0) ++j;
        std::stable_sort(text.begin() + static_cast<std::ptrdiff_t>(i),
                         text.begin() + static_cast<std::ptrdiff_t>(j),
                         [](char32_t a, char32_t b) { return combiningClass(a) < combiningClass(b); });
        i = j;
    }
}

inline std::u32string decompose(std::u32string_view input, bool compatibility) {
    std::u32string output;
    output.reserve(input.size() + input.size() / 4);
    for (const char32_t c : input) decomposeInto(c, compatibility, output);
    canonicalReorder(output);
    return output;
}

/// Compõe um par (starter, próximo) em um único code point, se existir.
inline bool composePair(char32_t first, char32_t second, char32_t& composed) {
    // Hangul L + V -> LV
    if (first >= kLBase && first < kLBase + kLCount && second >= kVBase && second < kVBase + kVCount) {
        composed = kSBase + ((first - kLBase) * kVCount + (second - kVBase)) * kTCount;
        return true;
    }
    // Hangul LV + T -> LVT
    if (first >= kSBase && first < kSBase + kSCount && (first - kSBase) % kTCount == 0 &&
        second > kTBase && second < kTBase + kTCount) {
        composed = first + (second - kTBase);
        return true;
    }
    const auto& map = compositionMap();
    const std::uint64_t key = (static_cast<std::uint64_t>(first) << 32) | second;
    if (const auto it = map.find(key); it != map.end()) {
        composed = it->second;
        return true;
    }
    return false;
}

/// Algoritmo de composição canônica (UAX #15), assumindo entrada decomposta e
/// já reordenada.
inline std::u32string compose(std::u32string_view input) {
    std::u32string result;
    result.reserve(input.size());

    constexpr std::size_t kNoStarter = static_cast<std::size_t>(-1);
    std::size_t starterIndex = kNoStarter;
    int lastClass = 0;  // ccc do último caractere não composto após o starter

    for (const char32_t c : input) {
        const int currentClass = combiningClass(c);

        if (starterIndex != kNoStarter) {
            const bool adjacent = (result.size() - 1 == starterIndex);
            // Bloqueado se houver caractere intermediário com ccc >= ccc atual.
            if (adjacent || lastClass < currentClass) {
                char32_t composed = 0;
                if (composePair(result[starterIndex], c, composed)) {
                    result[starterIndex] = composed;
                    continue;
                }
            }
        }

        if (currentClass == 0) {
            starterIndex = result.size();
            lastClass = 0;
        } else {
            lastClass = currentClass;
        }
        result.push_back(c);
    }
    return result;
}

// ---- Case folding -----------------------------------------------------------

/// Case folding "full" (CaseFolding.txt, status C + F). Diferente de tolower,
/// é pensado para comparação: "ß" -> "ss", "ς" -> "σ", "ſ" -> "s", etc.
inline void appendCaseFolded(char32_t c, std::u32string& out) {
    if (c < 0x80) {
        out.push_back((c >= U'A' && c <= U'Z') ? c + 0x20 : c);
        return;
    }

    switch (c) {
        case 0x00B5: out.push_back(0x03BC); return;           // µ -> μ
        case 0x00DF: out.append(U"ss"); return;               // ß
        case 0x0130: out.append(U"i\u0307"); return;          // İ
        case 0x0149: out.append(U"\u02BCn"); return;          // ŉ
        case 0x0178: out.push_back(0x00FF); return;           // Ÿ
        case 0x017F: out.push_back(U's'); return;             // ſ
        case 0x01AF: out.push_back(0x01B0); return;           // Ư
        case 0x01F4: out.push_back(0x01F5); return;           // Ǵ
        case 0x0386: out.push_back(0x03AC); return;           // Ά
        case 0x038C: out.push_back(0x03CC); return;           // Ό
        case 0x03C2: out.push_back(0x03C3); return;           // ς (sigma final)
        case 0x04C0: out.push_back(0x04CF); return;           // Ӏ
        case 0x1E9B: out.push_back(0x1E61); return;           // ẛ
        case 0x1E9E: out.append(U"ss"); return;               // ẞ
        default: break;
    }

    // Latin-1: À..Þ (exceto ×)
    if (inRange(c, 0x00C0, 0x00DE) && c != 0x00D7) { out.push_back(c + 0x20); return; }

    // Blocos em que a maiúscula é o code point par e a minúscula é o seguinte.
    if (inRange(c, 0x0100, 0x012F) || inRange(c, 0x0132, 0x0137) || inRange(c, 0x014A, 0x0177) ||
        inRange(c, 0x01A0, 0x01A5) ||
        inRange(c, 0x01DE, 0x01EF) || inRange(c, 0x01F8, 0x021F) || inRange(c, 0x0460, 0x0481) ||
        inRange(c, 0x048A, 0x04BF) || inRange(c, 0x04D0, 0x052F) || inRange(c, 0x1E00, 0x1E95) ||
        inRange(c, 0x1EA0, 0x1EFF)) {
        out.push_back((c & 1u) == 0 ? c + 1 : c);
        return;
    }

    // Blocos em que a maiúscula é o code point ímpar e a minúscula é o seguinte.
    if (inRange(c, 0x0139, 0x0148) || inRange(c, 0x0179, 0x017E) || inRange(c, 0x01CD, 0x01DC) ||
        inRange(c, 0x04C1, 0x04CE)) {
        out.push_back((c & 1u) != 0 ? c + 1 : c);
        return;
    }

    // Grego
    if (inRange(c, 0x0391, 0x03A1) || inRange(c, 0x03A3, 0x03AB)) { out.push_back(c + 0x20); return; }
    if (inRange(c, 0x0388, 0x038A)) { out.push_back(c + 0x25); return; }
    if (inRange(c, 0x038E, 0x038F)) { out.push_back(c + 0x3F); return; }

    // Cirílico
    if (inRange(c, 0x0400, 0x040F)) { out.push_back(c + 0x50); return; }
    if (inRange(c, 0x0410, 0x042F)) { out.push_back(c + 0x20); return; }

    // Armênio
    if (inRange(c, 0x0531, 0x0556)) { out.push_back(c + 0x30); return; }

    out.push_back(c);
}

// ---- Espaços e caracteres de formatação ------------------------------------

inline bool isWhitespace(char32_t c) noexcept {
    return inRange(c, 0x0009, 0x000D) || c == 0x0020 || c == 0x0085 || c == 0x00A0 || c == 0x1680 ||
           inRange(c, 0x2000, 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

/// Caracteres invisíveis que não alteram o significado do texto em escritas
/// alfabéticas: soft hyphen, zero-width space, word joiner e BOM.
/// (ZWJ/ZWNJ são preservados: são significativos em árabe, persa e escritas índicas.)
inline bool isIgnorableFormatChar(char32_t c) noexcept {
    return c == 0x00AD || c == 0x200B || c == 0x2060 || c == 0xFEFF;
}

}  // namespace detail

// -----------------------------------------------------------------------------
// Interface de passo de normalização (Strategy)
// -----------------------------------------------------------------------------

/// Um passo puro: recebe UTF-32 e devolve UTF-32. Novas regras podem ser
/// adicionadas implementando esta interface, sem alterar os passos existentes.
class INormalizationStep {
public:
    virtual ~INormalizationStep() = default;
    virtual std::u32string apply(std::u32string input) const = 0;
};

enum class UnicodeForm { NFC, NFD, NFKC, NFKD };

// -----------------------------------------------------------------------------
// Passos concretos
// -----------------------------------------------------------------------------

/// Normalização Unicode (NFC/NFD/NFKC/NFKD).
class UnicodeNormalizationStep final : public INormalizationStep {
public:
    explicit UnicodeNormalizationStep(UnicodeForm form) noexcept : form_(form) {}

    std::u32string apply(std::u32string input) const override {
        const bool compatibility = (form_ == UnicodeForm::NFKC || form_ == UnicodeForm::NFKD);
        const bool composing = (form_ == UnicodeForm::NFC || form_ == UnicodeForm::NFKC);

        std::u32string decomposed = detail::decompose(input, compatibility);
        return composing ? detail::compose(decomposed) : decomposed;
    }

private:
    UnicodeForm form_;
};

/// Case Folding completo, adequado para comparação sem distinção de caixa.
class CaseFoldingStep final : public INormalizationStep {
public:
    std::u32string apply(std::u32string input) const override {
        std::u32string output;
        output.reserve(input.size());
        for (const char32_t c : input) detail::appendCaseFolded(c, output);
        return output;
    }
};

/// Remoção de diacríticos: decompõe (NFD), descarta marcas combinantes e
/// recompõe (NFC) para manter o restante do texto em forma canônica.
class DiacriticRemovalStep final : public INormalizationStep {
public:
    std::u32string apply(std::u32string input) const override {
        const std::u32string decomposed = detail::decompose(input, /*compatibility=*/false);

        std::u32string stripped;
        stripped.reserve(decomposed.size());
        for (const char32_t c : decomposed) {
            if (!detail::isDiacriticMark(c)) stripped.push_back(c);
        }
        return detail::compose(stripped);
    }
};

/// Normalização de formatação: remove caracteres invisíveis, colapsa qualquer
/// sequência de espaços em um único U+0020 e remove espaços nas extremidades.
class WhitespaceNormalizationStep final : public INormalizationStep {
public:
    std::u32string apply(std::u32string input) const override {
        std::u32string output;
        output.reserve(input.size());

        bool pendingSpace = false;
        for (const char32_t c : input) {
            if (detail::isIgnorableFormatChar(c)) continue;
            if (detail::isWhitespace(c)) {
                pendingSpace = !output.empty();
                continue;
            }
            if (pendingSpace) {
                output.push_back(U' ');
                pendingSpace = false;
            }
            output.push_back(c);
        }
        return output;
    }
};

// -----------------------------------------------------------------------------
// Configuração
// -----------------------------------------------------------------------------

struct NormalizerOptions {
    bool applyUnicodeNormalization = true;  ///< Normalização Unicode (ver `form`).
    bool applyCaseFolding = true;           ///< Case folding completo.
    bool removeDiacritics = true;           ///< Remove acentos e outras marcas combinantes.
    bool normalizeWhitespace = true;        ///< Colapsa espaços e remove invisíveis.

    /// NFKC é a escolha padrão para comparação: além de unificar composição,
    /// também unifica variantes de compatibilidade (ligaturas "ﬁ", largura total, NBSP).
    UnicodeForm form = UnicodeForm::NFKC;
};

// -----------------------------------------------------------------------------
// Normalizer (Facade sobre o pipeline de passos)
// -----------------------------------------------------------------------------

/// Ordem do pipeline:
///   Unicode -> Case Folding -> Remoção de diacríticos -> Unicode -> Espaços
///
/// A segunda normalização Unicode garante que o resultado final esteja na forma
/// escolhida, já que o case folding e a remoção de marcas podem produzir
/// sequências não normalizadas. Com isso a operação é idempotente:
/// normalize(normalize(x)) == normalize(x).
///
/// Observação: se `removeDiacritics` estiver ativo e `applyUnicodeNormalization`
/// não, o passo de remoção ainda decompõe/recompõe internamente (saída em NFC).
class Normalizer {
public:
    explicit Normalizer(NormalizerOptions options = NormalizerOptions{}) : options_(options) {
        if (options_.applyUnicodeNormalization) {
            steps_.push_back(std::make_unique<UnicodeNormalizationStep>(options_.form));
        }
        if (options_.applyCaseFolding) {
            steps_.push_back(std::make_unique<CaseFoldingStep>());
        }
        if (options_.removeDiacritics) {
            steps_.push_back(std::make_unique<DiacriticRemovalStep>());
        }
        if (options_.applyUnicodeNormalization) {
            steps_.push_back(std::make_unique<UnicodeNormalizationStep>(options_.form));
        }
        if (options_.normalizeWhitespace) {
            steps_.push_back(std::make_unique<WhitespaceNormalizationStep>());
        }
    }

    Normalizer(const Normalizer&) = delete;
    Normalizer& operator=(const Normalizer&) = delete;
    Normalizer(Normalizer&&) noexcept = default;
    Normalizer& operator=(Normalizer&&) noexcept = default;

    /// Normaliza texto UTF-8.
    /// @throws InvalidUtf8Error se a entrada não for UTF-8 válido.
    std::string normalize(std::string_view utf8Text) const {
        return utf8::encode(normalize(utf8::decode(utf8Text)));
    }

    /// Normaliza uma sequência de code points Unicode (UTF-32).
    std::u32string normalize(std::u32string_view codePoints) const {
        std::u32string current(codePoints);
        for (const auto& step : steps_) {
            current = step->apply(std::move(current));
        }
        return current;
    }

    /// Verdadeiro se os dois textos possuem a mesma representação normalizada.
    /// @throws InvalidUtf8Error se alguma entrada não for UTF-8 válido.
    bool equivalent(std::string_view lhs, std::string_view rhs) const {
        return normalize(lhs) == normalize(rhs);
    }

    const NormalizerOptions& options() const noexcept { return options_; }

private:
    NormalizerOptions options_;
    std::vector<std::unique_ptr<INormalizationStep>> steps_;
};

}  // namespace text

#endif  // STRING_NORMALIZER
