#ifndef ALGORITHMS_KMP
#define ALGORITHMS_KMP

#include <cstddef>
#include <string_view>
#include <vector>

/**
 * Implementação do algoritmo de Knuth-Morris-Pratt (KMP).
 *
 * Complexidade:
 *  - computeLPS: O(m), onde m = tamanho do padrão
 *  - search:     O(n + m), onde n = tamanho do texto
 *
 * Requer C++17 (std::string_view).
 */
namespace Algorithms {

/**
 * Pré-processamento: constrói a tabela LPS (Longest Proper Prefix which is
 * also Suffix) do padrão.
 *
 * lps[i] = tamanho do maior prefixo próprio de pattern[0..i] que também é
 * sufixo de pattern[0..i]. "Próprio" significa que o prefixo não pode ser a
 * própria substring inteira.
 *
 * Exemplo: "ABABC" -> {0, 0, 1, 2, 0}
 *
 * Caso de borda: padrão vazio retorna um vetor vazio.
 */
inline std::vector<int> computeLPS(std::string_view pattern) {
    std::vector<int> lps(pattern.size(), 0);

    if (pattern.empty()) {
        return lps;
    }

    std::size_t prefixLength = 0;  // comprimento do prefixo-sufixo atual
    for (std::size_t i = 1; i < pattern.size(); ++i) {
        // Em caso de divergência, recua para o próximo melhor prefixo
        // candidato, sem reexaminar caracteres já comparados.
        while (prefixLength > 0 && pattern[i] != pattern[prefixLength]) {
            prefixLength = static_cast<std::size_t>(lps[prefixLength - 1]);
        }

        if (pattern[i] == pattern[prefixLength]) {
            ++prefixLength;
        }

        lps[i] = static_cast<int>(prefixLength);
    }

    return lps;
}

/**
 * Busca: encontra todas as ocorrências de `pattern` em `text`, inclusive as
 * que se sobrepõem (ex.: "AA" em "AAAA" ocorre nas posições 0, 1 e 2).
 *
 * Retorna os índices (base 0) em `text` onde cada ocorrência começa.
 *
 * Casos de borda (todos retornam um vetor vazio, sem lançar exceção):
 *  - texto vazio
 *  - padrão vazio (não há ocorrência significativa a reportar)
 *  - padrão maior que o texto
 */
inline std::vector<std::size_t> search(std::string_view text,
                                       std::string_view pattern) {
    std::vector<std::size_t> occurrences;

    if (text.empty() || pattern.empty() || pattern.size() > text.size()) {
        return occurrences;
    }

    const std::vector<int> lps = computeLPS(pattern);

    std::size_t matched = 0;  // quantidade de caracteres do padrão casados
    for (std::size_t i = 0; i < text.size(); ++i) {
        // Em divergência, reaproveita o prefixo já casado em vez de
        // retroceder o índice do texto.
        while (matched > 0 && text[i] != pattern[matched]) {
            matched = static_cast<std::size_t>(lps[matched - 1]);
        }

        if (text[i] == pattern[matched]) {
            ++matched;
        }

        if (matched == pattern.size()) {
            occurrences.push_back(i + 1 - matched);
            // Continua a partir do maior borda para capturar sobreposições.
            matched = static_cast<std::size_t>(lps[matched - 1]);
        }
    }

    return occurrences;
}

}  // namespace Algorithms

#endif  // ALGORITHMS_KMP
