#ifndef ALGORITHMS_RABIN_KARP_HPP
#define ALGORITHMS_RABIN_KARP_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

/**
 * Implementação do algoritmo de Rabin-Karp para busca de padrões em strings.
 *
 * Usa um hash polinomial rolante: o hash da próxima janela do texto é obtido
 * em O(1) a partir do hash da janela atual. Sempre que os hashes coincidem, os
 * caracteres são comparados diretamente, de modo que colisões de hash nunca
 * geram falsos positivos.
 *
 * Complexidade:
 *   - Caso médio:  O(n + m)
 *   - Pior caso:   O(n * m) (muitas colisões de hash)
 *   - Memória:     O(1), sem contar o vetor de resultados
 *
 * Convenções:
 *   - Ocorrências sobrepostas são reportadas (ex.: "aa" em "aaa" -> {0, 1}).
 *   - Padrão vazio não possui ocorrências significativas: retorna vazio.
 *   - Os bytes são tratados como unsigned char, então funciona com qualquer
 *     codificação de 8 bits (ASCII, UTF-8 comparado byte a byte, etc.).
 */
namespace Algorithms {

namespace RabinKarp::Detail {

inline constexpr std::uint64_t kBase = 256;
inline constexpr std::uint64_t kModulus = 1'000'000'007ULL;

inline std::uint64_t to_value(char c) noexcept {
    return static_cast<unsigned char>(c);
}

// Calcula (kBase^(exponent)) % kModulus por exponenciação por quadrados.
inline std::uint64_t power_mod(std::size_t exponent) noexcept {
    std::uint64_t result = 1;
    std::uint64_t base = kBase % kModulus;
    while (exponent > 0) {
        if (exponent & 1U) {
            result = (result * base) % kModulus;
        }
        base = (base * base) % kModulus;
        exponent >>= 1U;
    }
    return result;
}

inline std::uint64_t hash_of(std::string_view text, std::size_t length) noexcept {
    std::uint64_t hash = 0;
    for (std::size_t i = 0; i < length; ++i) {
        hash = (hash * kBase + to_value(text[i])) % kModulus;
    }
    return hash;
}

/**
 * Percorre o texto e invoca `on_match(posicao)` para cada ocorrência.
 * Se o callback retornar false, a busca é interrompida.
 */
template <typename OnMatch>
void for_each_match(std::string_view text, std::string_view pattern, OnMatch on_match) {
    const std::size_t text_size = text.size();
    const std::size_t pattern_size = pattern.size();

    if (pattern_size == 0 || pattern_size > text_size) {
        return;
    }

    // Peso do caractere mais à esquerda da janela: base^(m-1).
    const std::uint64_t leading_weight = power_mod(pattern_size - 1);

    const std::uint64_t pattern_hash = hash_of(pattern, pattern_size);
    std::uint64_t window_hash = hash_of(text, pattern_size);

    const std::size_t last_start = text_size - pattern_size;
    for (std::size_t start = 0; start <= last_start; ++start) {
        // A comparação direta descarta falsos positivos causados por colisões.
        if (window_hash == pattern_hash &&
            text.compare(start, pattern_size, pattern) == 0) {
            if (!on_match(start)) {
                return;
            }
        }

        if (start < last_start) {
            const std::uint64_t outgoing = (to_value(text[start]) * leading_weight) % kModulus;
            const std::uint64_t incoming = to_value(text[start + pattern_size]);

            // Somar kModulus antes de subtrair evita underflow em unsigned.
            window_hash = (window_hash + kModulus - outgoing) % kModulus;
            window_hash = (window_hash * kBase + incoming) % kModulus;
        }
    }
}

}  // namespace RabinKarp::Detail

/**
 * Retorna as posições iniciais de todas as ocorrências de `pattern` em `text`,
 * em ordem crescente.
 */
inline std::vector<std::size_t> find_all(std::string_view text, std::string_view pattern) {
    std::vector<std::size_t> positions;
    RabinKarp::Detail::for_each_match(text, pattern, [&positions](std::size_t position) {
        positions.push_back(position);
        return true;
    });
    return positions;
}

/**
 * Retorna a posição da primeira ocorrência de `pattern` em `text`,
 * ou std::nullopt se não houver nenhuma.
 */
inline std::optional<std::size_t> find_first(std::string_view text, std::string_view pattern) {
    std::optional<std::size_t> first_position;
    RabinKarp::Detail::for_each_match(text, pattern, [&first_position](std::size_t position) {
        first_position = position;
        return false;  // interrompe após o primeiro resultado
    });
    return first_position;
}

}  // namespace Algorithms

#endif  //  ALGORITHMS_RABIN_KARP_HPP
