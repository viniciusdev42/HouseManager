#ifndef STRUCTURES_TRIE_HPP
#define STRUCTURES_TRIE_HPP

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

/**
 * Trie (árvore de prefixos) para armazenar palavras.
 *
 * Complexidades (L = tamanho da palavra/prefixo):
 *  - insert / remove / search / startsWith: O(L)
 *  - autocomplete: O(L + K), onde K é o número de nós visitados na subárvore
 *
 * Observações:
 *  - A comparação é feita byte a byte (case-sensitive). Strings UTF-8 funcionam
 *    normalmente, mas a ordenação lexicográfica é por byte, não por caractere.
 *  - Os filhos são mantidos em std::map para que o autocomplete retorne os
 *    resultados em ordem lexicográfica de forma determinística.
 *  - A classe é move-only (não copiável), pois é dona exclusiva dos seus nós.
 */
class Trie {
public:
    Trie();
    ~Trie() = default;

    Trie(const Trie&) = delete;
    Trie& operator=(const Trie&) = delete;
    Trie(Trie&&) noexcept = default;
    Trie& operator=(Trie&&) noexcept = default;

    /**
     * Insere uma palavra.
     * @return true se a palavra era nova; false se já existia.
     * @throws std::invalid_argument se a palavra for vazia.
     */
    bool insert(std::string_view word);

    /**
     * Remove uma palavra e poda os nós que ficaram sem utilidade.
     * @return true se a palavra existia e foi removida; false caso contrário.
     */
    bool remove(std::string_view word);

    /** Retorna true se a palavra exata está armazenada. */
    bool search(std::string_view word) const;

    /**
     * Retorna true se existe ao menos uma palavra que comece com o prefixo.
     * Um prefixo vazio retorna true somente se a Trie não estiver vazia.
     */
    bool startsWith(std::string_view prefix) const;

    /**
     * Retorna as palavras que começam com o prefixo, em ordem lexicográfica.
     * @param prefix     Prefixo a ser completado (pode ser vazio para listar tudo).
     * @param maxResults Limite de resultados; 0 significa sem limite.
     */
    std::vector<std::string> autocomplete(std::string_view prefix,
                                          std::size_t maxResults = 0) const;

    /** Quantidade de palavras armazenadas. */
    std::size_t size() const noexcept;

    /** Retorna true se não há palavras armazenadas. */
    bool empty() const noexcept;

    /** Remove todas as palavras. */
    void clear();

private:
    struct Node {
        std::map<char, std::unique_ptr<Node>> children;
        bool isEndOfWord = false;
    };

    /** Retorna o nó correspondente ao prefixo, ou nullptr se não existir. */
    const Node* findNode(std::string_view prefix) const;

    /** Coleta, em DFS, as palavras da subárvore respeitando o limite. */
    void collectWords(const Node* node,
                      std::string& currentWord,
                      std::vector<std::string>& results,
                      std::size_t maxResults) const;

    std::unique_ptr<Node> root_;
    std::size_t wordCount_ = 0;
};
