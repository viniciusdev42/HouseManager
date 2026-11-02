#ifndef  ALGORITHMS_BOYER_MOORE
#define  ALGORITHMS_BOYER_MOORE

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

/**
 * Implementação do algoritmo de busca de substring Boyer-Moore,
 * com as duas heurísticas clássicas:
 *   - Bad Character Rule (caractere ruim)
 *   - Good Suffix Rule   (sufixo bom)
 *
 * O pré-processamento do padrão é feito uma única vez no construtor,
 * permitindo reutilizar o mesmo objeto para buscar em vários textos.
 *
 * Complexidade:
 *   - Pré-processamento: O(m + |alfabeto|)
 *   - Busca: O(n) no pior caso para a primeira ocorrência com a regra do
 *     sufixo bom, e tipicamente sublinear (~n/m) em textos comuns.
 *
 * Observação: a comparação é feita byte a byte (alfabeto de 256 símbolos).
 * Para texto UTF-8, a busca é correta em nível de bytes.
 */
namespace Algorithms::BoyerMoore {

class BoyerMoore {
public:
    /// Posição retornada por find_first quando não há ocorrência.
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    /// @throws std::invalid_argument se o padrão for vazio.
    explicit BoyerMoore(std::string_view pattern) : pattern_(pattern) {
        if (pattern_.empty()) {
            throw std::invalid_argument("BoyerMoore: o padrao nao pode ser vazio.");
        }
        build_bad_character_table();
        build_good_suffix_table();
    }

    /// Retorna o índice da primeira ocorrência do padrão em `text`, ou `npos`.
    [[nodiscard]] std::size_t find_first(std::string_view text) const noexcept {
        std::size_t result = npos;
        search(text, [&result](std::size_t position) {
            result = position;
            return false;  // interrompe a busca na primeira ocorrência
        });
        return result;
    }

    /// Versão com std::optional de find_first.
    [[nodiscard]] std::optional<std::size_t> find(std::string_view text) const noexcept {
        const std::size_t position = find_first(text);
        if (position == npos) {
            return std::nullopt;
        }
        return position;
    }

    /// Retorna todas as ocorrências (inclusive sobrepostas) em ordem crescente.
    [[nodiscard]] std::vector<std::size_t> find_all(std::string_view text) const {
        std::vector<std::size_t> positions;
        search(text, [&positions](std::size_t position) {
            positions.push_back(position);
            return true;  // continua a busca
        });
        return positions;
    }

    /// Indica se o padrão ocorre em `text`.
    [[nodiscard]] bool contains(std::string_view text) const noexcept {
        return find_first(text) != npos;
    }

    [[nodiscard]] const std::string& pattern() const noexcept { return pattern_; }

private:
    static constexpr std::size_t kAlphabetSize =
        static_cast<std::size_t>(std::numeric_limits<unsigned char>::max()) + 1;

    std::string pattern_;
    // Última posição de cada byte no padrão (-1 se ausente).
    std::array<std::ptrdiff_t, kAlphabetSize> last_occurrence_{};
    // good_suffix_shift_[j]: deslocamento quando houve mismatch após casar o sufixo
    // pattern_[j..m-1]. O índice 0 guarda o deslocamento após um match completo.
    std::vector<std::size_t> good_suffix_shift_;

    static std::size_t to_index(char c) noexcept {
        return static_cast<unsigned char>(c);
    }

    void build_bad_character_table() {
        last_occurrence_.fill(-1);
        for (std::size_t i = 0; i < pattern_.size(); ++i) {
            last_occurrence_[to_index(pattern_[i])] = static_cast<std::ptrdiff_t>(i);
        }
    }

    void build_good_suffix_table() {
        const std::size_t m = pattern_.size();
        good_suffix_shift_.assign(m + 1, 0);
        std::vector<std::size_t> border(m + 1, 0);

        // Caso 1: o sufixo casado ocorre em outra posição do padrão
        // precedido por um caractere diferente.
        std::size_t i = m;
        std::size_t j = m + 1;
        border[i] = j;
        while (i > 0) {
            while (j <= m && pattern_[i - 1] != pattern_[j - 1]) {
                if (good_suffix_shift_[j] == 0) {
                    good_suffix_shift_[j] = j - i;
                }
                j = border[j];
            }
            --i;
            --j;
            border[i] = j;
        }

        // Caso 2: apenas um prefixo do padrão coincide com parte do sufixo casado.
        j = border[0];
        for (i = 0; i <= m; ++i) {
            if (good_suffix_shift_[i] == 0) {
                good_suffix_shift_[i] = j;
            }
            if (i == j) {
                j = border[j];
            }
        }
    }

    /// Deslocamento pela regra do caractere ruim (pode ser <= 0, por isso é com sinal).
    std::ptrdiff_t bad_character_shift(std::size_t mismatch_index, char text_char) const noexcept {
        return static_cast<std::ptrdiff_t>(mismatch_index) - last_occurrence_[to_index(text_char)];
    }

    /**
     * Percorre o texto e invoca `on_match(posicao)` a cada ocorrência.
     * Se o callback retornar false, a busca é interrompida.
     */
    template <typename OnMatch>
    void search(std::string_view text, OnMatch&& on_match) const {
        const std::size_t m = pattern_.size();
        const std::size_t n = text.size();
        if (n < m) {
            return;
        }

        std::size_t shift = 0;
        while (shift <= n - m) {
            std::size_t j = m;
            while (j > 0 && pattern_[j - 1] == text[shift + j - 1]) {
                --j;
            }

            if (j == 0) {
                if (!on_match(shift)) {
                    return;
                }
                shift += good_suffix_shift_[0];
            } else {
                const std::ptrdiff_t bad_char = bad_character_shift(j - 1, text[shift + j - 1]);
                const auto good_suffix = static_cast<std::ptrdiff_t>(good_suffix_shift_[j]);
                shift += static_cast<std::size_t>(std::max(bad_char, good_suffix));
            }
        }
    }
};

/// Atalho: primeira ocorrência de `pattern` em `text`, ou BoyerMoore::npos.
/// @throws std::invalid_argument se o padrão for vazio.
[[nodiscard]] inline std::size_t find_first(std::string_view text, std::string_view pattern) {
    return BoyerMoore(pattern).find_first(text);
}

/// Atalho: todas as ocorrências de `pattern` em `text`.
/// @throws std::invalid_argument se o padrão for vazio.
[[nodiscard]] inline std::vector<std::size_t> find_all(std::string_view text, std::string_view pattern) {
    return BoyerMoore(pattern).find_all(text);
}

}  // namespace bm

#endif  //  ALGORITHMS_BOYER_MOORE
