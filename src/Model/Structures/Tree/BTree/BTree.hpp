#ifndef STRUCTURES_BTRE_HPP
#define STRUCTURES_BTRE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace BTree::Page  {

// ======================================================================
// Contrato com o Sistema de Páginas
// ======================================================================

/** Identificador de uma página gerenciada pelo Sistema de Páginas. */
using PageId = std::uint64_t;

/** Valor reservado para "nenhuma página" (árvore vazia, ausência de filho). */
inline constexpr PageId kNullPageId = std::numeric_limits<PageId>::max();

/**
 * Representação em memória de um nó da Árvore B.
 *
 * Os filhos são referenciados por PageId (e não por ponteiros), pois cada nó
 * vive em uma página. A serialização deste struct para os bytes da página é
 * responsabilidade exclusiva do Sistema de Páginas.
 */
template <typename Key, typename Value>
struct BTreeNode {
    PageId pageId = kNullPageId;               // página onde o nó está persistido
    bool leaf = true;
    std::vector<std::pair<Key, Value>> entries;  // ordenadas por chave
    std::vector<PageId> children;                // entries.size() + 1 se interno
};

/**
 * Interface que a Árvore B exige do Sistema de Páginas (Inversão de
 * Dependência): a árvore conhece apenas este contrato e nunca acessa arquivos.
 *
 * Para integrar o Sistema de Páginas existente, implemente esta interface
 * diretamente nele ou em um adaptador que delegue as chamadas.
 *
 * Todos os métodos podem lançar exceção em caso de falha de E/S; a Árvore B
 * não captura nem mascara esses erros.
 */
template <typename Key, typename Value>
class PageStorage {
public:
    using Node = BTreeNode<Key, Value>;

    virtual ~PageStorage() = default;

    /** Recupera o nó armazenado na página pageId (retorna uma cópia em memória). */
    virtual Node loadNode(PageId pageId) = 0;

    /** Persiste o nó na página node.pageId. */
    virtual void writeNode(const Node& node) = 0;

    /** Solicita uma nova página para um nó e retorna seu identificador. */
    virtual PageId allocateNode() = 0;

    /** Libera a página de um nó que foi removido da árvore. */
    virtual void freeNode(PageId pageId) = 0;

    /** Identificador da página raiz, ou kNullPageId se a árvore estiver vazia. */
    virtual PageId getRootPageId() const = 0;

    /** Atualiza a referência da raiz (kNullPageId para árvore vazia). */
    virtual void setRootPageId(PageId pageId) = 0;
};

// ======================================================================
// Árvore B
// ======================================================================

/**
 * Árvore B persistente (mapa ordenado chave -> valor, sem chaves duplicadas).
 *
 * Todo acesso a nós passa pelo PageStorage injetado: a árvore carrega um nó
 * (loadNode), modifica a cópia em memória e a persiste (writeNode). Nenhum
 * estado da estrutura é mantido na árvore além da referência ao storage, então
 * várias instâncias sobre o mesmo storage enxergam a mesma árvore.
 *
 * Parâmetros de template:
 *   Key        Tipo da chave (copiável e comparável via Compare).
 *   Value      Tipo do valor (copiável).
 *   MinDegree  Grau mínimo "t" (t >= 2). Cada nó, exceto a raiz, possui entre
 *              t-1 e 2t-1 chaves. Escolha t de modo que um nó com 2t-1 entradas
 *              e 2t PageIds caiba em uma página do Sistema de Páginas
 *              (veja kMaxEntries e kMaxChildren).
 *   Compare    Relação de ordem estrita (padrão: std::less<Key>).
 *
 * Complexidade em páginas acessadas (n = chaves, k = tamanho do resultado):
 *   search / insert / update / remove / min / max ... O(log_t n)
 *   range_query .................................... O(log_t n + k/t)
 *   in_order / to_vector / size / clear ............ O(n/t)
 *
 * Observação: a árvore não garante atomicidade entre várias escritas de uma
 * mesma operação; isso deve ser tratado pelo Sistema de Páginas (ex.: log).
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
    using Node        = BTreeNode<Key, Value>;
    using Storage     = PageStorage<Key, Value>;

    static constexpr std::size_t kMinDegree   = MinDegree;
    static constexpr std::size_t kMaxEntries  = 2 * MinDegree - 1;
    static constexpr std::size_t kMaxChildren = 2 * MinDegree;

    /** O storage deve sobreviver à árvore; a árvore não é dona dele. */
    explicit BTree(Storage& storage, Compare comp = Compare())
        : storage_(storage), comp_(std::move(comp)) {}

    BTree(const BTree&) = delete;
    BTree& operator=(const BTree&) = delete;

    // ------------------------------------------------------------------
    // Informações gerais
    // ------------------------------------------------------------------

    bool empty() const { return storage_.getRootPageId() == kNullPageId; }

    /** Número de chaves. Custo O(n/t) páginas, pois a contagem não é persistida. */
    size_type size() const {
        size_type count = 0;
        in_order([&count](const Key&, const Value&) { ++count; });
        return count;
    }

    /** Remove todas as chaves e libera todas as páginas da árvore. */
    void clear() {
        const PageId root_id = storage_.getRootPageId();
        if (root_id == kNullPageId) {
            return;
        }
        // A raiz é desvinculada antes de liberar as páginas, para que a
        // árvore nunca aponte para uma página já liberada.
        storage_.setRootPageId(kNullPageId);
        release_subtree(root_id);
    }

    // ------------------------------------------------------------------
    // Busca
    // ------------------------------------------------------------------

    bool contains(const Key& key) const { return locate(key).has_value(); }

    /** Retorna o valor associado à chave, ou std::nullopt se não existir. */
    std::optional<Value> search(const Key& key) const {
        std::optional<Location> location = locate(key);
        if (!location) {
            return std::nullopt;
        }
        return std::move(location->node.entries[location->index].second);
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
        if (locate(key)) {
            return false;
        }

        Node root = prepare_root_for_insertion();
        insert_non_full(std::move(root), value_type(std::move(key), std::move(value)));
        return true;
    }

    // ------------------------------------------------------------------
    // Atualização
    // ------------------------------------------------------------------

    /** Substitui o valor de uma chave existente. Retorna false se ela não existir. */
    bool update(const Key& key, Value new_value) {
        std::optional<Location> location = locate(key);
        if (!location) {
            return false;
        }
        location->node.entries[location->index].second = std::move(new_value);
        storage_.writeNode(location->node);
        return true;
    }

    // ------------------------------------------------------------------
    // Remoção
    // ------------------------------------------------------------------

    /** Remove a chave. Retorna false se ela não existir. */
    bool remove(const Key& key) {
        const PageId root_id = storage_.getRootPageId();
        if (root_id == kNullPageId) {
            return false;
        }

        Node root = load_node(root_id);
        const bool removed = remove_from(root, key);

        // A única forma de a árvore perder altura é a raiz ficar sem chaves.
        // Isso pode ocorrer mesmo sem remoção: a descida funde filhos
        // preventivamente, ainda que a chave acabe não sendo encontrada.
        if (root.entries.empty()) {
            release_empty_root(root);
        }
        return removed;
    }

    // ------------------------------------------------------------------
    // Mínimo e máximo
    // ------------------------------------------------------------------

    /** Menor par (chave, valor), ou std::nullopt se a árvore estiver vazia. */
    std::optional<value_type> min() const {
        const PageId root_id = storage_.getRootPageId();
        if (root_id == kNullPageId) {
            return std::nullopt;
        }
        return min_entry(load_node(root_id));
    }

    /** Maior par (chave, valor), ou std::nullopt se a árvore estiver vazia. */
    std::optional<value_type> max() const {
        const PageId root_id = storage_.getRootPageId();
        if (root_id == kNullPageId) {
            return std::nullopt;
        }
        return max_entry(load_node(root_id));
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
        const PageId root_id = storage_.getRootPageId();
        if (root_id != kNullPageId) {
            collect_range(load_node(root_id), lower, upper, result);
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
        const PageId root_id = storage_.getRootPageId();
        if (root_id != kNullPageId) {
            in_order_impl(load_node(root_id), visit);
        }
    }

    /** Todos os pares em ordem crescente de chave. */
    std::vector<value_type> to_vector() const {
        std::vector<value_type> result;
        in_order([&result](const Key& key, const Value& value) {
            result.emplace_back(key, value);
        });
        return result;
    }

private:
    /** Resultado de uma busca: o nó carregado e a posição da chave nele. */
    struct Location {
        Node node;
        std::size_t index;
    };

    Storage& storage_;
    Compare comp_;

    // ------------------------------------------------------------------
    // Acesso a páginas (único ponto de contato com o Sistema de Páginas)
    // ------------------------------------------------------------------

    Node load_node(PageId page_id) const { return storage_.loadNode(page_id); }

    /** Cria um nó vazio já associado a uma página recém-alocada (ainda não escrita). */
    Node create_node(bool leaf) {
        Node node;
        node.pageId = storage_.allocateNode();
        node.leaf   = leaf;
        node.entries.reserve(kMaxEntries);
        if (!leaf) {
            node.children.reserve(kMaxChildren);
        }
        return node;
    }

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

    std::optional<Location> locate(const Key& key) const {
        PageId page_id = storage_.getRootPageId();
        while (page_id != kNullPageId) {
            Node node = load_node(page_id);
            const std::size_t i = lower_index(node, key);
            if (matches_at(node, i, key)) {
                return Location{std::move(node), i};
            }
            if (node.leaf) {
                return std::nullopt;
            }
            page_id = node.children[i];
        }
        return std::nullopt;
    }

    value_type min_entry(const Node& node) const {
        if (node.leaf) {
            return node.entries.front();
        }
        Node current = load_node(node.children.front());
        while (!current.leaf) {
            const PageId next = current.children.front();
            current = load_node(next);
        }
        return current.entries.front();
    }

    value_type max_entry(const Node& node) const {
        if (node.leaf) {
            return node.entries.back();
        }
        Node current = load_node(node.children.back());
        while (!current.leaf) {
            const PageId next = current.children.back();
            current = load_node(next);
        }
        return current.entries.back();
    }

    /** Libera recursivamente (pós-ordem) a página de um nó e de seus descendentes. */
    void release_subtree(PageId page_id) {
        const Node node = load_node(page_id);
        if (!node.leaf) {
            for (const PageId child_id : node.children) {
                release_subtree(child_id);
            }
        }
        storage_.freeNode(page_id);
    }

    // ------------------------------------------------------------------
    // Inserção
    // ------------------------------------------------------------------

    /**
     * Devolve a raiz pronta para receber uma inserção (nunca cheia),
     * criando-a se a árvore estiver vazia ou dividindo-a se estiver cheia.
     */
    Node prepare_root_for_insertion() {
        const PageId root_id = storage_.getRootPageId();

        if (root_id == kNullPageId) {
            Node root = create_node(true);
            storage_.writeNode(root);
            storage_.setRootPageId(root.pageId);
            return root;
        }

        Node root = load_node(root_id);
        if (!is_full(root)) {
            return root;
        }

        Node new_root = create_node(false);
        new_root.children.push_back(root.pageId);
        split_child(new_root, 0, root);  // persiste new_root, a antiga raiz e o irmão
        storage_.setRootPageId(new_root.pageId);
        return new_root;
    }

    /**
     * Divide o filho cheio `full_child` (= parent.children[index]) em dois nós
     * com t-1 chaves, promovendo a mediana para `parent` (que não pode estar
     * cheio). Persiste os três nós e devolve o novo irmão direito.
     */
    Node split_child(Node& parent, std::size_t index, Node& full_child) {
        Node right = create_node(full_child.leaf);

        value_type median = std::move(full_child.entries[kMinDegree - 1]);

        right.entries.assign(
            std::make_move_iterator(full_child.entries.begin() + kMinDegree),
            std::make_move_iterator(full_child.entries.end()));
        full_child.entries.erase(full_child.entries.begin() + (kMinDegree - 1),
                                 full_child.entries.end());

        if (!full_child.leaf) {
            right.children.assign(full_child.children.begin() + kMinDegree,
                                  full_child.children.end());
            full_child.children.erase(full_child.children.begin() + kMinDegree,
                                      full_child.children.end());
        }

        parent.entries.insert(parent.entries.begin() + index, std::move(median));
        parent.children.insert(parent.children.begin() + index + 1, right.pageId);

        // Ordem: o pai só passa a apontar para páginas que já foram escritas.
        storage_.writeNode(right);
        storage_.writeNode(full_child);
        storage_.writeNode(parent);
        return right;
    }

    /** Insere em uma subárvore cuja raiz `node` não está cheia (chave inédita). */
    void insert_non_full(Node node, value_type entry) {
        while (!node.leaf) {
            std::size_t i = lower_index(node, entry.first);
            Node child = load_node(node.children[i]);

            if (is_full(child)) {
                Node right = split_child(node, i, child);
                // A mediana subiu para node.entries[i]; decide o lado.
                if (comp_(node.entries[i].first, entry.first)) {
                    child = std::move(right);
                }
            }
            node = std::move(child);
        }

        const std::size_t position = lower_index(node, entry.first);
        node.entries.insert(node.entries.begin() + position, std::move(entry));
        storage_.writeNode(node);
    }

    // ------------------------------------------------------------------
    // Remoção
    // ------------------------------------------------------------------

    /**
     * Remove a chave da subárvore de `node`, que é a cópia em memória
     * autoritativa (atualizada in-place e persistida quando modificada).
     * Pré-condição: `node` é a raiz ou possui ao menos t chaves.
     */
    bool remove_from(Node& node, const Key& key) {
        const std::size_t i = lower_index(node, key);

        if (matches_at(node, i, key)) {
            if (node.leaf) {
                node.entries.erase(node.entries.begin() + i);
                storage_.writeNode(node);
                return true;
            }
            return remove_from_internal(node, i, key);
        }

        if (node.leaf) {
            return false;
        }

        Node child = load_node(node.children[i]);
        if (child.entries.size() < kMinDegree) {
            child = fill_child(node, i, std::move(child));
        }
        return remove_from(child, key);
    }

    /** A chave está em node.entries[i], e `node` é interno. */
    bool remove_from_internal(Node& node, std::size_t i, const Key& key) {
        Node left = load_node(node.children[i]);

        if (left.entries.size() >= kMinDegree) {
            // Substitui pelo predecessor e o remove da subárvore esquerda.
            // A cópia é necessária: a entrada original será apagada da folha.
            const value_type predecessor = max_entry(left);
            node.entries[i] = predecessor;
            storage_.writeNode(node);
            return remove_from(left, predecessor.first);
        }

        Node right = load_node(node.children[i + 1]);

        if (right.entries.size() >= kMinDegree) {
            const value_type successor = min_entry(right);
            node.entries[i] = successor;
            storage_.writeNode(node);
            return remove_from(right, successor.first);
        }

        // Ambos os filhos estão no mínimo: funde-os (a chave desce para o
        // nó fundido) e continua a remoção ali.
        merge_children(node, i, left, right);
        return remove_from(left, key);
    }

    /**
     * Garante que `child` (= parent.children[i]) tenha ao menos t chaves,
     * emprestando de um irmão ou fundindo com ele. Devolve o nó que passou a
     * cobrir o intervalo original (é o irmão esquerdo quando a fusão ocorre
     * com ele). Todos os nós modificados são persistidos.
     */
    Node fill_child(Node& parent, std::size_t i, Node child) {
        if (i > 0) {
            Node left = load_node(parent.children[i - 1]);
            if (left.entries.size() >= kMinDegree) {
                borrow_from_previous(parent, i, child, left);
                return child;
            }
            if (i == parent.entries.size()) {
                // Último filho: só existe irmão à esquerda.
                merge_children(parent, i - 1, left, child);
                return left;
            }
        }

        Node right = load_node(parent.children[i + 1]);
        if (right.entries.size() >= kMinDegree) {
            borrow_from_next(parent, i, child, right);
            return child;
        }
        merge_children(parent, i, child, right);
        return child;
    }

    /** Rotação à direita: a última chave do irmão esquerdo sobe e a do pai desce. */
    void borrow_from_previous(Node& parent, std::size_t i, Node& child, Node& sibling) {
        child.entries.insert(child.entries.begin(), std::move(parent.entries[i - 1]));
        if (!child.leaf) {
            child.children.insert(child.children.begin(), sibling.children.back());
            sibling.children.pop_back();
        }

        parent.entries[i - 1] = std::move(sibling.entries.back());
        sibling.entries.pop_back();

        storage_.writeNode(child);
        storage_.writeNode(sibling);
        storage_.writeNode(parent);
    }

    /** Rotação à esquerda: a primeira chave do irmão direito sobe e a do pai desce. */
    void borrow_from_next(Node& parent, std::size_t i, Node& child, Node& sibling) {
        child.entries.push_back(std::move(parent.entries[i]));
        if (!child.leaf) {
            child.children.push_back(sibling.children.front());
            sibling.children.erase(sibling.children.begin());
        }

        parent.entries[i] = std::move(sibling.entries.front());
        sibling.entries.erase(sibling.entries.begin());

        storage_.writeNode(child);
        storage_.writeNode(sibling);
        storage_.writeNode(parent);
    }

    /**
     * Funde `right` e a chave separadora parent.entries[i] em `left`
     * (= parent.children[i]). Ambos têm t-1 chaves, então o resultado tem 2t-1.
     * A página de `right` é liberada depois que pai e `left` foram persistidos.
     */
    void merge_children(Node& parent, std::size_t i, Node& left, Node& right) {
        left.entries.push_back(std::move(parent.entries[i]));
        std::move(right.entries.begin(), right.entries.end(),
                  std::back_inserter(left.entries));

        if (!left.leaf) {
            left.children.insert(left.children.end(),
                                 right.children.begin(), right.children.end());
        }

        parent.entries.erase(parent.entries.begin() + i);
        parent.children.erase(parent.children.begin() + i + 1);

        storage_.writeNode(left);
        storage_.writeNode(parent);
        storage_.freeNode(right.pageId);
    }

    /** Raiz ficou sem chaves: a árvore encolhe um nível (ou fica vazia). */
    void release_empty_root(const Node& root) {
        storage_.setRootPageId(root.leaf ? kNullPageId : root.children.front());
        storage_.freeNode(root.pageId);
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
                collect_range(load_node(node.children[i]), lower, upper, out);
            }
            if (comp_(upper, node.entries[i].first)) {
                return;  // a partir daqui tudo é maior que upper
            }
            out.push_back(node.entries[i]);
        }

        if (!node.leaf) {
            collect_range(load_node(node.children[count]), lower, upper, out);
        }
    }

    template <typename Visitor>
    void in_order_impl(const Node& node, Visitor& visit) const {
        const std::size_t count = node.entries.size();

        for (std::size_t i = 0; i < count; ++i) {
            if (!node.leaf) {
                in_order_impl(load_node(node.children[i]), visit);
            }
            visit(node.entries[i].first, node.entries[i].second);
        }

        if (!node.leaf) {
            in_order_impl(load_node(node.children[count]), visit);
        }
    }
};

}  // namespace BTree::Page 

#endif  // STRUCTURES_BTRE_HPP