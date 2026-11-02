#ifndef ALGORITHMS_LEVENSHTEIN
#define ALGORITHMS_LEVENSHTEIN

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

namespace Levenshtein::Text {

/**
 * Calcula a Distância de Levenshtein entre duas sequências de bytes.
 *
 * A distância é o número mínimo de operações de edição de um único
 * caractere (inserção, remoção ou substituição) necessárias para
 * transformar `a` em `b`.
 *
 * Complexidade de tempo:  O(n * m)
 * Complexidade de espaço: O(min(n, m)) (duas linhas da matriz de programação dinâmica)
 *
 * Observação: a comparação é feita byte a byte. Para textos UTF-8 com
 * caracteres multibyte, converta previamente para uma sequência de
 * code points (por exemplo, std::u32string) e utilize a sobrecarga
 * genérica `levenshtein_distance(first, last, first2, last2)`.
 */
template <typename RandomIt1, typename RandomIt2>
std::size_t levenshtein_distance(RandomIt1 first1, RandomIt1 last1,
                                 RandomIt2 first2, RandomIt2 last2) {
    std::size_t n = static_cast<std::size_t>(last1 - first1);
    std::size_t m = static_cast<std::size_t>(last2 - first2);

    // Garante que a linha da DP use o menor comprimento.
    if (n < m) {
        return levenshtein_distance(first2, last2, first1, last1);
    }

    if (m == 0) {
        return n;
    }

    std::vector<std::size_t> previous(m + 1);
    std::vector<std::size_t> current(m + 1);

    for (std::size_t j = 0; j <= m; ++j) {
        previous[j] = j;
    }

    for (std::size_t i = 1; i <= n; ++i) {
        current[0] = i;

        for (std::size_t j = 1; j <= m; ++j) {
            const std::size_t cost = (first1[i - 1] == first2[j - 1]) ? 0 : 1;

            current[j] = std::min({
                previous[j] + 1,        // remoção
                current[j - 1] + 1,     // inserção
                previous[j - 1] + cost  // substituição (ou igualdade)
            });
        }

        std::swap(previous, current);
    }

    return previous[m];
}

/**
 * Sobrecarga de conveniência para std::string_view (e, por conversão
 * implícita, std::string e const char*).
 */
inline std::size_t levenshtein_distance(std::string_view a, std::string_view b) {
    return levenshtein_distance(a.begin(), a.end(), b.begin(), b.end());
}

/**
 * Similaridade normalizada no intervalo [0.0, 1.0], onde 1.0 indica
 * sequências idênticas.
 */
inline double levenshtein_similarity(std::string_view a, std::string_view b) {
    const std::size_t max_len = std::max(a.size(), b.size());
    if (max_len == 0) {
        return 1.0;
    }
    return 1.0 - static_cast<double>(levenshtein_distance(a, b)) /
                     static_cast<double>(max_len);
}

}  // namespace Levenshtein::Text

#endif  // ALGORITHMS_LEVENSHTEIN
