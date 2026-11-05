#ifndef STRING_INDEXING_SUFIX_ARRAY_HPP
#define STRING_INDEX_SUFIX_ARRAY_HPP

#include <algorithm>
#include <cstddef>
#include <string_view>
#include <vector>

/**
 * Suffix Array construído com o algoritmo Prefix Doubling (Manber & Myers).
 *
 * Ideia: ordenamos os sufixos pelos primeiros k caracteres e, a cada rodada,
 * dobramos k (1, 2, 4, ...). O ranking dos 2k primeiros caracteres de um
 * sufixo i é o par (rank[i], rank[i + k]), que já é conhecido da rodada anterior.
 * Cada rodada usa Counting Sort (radix de 2 passes), logo:
 *
 *   Tempo:   O(n log n)
 *   Memória: O(n + sigma), com sigma = 256
 *
 * Além do vetor de sufixos, mantém o vetor de ranks (inverso do SA) e o
 * vetor LCP calculado pelo algoritmo de Kasai em O(n).
 */
class SuffixArray {
public:
    using size_type = std::size_t;

    explicit SuffixArray(std::string_view text)
        : text_(text) {
        buildSuffixArray();
        buildLcp();
    }

    /// Tamanho do texto (e do suffix array).
    size_type size() const noexcept { return sa_.size(); }

    /// Posição inicial do i-ésimo menor sufixo (ordem lexicográfica).
    int operator[](size_type i) const { return sa_[i]; }

    /// Vetor de sufixos completo.
    const std::vector<int>& suffixArray() const noexcept { return sa_; }

    /// rank()[i] = posição do sufixo que começa em i dentro do suffix array.
    const std::vector<int>& rank() const noexcept { return rank_; }

    /// lcp()[i] = tamanho do maior prefixo comum entre sa[i] e sa[i-1]; lcp()[0] = 0.
    const std::vector<int>& lcp() const noexcept { return lcp_; }

private:
    static constexpr int kAlphabetSize = 256;

    std::string_view text_;
    std::vector<int> sa_;
    std::vector<int> rank_;
    std::vector<int> lcp_;

    // ------------------------------------------------------------------
    // Prefix Doubling
    // ------------------------------------------------------------------
    void buildSuffixArray() {
        const int n = static_cast<int>(text_.size());
        sa_.assign(n, 0);
        rank_.assign(n, 0);
        if (n == 0) return;

        std::vector<int> sortedBySecondKey(n);
        std::vector<int> newRank(n);
        std::vector<int> counter(std::max(n, kAlphabetSize) + 1);

        int classes = sortByFirstCharacter(counter);

        for (int k = 1; k < n && classes < n; k <<= 1) {
            orderBySecondKey(k, sortedBySecondKey);
            countingSortByFirstKey(sortedBySecondKey, classes, counter);
            classes = recomputeRanks(k, newRank);
            rank_.swap(newRank);
        }
    }

    /// Ordena os sufixos pelo primeiro caractere e inicializa rank_.
    /// Retorna a quantidade de classes de equivalência distintas.
    int sortByFirstCharacter(std::vector<int>& counter) {
        const int n = static_cast<int>(text_.size());

        std::fill(counter.begin(), counter.begin() + kAlphabetSize, 0);
        for (int i = 0; i < n; ++i) ++counter[charAt(i)];
        for (int c = 1; c < kAlphabetSize; ++c) counter[c] += counter[c - 1];
        for (int i = n - 1; i >= 0; --i) sa_[--counter[charAt(i)]] = i;

        int classes = 1;
        rank_[sa_[0]] = 0;
        for (int i = 1; i < n; ++i) {
            if (charAt(sa_[i]) != charAt(sa_[i - 1])) ++classes;
            rank_[sa_[i]] = classes - 1;
        }
        return classes;
    }

    /// Gera os sufixos ordenados pela segunda metade (chave i + k).
    /// Sufixos com i + k >= n têm segunda chave vazia (menor possível) e vêm primeiro.
    void orderBySecondKey(int k, std::vector<int>& out) const {
        const int n = static_cast<int>(text_.size());
        int p = 0;
        for (int i = n - k; i < n; ++i) out[p++] = i;
        for (int i = 0; i < n; ++i) {
            if (sa_[i] >= k) out[p++] = sa_[i] - k;
        }
    }

    /// Counting Sort estável pela primeira chave (rank_[i]) sobre a ordem da segunda chave.
    void countingSortByFirstKey(const std::vector<int>& bySecondKey,
                                int classes,
                                std::vector<int>& counter) {
        const int n = static_cast<int>(text_.size());

        std::fill(counter.begin(), counter.begin() + classes, 0);
        for (int i = 0; i < n; ++i) ++counter[rank_[i]];
        for (int c = 1; c < classes; ++c) counter[c] += counter[c - 1];
        for (int i = n - 1; i >= 0; --i) {
            const int suffix = bySecondKey[i];
            sa_[--counter[rank_[suffix]]] = suffix;
        }
    }

    /// Recalcula os ranks com base nos pares (rank[i], rank[i + k]).
    /// Retorna o novo número de classes.
    int recomputeRanks(int k, std::vector<int>& newRank) const {
        const int n = static_cast<int>(text_.size());

        int classes = 1;
        newRank[sa_[0]] = 0;
        for (int i = 1; i < n; ++i) {
            const int prev = sa_[i - 1];
            const int curr = sa_[i];
            if (rank_[prev] != rank_[curr] || secondKey(prev, k) != secondKey(curr, k)) {
                ++classes;
            }
            newRank[curr] = classes - 1;
        }
        return classes;
    }

    int secondKey(int i, int k) const {
        return (i + k < static_cast<int>(text_.size())) ? rank_[i + k] : -1;
    }

    int charAt(int i) const { return static_cast<unsigned char>(text_[i]); }

    // ------------------------------------------------------------------
    // LCP (Kasai)
    // ------------------------------------------------------------------
    void buildLcp() {
        const int n = static_cast<int>(text_.size());
        lcp_.assign(n, 0);

        int h = 0;
        for (int i = 0; i < n; ++i) {
            if (rank_[i] > 0) {
                const int j = sa_[rank_[i] - 1];
                while (i + h < n && j + h < n && text_[i + h] == text_[j + h]) ++h;
                lcp_[rank_[i]] = h;
                if (h > 0) --h;
            } else {
                h = 0;
            }
        }
    }
};
#endif // STRING_INDEXING_SUFIX_ARRAY_HPP
