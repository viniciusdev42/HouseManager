#ifndef STRUCTURES_BTRE_HPP
#define STRUCTURES_BTRE_HPP

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

/**
 * Árvore B (mapa ordenado chave -> valor, sem chaves duplicadas).
 *
 * Parâmetros de template:
 *   Key        Tipo da chave (copiável e comparável via Compare).
 *   Value      Tipo do valor (copiável; a cópia ocorre apenas na remoção de
 *              nós internos, quando o predecessor/sucessor substitui a chave).
 *   MinDegree  Grau mínimo "t" (t >= 2). Cada nó, exceto a raiz, possui entre
 *              t-1 e 2t-1 chaves; um nó interno possui entre t e 2t filhos.
 *   Compare    Relação de ordem estrita (padrão: std::less<Key>).
 *
 * Complexidade (n = número de chaves, k = tamanho do resultado):
 *   search / insert / update / remove ..... O(t * log_t n)
 *   min / max ............................. O(log_t n)
 *   range_query ........................... O(log_t n + k)
 *   in_order / to_vector .................. O(n)
 *
 * A inserção divide nós cheios preventivamente (top-down) e a remoção garante
 * que todo nó visitado tenha ao menos t chaves antes de descer, de modo que
 * nenhuma operação precisa subir novamente pela árvore.
 */
template <typename Key,
          typename Value,
          std::size_t MinDegree = 3,
          typename Compare = std::less<Key>>
class BTree {
    static_assert(MinDegree >= 2, "O grau mínimo da Árvore B deve ser >= 2.");

public:
    using key_type    = Key;
    using mapped_type = Value;
    using value_type  = std::pair<Key, Value>;
    using size_type   = std::size_t;

    explicit BTree(Compare comp = Compare()) : comp_(std::move(comp)) {}

    // Copiar uma árvore exigiria clonagem profunda; fica fora do escopo.
    BTree(const BTree&) = delete;
    BTree& operator=(const BTree&) = delete;

    BTree(BTree&& other) noexcept
        : comp_(std::move(other.comp_)),
          root_(std::move(other.root_)),
          size_(std::exchange(other.size_, 0)) {}

    BTree& operator=(BTree&& other) noexcept {
        if (this != &other) {
            comp_ = std::move(other.comp_);
            root_ = std::move(other.root_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }

    ~BTree() = default;

    // ------------------------------------------------------------------
    // Informações gerais
    // ------------------------------------------------------------------

    size_type size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    void clear() noexcept {
        root_.reset();
        size_ = 0;
    }

    // ------------------------------------------------------------------
    // Busca
    // ------------------------------------------------------------------

    bool contains(const Key& key) const { return locate(key) != nullptr; }

    /** Retorna o valor associado à chave, ou std::nullopt se não existir. */
    std::optional<Value> search(const Key& key) const {
        const value_type* entry = locate(key);
        if (entry == nullptr) {
            return std::nullopt;
        }
        return entry->second;
    }

    // ------------------------------------------------------------------
    // Inserção
    // ------------------------------------------------------------------

    /**
     * Insere o par (chave, valor).
     * Retorna false, sem alterar a árvore, se a chave já existir
     * (use update() para trocar o valor de uma chave existente).
     */
    bool insert(Key key, Value value) {
        // A verificação prévia permite que a descida de inserção assuma
        // chaves únicas e nunca precise desfazer divisões de nós.
        if (locate(key) != nullptr) {
            return false;
        }

        if (!root_) {
            root_ = std::make_unique<Node>(true);
        }

        if (is_full(*root_)) {
            auto new_root = std::make_unique<Node>(false);
            new_root->children.push_back(std::move(root_));
            split_child(*new_root, 0);
            root_ = std::move(new_root);
        }

        insert_non_full(*root_, value_type(std::move(key), std::move(value)));
        ++size_;
        return true;
    }

    // ------------------------------------------------------------------
    // Atualização
    // ------------------------------------------------------------------

    /** Substitui o valor de uma chave existente. Retorna false se ela não existir. */
    bool update(const Key& key, Value new_value) {
        value_type* entry = locate(key);
        if (entry == nullptr) {
            return false;
        }
        entry->second = std::move(new_value);
        return true;
    }

    // ------------------------------------------------------------------
    // Remoção
    // ------------------------------------------------------------------

    /** Remove a chave. Retorna false se ela não existir. */
    bool remove(const Key& key) {
        if (!root_) {
            return false;
        }

        const bool removed = remove_from(*root_, key);
        if (removed) {
            --size_;
        }

        // A única forma de a árvore perder altura é a raiz ficar sem chaves.
        if (root_->entries.empty()) {
            if (root_->leaf) {
                root_.reset();
            } else {
                std::unique_ptr<Node> only_child = std::move(root_->children.front());
                root_ = std::move(only_child);
            }
        }
        return removed;
    }

    // ------------------------------------------------------------------
    // Mínimo e máximo
    // ------------------------------------------------------------------

    /** Menor par (chave, valor), ou std::nullopt se a árvore estiver vazia. */
    std::optional<value_type> min() const {
        if (!root_) {
            return std::nullopt;
        }
        return min_entry(*root_);
    }

    /** Maior par (chave, valor), ou std::nullopt se a árvore estiver vazia. */
    std::optional<value_type> max() const {
        if (!root_) {
            return std::nullopt;
        }
        return max_entry(*root_);
    }

    // ------------------------------------------------------------------
    // Consulta por intervalo
    // ------------------------------------------------------------------

    /**
     * Retorna, em ordem crescente, todos os pares com lower <= chave <= upper.
     * Lança std::invalid_argument se upper < lower.
     */
    std::vector<value_type> range_query(const Key& lower, const Key& upper) const {
        if (comp_(upper, lower)) {
            throw std::invalid_argument(
                "BTree::range_query: o limite superior é menor que o limite inferior.");
        }

        std::vector<value_type> result;
        if (root_) {
            collect_range(*root_, lower, upper, result);
        }
        return result;
    }

    // ------------------------------------------------------------------
    // Percurso ordenado
    // ------------------------------------------------------------------

    /**
     * Percorre a árvore em ordem crescente de chave, chamando
     * visit(const Key&, const Value&) para cada par.
     */
    template <typename Visitor>
    void in_order(Visitor&& visit) const {
        if (root_) {
            in_order_impl(*root_, visit);
        }
    }

    /** Todos os pares em ordem crescente de chave. */
    std::vector<value_type> to_vector() const {
        std::vector<value_type> result;
        result.reserve(size_);
        in_order([&result](const Key& key, const Value& value) {
            result.emplace_back(key, value);
        });
        return result;
    }

private:
    static constexpr std::size_t kMinDegree  = MinDegree;
    static constexpr std::size_t kMaxEntries = 2 * MinDegree - 1;

    struct Node {
        explicit Node(bool is_leaf) : leaf(is_leaf) {
            entries.reserve(kMaxEntries);
            if (!is_leaf) {
                children.reserve(kMaxEntries + 1);
            }
        }

        bool leaf;
        std::vector<value_type> entries;              // ordenadas por chave
        std::vector<std::unique_ptr<Node>> children;  // entries.size() + 1 se interno
    };

    Compare comp_;
    std::unique_ptr<Node> root_;
    size_type size_ = 0;

    // ------------------------------------------------------------------
    // Utilitários de nó
    // ------------------------------------------------------------------

    static bool is_full(const Node& node) { return node.entries.size() == kMaxEntries; }

    /** Índice da primeira entrada com chave >= key (ou entries.size()). */
    std::size_t lower_index(const Node& node, const Key& key) const {
        const auto it = std::lower_bound(
            node.entries.begin(), node.entries.end(), key,
            [this](const value_type& entry, const Key& k) { return comp_(entry.first, k); });
        return static_cast<std::size_t>(std::distance(node.entries.begin(), it));
    }

    /** True se a entrada no índice i existe e possui chave equivalente a key. */
    bool matches_at(const Node& node, std::size_t i, const Key& key) const {
        return i < node.entries.size() && !comp_(key, node.entries[i].first);
    }

    /**
     * Localiza a entrada da chave. Retorna ponteiro mutável mesmo em método
     * const porque é um detalhe interno: quem expõe ao usuário (search)
     * só lê, e update() é o único que escreve.
     */
    value_type* locate(const Key& key) const {
        Node* current = root_.get();
        while (current != nullptr) {
            const std::size_t i = lower_index(*current, key);
            if (matches_at(*current, i, key)) {
                return &current->entries[i];
            }
            if (current->leaf) {
                return nullptr;
            }
            current = current->children[i].get();
        }
        return nullptr;
    }

    static const value_type& min_entry(const Node& node) {
        const Node* current = &node;
        while (!current->leaf) {
            current = current->children.front().get();
        }
        return current->entries.front();
    }

    static const value_type& max_entry(const Node& node) {
        const Node* current = &node;
        while (!current->leaf) {
            current = current->children.back().get();
        }
        return current->entries.back();
    }

    // ------------------------------------------------------------------
    // Inserção
    // ------------------------------------------------------------------

    /**
     * Divide o filho cheio parent.children[index] em dois nós com t-1 chaves
     * cada, promovendo a chave mediana para o pai (que não pode estar cheio).
     */
    void split_child(Node& parent, std::size_t index) {
        Node& full_child = *parent.children[index];
        auto right_sibling = std::make_unique<Node>(full_child.leaf);

        value_type median = std::move(full_child.entries[kMinDegree - 1]);

        right_sibling->entries.assign(
            std::make_move_iterator(full_child.entries.begin() + kMinDegree),
            std::make_move_iterator(full_child.entries.end()));
        full_child.entries.erase(full_child.entries.begin() + (kMinDegree - 1),
                                 full_child.entries.end());

        if (!full_child.leaf) {
            right_sibling->children.assign(
                std::make_move_iterator(full_child.children.begin() + kMinDegree),
                std::make_move_iterator(full_child.children.end()));
            full_child.children.erase(full_child.children.begin() + kMinDegree,
                                      full_child.children.end());
        }

        parent.entries.insert(parent.entries.begin() + index, std::move(median));
        parent.children.insert(parent.children.begin() + index + 1, std::move(right_sibling));
    }

    /** Insere em uma subárvore cuja raiz não está cheia (chave inédita). */
    void insert_non_full(Node& root, value_type entry) {
        Node* current = &root;

        while (!current->leaf) {
            std::size_t i = lower_index(*current, entry.first);

            if (is_full(*current->children[i])) {
                split_child(*current, i);
                // A mediana subiu para current->entries[i]; decide o lado.
                if (comp_(current->entries[i].first, entry.first)) {
                    ++i;
                }
            }
            current = current->children[i].get();
        }

        const std::size_t position = lower_index(*current, entry.first);
        current->entries.insert(current->entries.begin() + position, std::move(entry));
    }

    // ------------------------------------------------------------------
    // Remoção
    // ------------------------------------------------------------------

    /**
     * Remove a chave da subárvore de `node`. Pré-condição: `node` é a raiz
     * ou possui ao menos t chaves, garantindo que a remoção não deixe um
     * nó abaixo do mínimo.
     */
    bool remove_from(Node& node, const Key& key) {
        std::size_t i = lower_index(node, key);

        if (matches_at(node, i, key)) {
            if (node.leaf) {
                node.entries.erase(node.entries.begin() + i);
                return true;
            }
            return remove_from_internal(node, i, key);
        }

        if (node.leaf) {
            return false;
        }

        if (node.children[i]->entries.size() < kMinDegree) {
            i = fill_child(node, i);
        }
        return remove_from(*node.children[i], key);
    }

    /** A chave está em node.entries[i], e `node` é interno. */
    bool remove_from_internal(Node& node, std::size_t i, const Key& key) {
        Node& left  = *node.children[i];
        Node& right = *node.children[i + 1];

        if (left.entries.size() >= kMinDegree) {
            // Substitui pelo predecessor e o remove da subárvore esquerda.
            // A cópia é necessária: a entrada original será apagada da folha.
            value_type predecessor = max_entry(left);
            node.entries[i] = predecessor;
            return remove_from(left, predecessor.first);
        }

        if (right.entries.size() >= kMinDegree) {
            value_type successor = min_entry(right);
            node.entries[i] = successor;
            return remove_from(right, successor.first);
        }

        // Ambos os filhos estão no mínimo: funde-os (a chave desce para o
        // nó fundido) e continua a remoção ali.
        merge_children(node, i);
        return remove_from(*node.children[i], key);
    }

    /**
     * Garante que node.children[i] tenha ao menos t chaves, emprestando de um
     * irmão ou fundindo com ele. Retorna o índice do filho que passou a
     * cobrir o intervalo original (muda quando funde com o irmão anterior).
     */
    std::size_t fill_child(Node& node, std::size_t i) {
        if (i > 0 && node.children[i - 1]->entries.size() >= kMinDegree) {
            borrow_from_previous(node, i);
            return i;
        }
        if (i < node.entries.size() && node.children[i + 1]->entries.size() >= kMinDegree) {
            borrow_from_next(node, i);
            return i;
        }
        if (i < node.entries.size()) {
            merge_children(node, i);
            return i;
        }
        merge_children(node, i - 1);
        return i - 1;
    }

    /** Rotação à direita: a última chave do irmão esquerdo sobe e a do pai desce. */
    void borrow_from_previous(Node& parent, std::size_t i) {
        Node& child   = *parent.children[i];
        Node& sibling = *parent.children[i - 1];

        child.entries.insert(child.entries.begin(), std::move(parent.entries[i - 1]));
        if (!child.leaf) {
            child.children.insert(child.children.begin(), std::move(sibling.children.back()));
            sibling.children.pop_back();
        }

        parent.entries[i - 1] = std::move(sibling.entries.back());
        sibling.entries.pop_back();
    }

    /** Rotação à esquerda: a primeira chave do irmão direito sobe e a do pai desce. */
    void borrow_from_next(Node& parent, std::size_t i) {
        Node& child   = *parent.children[i];
        Node& sibling = *parent.children[i + 1];

        child.entries.push_back(std::move(parent.entries[i]));
        if (!child.leaf) {
            child.children.push_back(std::move(sibling.children.front()));
            sibling.children.erase(sibling.children.begin());
        }

        parent.entries[i] = std::move(sibling.entries.front());
        sibling.entries.erase(sibling.entries.begin());
    }

    /**
     * Funde children[i + 1] e a chave separadora entries[i] em children[i].
     * Ambos os filhos têm t-1 chaves, então o resultado tem 2t-1.
     */
    void merge_children(Node& parent, std::size_t i) {
        Node& left = *parent.children[i];
        std::unique_ptr<Node> right = std::move(parent.children[i + 1]);

        left.entries.push_back(std::move(parent.entries[i]));
        std::move(right->entries.begin(), right->entries.end(),
                  std::back_inserter(left.entries));

        if (!left.leaf) {
            std::move(right->children.begin(), right->children.end(),
                      std::back_inserter(left.children));
        }

        parent.entries.erase(parent.entries.begin() + i);
        parent.children.erase(parent.children.begin() + i + 1);
    }

    // ------------------------------------------------------------------
    // Consulta por intervalo e percurso
    // ------------------------------------------------------------------

    void collect_range(const Node& node,
                       const Key& lower,
                       const Key& upper,
                       std::vector<value_type>& out) const {
        const std::size_t count = node.entries.size();

        // Pula todas as entradas (e subárvores) estritamente menores que lower.
        for (std::size_t i = lower_index(node, lower); i < count; ++i) {
            if (!node.leaf) {
                collect_range(*node.children[i], lower, upper, out);
            }
            if (comp_(upper, node.entries[i].first)) {
                return;  // a partir daqui tudo é maior que upper
            }
            out.push_back(node.entries[i]);
        }

        if (!node.leaf) {
            collect_range(*node.children[count], lower, upper, out);
        }
    }

    template <typename Visitor>
    void in_order_impl(const Node& node, Visitor& visit) const {
        const std::size_t count = node.entries.size();

        for (std::size_t i = 0; i < count; ++i) {
            if (!node.leaf) {
                in_order_impl(*node.children[i], visit);
            }
            visit(node.entries[i].first, node.entries[i].second);
        }

        if (!node.leaf) {
            in_order_impl(*node.children[count], visit);
        }
    }
};

#endif  // STRUCTURES_BTRE_HPP
