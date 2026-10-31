/* Tokenizador de texto UTF-8, header-only (C++17), sem dependências externas.
 *
 * Contrato do módulo: recebe um texto e devolve uma coleção de tokens,
 * na mesma ordem em que aparecem no texto.
 *
 * Prioridade de classificação de cada code point:
 *  1. delimitador customizado  -> encerra o token, é descartado
 *   2. espaço (Unicode)         -> encerra o token, é descartado
 *   3. pontuação/símbolo        -> tratado conforme PunctuationMode
 *  4. qualquer outro           -> faz parte da palavra
 *
 * Exemplo:
 *   text::Tokenizer tokenizer;
 *    auto tokens = tokenizer.tokenize("Olá, mundo! Guarda-chuva.");
 *    {"Olá", ",", "mundo", "!", "Guarda-chuva", "."} 
*/

#ifndef STRUCTURES_TOKENIZER
#define STRUCTURES_TOKENIZER

#include <algorithm>
#include <iterator>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Tokenizer::Text {

// Como a pontuação é tratada na saída.
enum class PunctuationMode {
    Separate,  // vira um token próprio: "oi!" -> "oi", "!"
    Discard,   // é removida:            "oi!" -> "oi"
    Keep       // fica colada na palavra: "oi!" -> "oi!"
};

struct TokenizerOptions {
    // Delimitadores adicionais, como code points (ex.: U",;|").
    // Têm prioridade sobre a regra de pontuação.
    std::u32string delimiters;

    PunctuationMode punctuation = PunctuationMode::Separate;

    // Quando false, delimitadores consecutivos geram tokens vazios
    // (semântica de "split"): "a,,b" com ',' como delimitador -> "a", "", "b".
    bool removeEmptyTokens = true;

    // Mantém hífen e apóstrofo entre duas letras dentro do token
    // ("guarda-chuva", "d'água"). Ignorado em PunctuationMode::Keep.
    bool keepIntraWordConnectors = true;
};

namespace Tokenizer::Details {

struct DecodedCodePoint {
    char32_t value;
    std::size_t length;  // bytes consumidos no texto original
};

// Decodifica um code point UTF-8 em `pos`. Sequências inválidas (overlong,
// surrogates, truncadas, > U+10FFFF) consomem 1 byte e retornam U+FFFD, o que
// garante progresso sempre e permite preservar os bytes originais no token.
inline DecodedCodePoint decodeUtf8At(std::string_view s, std::size_t pos) noexcept {
    constexpr DecodedCodePoint invalid{0xFFFD, 1};

    const auto lead = static_cast<unsigned char>(s[pos]);
    if (lead < 0x80) return {lead, 1};

    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;  // menor valor legítimo para esse tamanho (anti-overlong)

    if ((lead & 0xE0) == 0xC0)      { length = 2; value = lead & 0x1F; minimum = 0x80; }
    else if ((lead & 0xF0) == 0xE0) { length = 3; value = lead & 0x0F; minimum = 0x800; }
    else if ((lead & 0xF8) == 0xF0) { length = 4; value = lead & 0x07; minimum = 0x10000; }
    else return invalid;

    if (pos + length > s.size()) return invalid;

    for (std::size_t i = 1; i < length; ++i) {
        const auto byte = static_cast<unsigned char>(s[pos + i]);
        if ((byte & 0xC0) != 0x80) return invalid;
        value = (value << 6) | (byte & 0x3F);
    }

    const bool isSurrogate = value >= 0xD800 && value <= 0xDFFF;
    if (value < minimum || value > 0x10FFFF || isSurrogate) return invalid;

    return {value, length};
}

// Propriedade White_Space do Unicode, mais U+200B (zero-width space), que na
// prática separa palavras em textos de várias escritas.
constexpr bool isWhitespace(char32_t cp) noexcept {
    return (cp >= 0x09 && cp <= 0x0D) || cp == 0x20 || cp == 0x85 || cp == 0xA0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200B) || cp == 0x2028 ||
           cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

struct CodePointRange {
    char32_t first;
    char32_t last;
};

// Pontuação e símbolos. Tabela ORDENADA por `first` (exigido pela busca binária).
// É uma aproximação das categorias P* e S* do Unicode, cobrindo ASCII, Latin-1,
// pontuação geral, símbolos monetários/matemáticos, CJK, formas de largura
// total e pontuações dos principais sistemas de escrita.
inline constexpr CodePointRange kPunctuationRanges[] = {
    {0x0021, 0x002F}, {0x003A, 0x0040}, {0x005B, 0x0060}, {0x007B, 0x007E},
    {0x00A1, 0x00A9}, {0x00AB, 0x00B1}, {0x00B4, 0x00B4}, {0x00B6, 0x00B8},
    {0x00BB, 0x00BB}, {0x00BF, 0x00BF}, {0x00D7, 0x00D7}, {0x00F7, 0x00F7},
    {0x037E, 0x037E}, {0x0387, 0x0387}, {0x055A, 0x055F}, {0x0589, 0x058A},
    {0x05BE, 0x05BE}, {0x05C0, 0x05C0}, {0x05C3, 0x05C3}, {0x05C6, 0x05C6},
    {0x05F3, 0x05F4}, {0x060C, 0x060D}, {0x061B, 0x061B}, {0x061F, 0x061F},
    {0x066A, 0x066D}, {0x06D4, 0x06D4}, {0x0964, 0x0965}, {0x0E4F, 0x0E4F},
    {0x0E5A, 0x0E5B}, {0x1360, 0x1368}, {0x2010, 0x2027}, {0x2030, 0x205E},
    {0x20A0, 0x20CF}, {0x2190, 0x23FF}, {0x2500, 0x27BF}, {0x2E00, 0x2E7F},
    {0x3001, 0x3003}, {0x3008, 0x3011}, {0x3014, 0x301F}, {0x3030, 0x3030},
    {0x303D, 0x303D}, {0x30FB, 0x30FB}, {0xFE30, 0xFE4F}, {0xFE50, 0xFE6B},
    {0xFF01, 0xFF0F}, {0xFF1A, 0xFF20}, {0xFF3B, 0xFF40}, {0xFF5B, 0xFF65}
};

inline bool isPunctuation(char32_t cp) noexcept {
    const auto* const begin = std::begin(kPunctuationRanges);
    const auto* const end = std::end(kPunctuationRanges);

    const auto* it = std::upper_bound(begin, end, cp,
                               [](char32_t value, const CodePointRange& range) {
                                   return value < range.first;
                               });
    if (it == begin) return false;
    --it;
    return cp <= it->last;
}

// Hífen, apóstrofo ASCII, apóstrofo tipográfico (U+2019) e hífen Unicode (U+2010).
constexpr bool isConnector(char32_t cp) noexcept {
    return cp == U'-' || cp == U'\'' || cp == 0x2019 || cp == 0x2010;
}

}  // namespace Tokenizer::Details

class Tokenizer {
public:
    explicit Tokenizer(TokenizerOptions options = TokenizerOptions{})
        : options_(std::move(options)) {}

    // Devolve os tokens na ordem original do texto. Bytes inválidos em UTF-8
    // são preservados como estão dentro do token (nunca lança exceção).
    std::vector<std::string> tokenize(std::string_view text) const {
        std::vector<std::string> tokens;
        if (text.empty()) return tokens;

        std::string current;
        // "Aberto" = existe um token em construção (mesmo que ainda vazio).
        // Fecha após pontuação separada, pois ela já encerrou o token anterior;
        // isso evita gerar tokens vazios espúrios logo depois de "." ou ",".
        bool tokenOpen = true;
        bool lastWasWordChar = false;

        std::size_t pos = 0;
        while (pos < text.size()) {
            const auto decoded = detail::decodeUtf8At(text, pos);
            const std::string_view raw = text.substr(pos, decoded.length);
            const std::size_t nextPos = pos + decoded.length;

            switch (classify(decoded.value)) {
                case CharClass::Delimiter:
                case CharClass::Whitespace:
                    if (tokenOpen) flush(tokens, current);
                    tokenOpen = true;
                    lastWasWordChar = false;
                    break;

                case CharClass::Punctuation:
                    if (shouldStayInsideToken(decoded.value, lastWasWordChar, text, nextPos)) {
                        current.append(raw);
                        tokenOpen = true;
                        lastWasWordChar = false;
                        break;
                    }
                    if (!current.empty()) flush(tokens, current);
                    if (options_.punctuation == PunctuationMode::Separate) {
                        tokens.emplace_back(raw);
                    }
                    tokenOpen = false;
                    lastWasWordChar = false;
                    break;

                case CharClass::Word:
                    current.append(raw);
                    tokenOpen = true;
                    lastWasWordChar = true;
                    break;
            }
            pos = nextPos;
        }

        if (tokenOpen) flush(tokens, current);
        return tokens;
    }

private:
    enum class CharClass { Delimiter, Whitespace, Punctuation, Word };

    CharClass classify(char32_t cp) const noexcept {
        if (options_.delimiters.find(cp) != std::u32string::npos) return CharClass::Delimiter;
        if (detail::isWhitespace(cp)) return CharClass::Whitespace;
        if (detail::isPunctuation(cp)) return CharClass::Punctuation;
        return CharClass::Word;
    }

    // Em modo Keep a pontuação sempre fica no token. Nos demais modos, só
    // hífen/apóstrofo cercados por caracteres de palavra permanecem no token.
    bool shouldStayInsideToken(char32_t cp, bool lastWasWordChar,
                               std::string_view text, std::size_t nextPos) const noexcept {
        if (options_.punctuation == PunctuationMode::Keep) return true;
        if (!options_.keepIntraWordConnectors) return false;
        if (!lastWasWordChar || !detail::isConnector(cp)) return false;
        if (nextPos >= text.size()) return false;

        const auto next = detail::decodeUtf8At(text, nextPos);
        return classify(next.value) == CharClass::Word;
    }

    void flush(std::vector<std::string>& tokens, std::string& current) const {
        if (!current.empty() || !options_.removeEmptyTokens) {
            tokens.push_back(std::move(current));
        }
        current.clear();
    }

    TokenizerOptions options_;
};

}  // namespace Tokenizer::Text
