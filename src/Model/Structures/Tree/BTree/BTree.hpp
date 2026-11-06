#ifndef BTREE_HPP
#define BTREE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "BTreeNode.hpp"  // formato do nó, validação e (des)serialização do nó
#include "Page.hpp"       // unidade de armazenamento (página) e seus erros

namespace BTree {

/**
 * Árvore B persistente (mapa ordenado chave -> valor, sem chaves duplicadas),
 * com chaves e valores de 64 bits (BTreeNode::KeyType / ValueType).
 *
 * Camadas e responsabilidades:
 *   - BTreeNode.hpp: converte um nó em bytes e valida sua estrutura.
 *   - Page.hpp:      guarda esses bytes em uma página (com estado e checksum).
 *   - PageManager:   move páginas entre memória e disco e controla quais
 *                    páginas estão livres e qual é a raiz. É o
 *                    PageSystem::PageManager::PageManager.
 *   - BTree:         apenas algoritmos. Não acessa arquivos; todo nó é carregado
 *                    (loadNode), alterado em memória e persistido (writeNode).
 *
 * Requisitos do parâmetro do template PageManager (pager):
 *
 *   PageManager             getPage(PageId id);                              // cópia da página
 *   void                    updatePage(PageId id, const std::vector<Byte>&); // substitui o conteúdo
 *   PageId                  allocatePage();                                  // reserva uma página
 *   void                    freePage(PageId id);                             // devolve ao pool livre
 *   PageId                  getRootPageId() const;                           // kInvalidPageId se vazia
 *   void                    setRootPageId(PageId id);                        // kInvalidPageId = vazia
 *   std::uint32_t           pageSize() const;
 *
 * onde PageId é PageSystem::Page::PageId (32 bits) e Byte é std::uint8_t.
 * O identificador de página e o do nó coincidem (a página 0 é válida).
 *
 * Ordem das operações: o PageManager recusa liberar a página que é a raiz
 * atual e exige que a nova raiz já esteja em uso; por isso a árvore sempre
 * grava o nó, depois aponta a raiz para ele e só então libera a página antiga.
 *
 * Grau mínimo (t): cada nó, exceto a raiz, possui entre t-1 e 2t-1 chaves. Por
 * padrão, t é o maior valor cujo nó cheio cabe em uma página, de modo que o
 * mesmo tamanho de página sempre produz o mesmo t ao reabrir o arquivo. Um nó
 * lido com grau diferente do esperado é rejeitado como corrompido.
 *
 * Erros: exceções de Page.hpp (PageError) e BTreeNode.hpp (NodeFormatError)
 * propagam sem serem capturadas.
 *
 * Complexidade em páginas acessadas (n = chaves, k = tamanho do resultado):
 *   search / insert / update / remove / min / max ... O(log_t n)
 *   rangeQuery ..................................... O(log_t n + k/t)
 *   inOrder / toVector / size / clear .............. O(n/t)
 *
 * A árvore não garante atomicidade entre as várias escritas de uma mesma
 * operação; isso deve ser tratado pelo PageManager (por exemplo, com um log).
 */
template <typename PageManager>
class BTree {
public:
    using Node      = ::BTreeNode::BTreeNode;
    using Key       = ::BTreeNode::KeyType;
    using Value     = ::BTreeNode::ValueType;
    using PageId    = ::BTreeNode::PageId;  // 64 bits, como nos nós
    using value_type = std::pair<Key, Value>;
    using size_type  = std::size_t;

    /**
     * @param pager      Gerenciador de páginas; deve sobreviver à árvore.
     * @param minDegree  Grau mínimo t; 0 (padrão) escolhe o maior que cabe na página.
     * @throws std::invalid_argument se t for inválido ou o nó não couber na página.
     */
    explicit BTree(PageManager& pager, std::uint32_t minDegree = 0)
        : pager_(pager),
          minDegree_(resolveMinDegree(pager.pageSize(), minDegree)) {}

    // A árvore é uma visão sobre o PageManager; copiá-la não faria sentido.
    BTree(const BTree&) = delete;
    BTree& operator=(const BTree&) = delete;

    std::uint32_t minDegree() const noexcept { return minDegree_; }

    // ------------------------------------------------------------------
    // Informações gerais
    // ------------------------------------------------------------------

    bool empty() const { return getRootPageId() == kNoPage; }

    /** Número de chaves. Custo O(n/t) páginas, pois a contagem não é persistida. */
    size_type size() const {
        size_type count = 0;
        inOrder([&count](Key, Value) { ++count; });
        return count;
    }

    /** Remove todas as chaves e libera todas as páginas da árvore. */
    void clear() {
        const PageId rootId = getRootPageId();
        if (rootId == kNoPage) {
            return;
        }
        // A raiz é desvinculada antes de liberar as páginas, para que a
        // árvore nunca aponte para uma página já liberada.
        setRootPageId(kNoPage);
        releaseSubtree(rootId);
    }

    // ------------------------------------------------------------------
    // Busca
    // ------------------------------------------------------------------

    bool contains(Key key) const { return locate(key).has_value(); }

    /** Retorna o valor associado à chave, ou std::nullopt se não existir. */
    std::optional<Value> search(Key key) const {
        const std::optional<Location> location = locate(key);
        if (!location) {
            return std::nullopt;
        }
        return location->node.values[location->index];
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
        const PageId rootId = getRootPageId();

        if (rootId == kNoPage) {
            Node root = createNode(/*leaf=*/true, /*root=*/true);
            appendEntry(root, {key, value});
            writeNode(root);            // a página só é referenciada depois de escrita
            setRootPageId(root.nodeId);
            return true;
        }

        // A verificação prévia permite que a descida de inserção assuma
        // chaves únicas e nunca precise desfazer divisões de nós.
        if (locate(key)) {
            return false;
        }

        Node root = loadNode(rootId);
        if (isFull(root)) {
            root = splitRoot(root);
        }
        insertNonFull(std::move(root), key, value);
        return true;
    }

    // ------------------------------------------------------------------
    // Atualização
    // ------------------------------------------------------------------

    /** Substitui o valor de uma chave existente. Retorna false se ela não existir. */
    bool update(Key key, Value newValue) {
        std::optional<Location> location = locate(key);
        if (!location) {
            return false;
        }
        location->node.values[location->index] = newValue;
        writeNode(location->node);
        return true;
    }

    // ------------------------------------------------------------------
    // Remoção
    // ------------------------------------------------------------------

    /** Remove a chave. Retorna false se ela não existir. */
    bool remove(Key key) {
        const PageId rootId = getRootPageId();
        if (rootId == kNoPage) {
            return false;
        }

        Node root = loadNode(rootId);
        const bool removed = removeFrom(root, key);

        // Raiz folha sem chaves: a árvore ficou vazia. (O encolhimento de uma
        // raiz interna é tratado em mergeChildren, onde ela deixa de ser raiz.)
        if (root.isLeaf && root.keys.empty()) {
            setRootPageId(kNoPage);
            freeNode(root.nodeId);
        }
        return removed;
    }

    // ------------------------------------------------------------------
    // Mínimo e máximo
    // ------------------------------------------------------------------

    /** Menor par (chave, valor), ou std::nullopt se a árvore estiver vazia. */
    std::optional<value_type> min() const {
        const PageId rootId = getRootPageId();
        if (rootId == kNoPage) {
            return std::nullopt;
        }
        return minEntry(loadNode(rootId));
    }

    /** Maior par (chave, valor), ou std::nullopt se a árvore estiver vazia. */
    std::optional<value_type> max() const {
        const PageId rootId = getRootPageId();
        if (rootId == kNoPage) {
            return std::nullopt;
        }
        return maxEntry(loadNode(rootId));
    }

    // ------------------------------------------------------------------
    // Consulta por intervalo
    // ------------------------------------------------------------------

    /**
     * Retorna, em ordem crescente, todos os pares com lower <= chave <= upper.
     * Lança std::invalid_argument se upper < lower.
     */
    std::vector<value_type> rangeQuery(Key lower, Key upper) const {
        if (upper < lower) {
            throw std::invalid_argument(
                "BTree::rangeQuery: o limite superior é menor que o limite inferior.");
        }

        std::vector<value_type> result;
        const PageId rootId = getRootPageId();
        if (rootId != kNoPage) {
            collectRange(loadNode(rootId), lower, upper, result);
        }
        return result;
    }

    // ------------------------------------------------------------------
    // Percurso ordenado
    // ------------------------------------------------------------------

    /**
     * Percorre a árvore em ordem crescente de chave, chamando
     * visit(Key, Value) para cada par.
     */
    template <typename Visitor>
    void inOrder(Visitor&& visit) const {
        const PageId rootId = getRootPageId();
        if (rootId != kNoPage) {
            inOrderImpl(loadNode(rootId), visit);
        }
    }

    /** Todos os pares em ordem crescente de chave. */
    std::vector<value_type> toVector() const {
        std::vector<value_type> result;
        inOrder([&result](Key key, Value value) { result.emplace_back(key, value); });
        return result;
    }

private:
    using StoragePage   = ::PageSystem::Page::pageSize;
    using StoragePageId = ::PageSystem::Page::PageId;

    /** "Sem página" no nível dos nós (árvore vazia). */
    static constexpr PageId kNoPage = ::BTreeNode::kInvalidPageId;

    /** Resultado de uma busca: o nó carregado e a posição da chave nele. */
    struct Location {
        Node node;
        std::size_t index;
    };

    PageManager& pager_;
    std::uint32_t minDegree_;

    // ------------------------------------------------------------------
    // Configuração
    // ------------------------------------------------------------------

    static std::uint32_t resolveMinDegree(std::uint32_t pageSize, std::uint32_t requested) {
        namespace Format = ::BTreeNode;

        if (pageSize < Format::calculateMaxNodeSize(Format::kMinAllowedDegree)) {
            throw std::invalid_argument(
                "BTree: página pequena demais para um nó com o menor grau permitido.");
        }

        if (requested != 0) {
            if (requested < Format::kMinAllowedDegree || requested > Format::kMaxAllowedDegree) {
                throw std::invalid_argument("BTree: grau mínimo fora do intervalo permitido.");
            }
            if (Format::calculateMaxNodeSize(requested) > pageSize) {
                throw std::invalid_argument(
                    "BTree: um nó cheio com esse grau mínimo não cabe na página.");
            }
            return requested;
        }

        std::uint32_t degree = Format::kMinAllowedDegree;
        while (degree < Format::kMaxAllowedDegree &&
               Format::calculateMaxNodeSize(degree + 1) <= pageSize) {
            ++degree;
        }
        return degree;
    }

    // ------------------------------------------------------------------
    // Acesso a páginas (único ponto de contato com Page.hpp e com o PageManager)
    // ------------------------------------------------------------------

    /** Converte o identificador do nó (64 bits) no da página (32 bits). */
    static StoragePageId toStoragePageId(PageId nodeId) {
        if (nodeId == kNoPage || nodeId >= ::PageSystem::Page::kInvalidPageId) {
            throw ::PageSystem::Page::PageError(
                ::PageSystem::Page::ErrorCode::InvalidPageId,
                "identificador de nó não corresponde a uma página válida: " +
                    std::to_string(nodeId));
        }
        return static_cast<StoragePageId>(nodeId);
    }

    /** Recupera o nó armazenado na página nodeId (cópia em memória). */
    Node loadNode(PageId nodeId) const {
        const StoragePage page = pager_.getPage(toStoragePageId(nodeId));
        const std::vector<std::uint8_t> bytes = page.read(0, page.size());

        // A página tem tamanho fixo: depois do nó há bytes de preenchimento.
        Node node = ::BTreeNode::deserializeNodeFromPage(bytes);

        if (node.nodeId != nodeId) {
            throw ::BTreeNode::NodeFormatError(
                "A página " + std::to_string(nodeId) + " contém o nó " +
                std::to_string(node.nodeId) + ".");
        }
        if (node.minDegree != minDegree_) {
            throw ::BTreeNode::NodeFormatError(
                "Grau mínimo do nó (" + std::to_string(node.minDegree) +
                ") difere do esperado (" + std::to_string(minDegree_) + ").");
        }
        return node;
    }

    /** Persiste o nó na página node.nodeId. O nó é validado ao ser serializado. */
    void writeNode(const Node& node) {
        // updatePage substitui todo o conteúdo e zera o preenchimento após o nó.
        pager_.updatePage(toStoragePageId(node.nodeId), ::BTreeNode::serializeNode(node));
    }

    /** Solicita uma nova página para um nó e devolve seu identificador. */
    PageId allocateNode() {
        const StoragePageId id = pager_.allocatePage();
        if (id == ::PageSystem::Page::kInvalidPageId) {
            throw ::PageSystem::Page::PageError(
                ::PageSystem::Page::ErrorCode::InvalidPageId,
                "o PageManager devolveu o identificador reservado como página alocada");
        }
        return id;
    }

    /** Libera a página de um nó removido da árvore. */
    void freeNode(PageId nodeId) { pager_.freePage(toStoragePageId(nodeId)); }

    /** Página da raiz, ou kNoPage se a árvore estiver vazia. */
    PageId getRootPageId() const {
        const StoragePageId id = pager_.getRootPageId();
        return id == ::PageSystem::Page::kInvalidPageId ? kNoPage : PageId{id};
    }

    void setRootPageId(PageId nodeId) {
        pager_.setRootPageId(nodeId == kNoPage ? ::PageSystem::Page::kInvalidPageId
                                               : toStoragePageId(nodeId));
    }

    // ------------------------------------------------------------------
    // Utilitários de nó
    // ------------------------------------------------------------------

    /** Cria um nó vazio já associado a uma página recém-alocada (ainda não escrita). */
    Node createNode(bool leaf, bool root) {
        Node node;
        node.nodeId    = allocateNode();
        node.minDegree = minDegree_;
        node.isLeaf    = leaf;
        node.isRoot    = root;
        node.keys.reserve(maxKeys());
        node.values.reserve(maxKeys());
        if (!leaf) {
            node.children.reserve(maxKeys() + 1);
        }
        return node;
    }

    std::size_t maxKeys() const { return static_cast<std::size_t>(minDegree_) * 2 - 1; }

    bool isFull(const Node& node) const { return node.keys.size() == maxKeys(); }

    /** Índice da primeira chave >= key (ou keys.size()). */
    static std::size_t lowerIndex(const Node& node, Key key) {
        const auto it = std::lower_bound(node.keys.begin(), node.keys.end(), key);
        return static_cast<std::size_t>(std::distance(node.keys.begin(), it));
    }

    /** True se a entrada no índice i existe e possui exatamente a chave key. */
    static bool matchesAt(const Node& node, std::size_t i, Key key) {
        return i < node.keys.size() && node.keys[i] == key;
    }

    // keys e values são paralelos: estes três helpers são o único lugar que
    // os altera, para que nunca fiquem dessincronizados.

    static void insertEntryAt(Node& node, std::size_t index, const value_type& entry) {
        node.keys.insert(node.keys.begin() + static_cast<std::ptrdiff_t>(index), entry.first);
        node.values.insert(node.values.begin() + static_cast<std::ptrdiff_t>(index), entry.second);
    }

    static void appendEntry(Node& node, const value_type& entry) {
        node.keys.push_back(entry.first);
        node.values.push_back(entry.second);
    }

    static void eraseEntryAt(Node& node, std::size_t index) {
        node.keys.erase(node.keys.begin() + static_cast<std::ptrdiff_t>(index));
        node.values.erase(node.values.begin() + static_cast<std::ptrdiff_t>(index));
    }

    static value_type entryAt(const Node& node, std::size_t index) {
        return value_type(node.keys[index], node.values[index]);
    }

    static void setEntryAt(Node& node, std::size_t index, const value_type& entry) {
        node.keys[index]   = entry.first;
        node.values[index] = entry.second;
    }

    std::optional<Location> locate(Key key) const {
        PageId pageId = getRootPageId();
        while (pageId != kNoPage) {
            Node node = loadNode(pageId);
            const std::size_t i = lowerIndex(node, key);
            if (matchesAt(node, i, key)) {
                return Location{std::move(node), i};
            }
            if (node.isLeaf) {
                return std::nullopt;
            }
            pageId = node.children[i];
        }
        return std::nullopt;
    }

    value_type minEntry(const Node& node) const {
        if (node.isLeaf) {
            return entryAt(node, 0);
        }
        Node current = loadNode(node.children.front());
        while (!current.isLeaf) {
            const PageId next = current.children.front();
            current = loadNode(next);
        }
        return entryAt(current, 0);
    }

    value_type maxEntry(const Node& node) const {
        if (node.isLeaf) {
            return entryAt(node, node.keys.size() - 1);
        }
        Node current = loadNode(node.children.back());
        while (!current.isLeaf) {
            const PageId next = current.children.back();
            current = loadNode(next);
        }
        return entryAt(current, current.keys.size() - 1);
    }

    /** Libera recursivamente (pós-ordem) a página de um nó e de seus descendentes. */
    void releaseSubtree(PageId nodeId) {
        const Node node = loadNode(nodeId);
        if (!node.isLeaf) {
            for (const PageId childId : node.children) {
                releaseSubtree(childId);
            }
        }
        freeNode(nodeId);
    }

    // ------------------------------------------------------------------
    // Inserção
    // ------------------------------------------------------------------

    /**
     * Divide a raiz cheia: cria uma nova raiz com a mediana promovida. A antiga
     * raiz deixa de ser raiz (isRoot = false). Devolve a nova raiz.
     */
    Node splitRoot(Node& oldRoot) {
        Node newRoot = createNode(/*leaf=*/false, /*root=*/true);
        newRoot.children.push_back(oldRoot.nodeId);
        oldRoot.isRoot = false;

        splitChild(newRoot, 0, oldRoot);  // persiste newRoot, a antiga raiz e o irmão
        setRootPageId(newRoot.nodeId);
        return newRoot;
    }

    /**
     * Divide o filho cheio `fullChild` (= parent.children[index]) em dois nós
     * com t-1 chaves, promovendo a mediana para `parent` (que não pode estar
     * cheio). Persiste os três nós e devolve o novo irmão direito.
     */
    Node splitChild(Node& parent, std::size_t index, Node& fullChild) {
        Node right = createNode(fullChild.isLeaf, /*root=*/false);
        const std::size_t t = minDegree_;

        const value_type median = entryAt(fullChild, t - 1);

        right.keys.assign(fullChild.keys.begin() + t, fullChild.keys.end());
        right.values.assign(fullChild.values.begin() + t, fullChild.values.end());
        fullChild.keys.erase(fullChild.keys.begin() + (t - 1), fullChild.keys.end());
        fullChild.values.erase(fullChild.values.begin() + (t - 1), fullChild.values.end());

        if (!fullChild.isLeaf) {
            right.children.assign(fullChild.children.begin() + t, fullChild.children.end());
            fullChild.children.erase(fullChild.children.begin() + t, fullChild.children.end());
        }

        insertEntryAt(parent, index, median);
        parent.children.insert(parent.children.begin() + static_cast<std::ptrdiff_t>(index) + 1,
                               right.nodeId);

        // Ordem: o pai só passa a apontar para páginas que já foram escritas.
        writeNode(right);
        writeNode(fullChild);
        writeNode(parent);
        return right;
    }

    /** Insere em uma subárvore cuja raiz `node` não está cheia (chave inédita). */
    void insertNonFull(Node node, Key key, Value value) {
        while (!node.isLeaf) {
            const std::size_t i = lowerIndex(node, key);
            Node child = loadNode(node.children[i]);

            if (isFull(child)) {
                Node right = splitChild(node, i, child);
                // A mediana subiu para node.keys[i]; decide o lado.
                if (node.keys[i] < key) {
                    child = std::move(right);
                }
            }
            node = std::move(child);
        }

        insertEntryAt(node, lowerIndex(node, key), value_type(key, value));
        writeNode(node);
    }

    // ------------------------------------------------------------------
    // Remoção
    // ------------------------------------------------------------------

    /**
     * Remove a chave da subárvore de `node`, que é a cópia em memória
     * autoritativa (atualizada in-place e persistida quando modificada).
     * Pré-condição: `node` é a raiz ou possui ao menos t chaves.
     */
    bool removeFrom(Node& node, Key key) {
        const std::size_t i = lowerIndex(node, key);

        if (matchesAt(node, i, key)) {
            if (node.isLeaf) {
                eraseEntryAt(node, i);
                writeNode(node);
                return true;
            }
            return removeFromInternal(node, i, key);
        }

        if (node.isLeaf) {
            return false;
        }

        Node child = loadNode(node.children[i]);
        if (child.keys.size() < minDegree_) {
            child = fillChild(node, i, std::move(child));
        }
        return removeFrom(child, key);
    }

    /** A chave está em node.keys[i], e `node` é interno. */
    bool removeFromInternal(Node& node, std::size_t i, Key key) {
        Node left = loadNode(node.children[i]);

        if (left.keys.size() >= minDegree_) {
            // Substitui pelo predecessor e o remove da subárvore esquerda.
            const value_type predecessor = maxEntry(left);
            setEntryAt(node, i, predecessor);
            writeNode(node);
            return removeFrom(left, predecessor.first);
        }

        Node right = loadNode(node.children[i + 1]);

        if (right.keys.size() >= minDegree_) {
            const value_type successor = minEntry(right);
            setEntryAt(node, i, successor);
            writeNode(node);
            return removeFrom(right, successor.first);
        }

        // Ambos os filhos estão no mínimo: funde-os (a chave desce para o
        // nó fundido) e continua a remoção ali.
        mergeChildren(node, i, left, right);
        return removeFrom(left, key);
    }

    /**
     * Garante que `child` (= parent.children[i]) tenha ao menos t chaves,
     * emprestando de um irmão ou fundindo com ele. Devolve o nó que passou a
     * cobrir o intervalo original (é o irmão esquerdo quando a fusão ocorre
     * com ele). Todos os nós modificados são persistidos.
     */
    Node fillChild(Node& parent, std::size_t i, Node child) {
        if (i > 0) {
            Node left = loadNode(parent.children[i - 1]);
            if (left.keys.size() >= minDegree_) {
                borrowFromPrevious(parent, i, child, left);
                return child;
            }
            if (i == parent.keys.size()) {
                // Último filho: só existe irmão à esquerda.
                mergeChildren(parent, i - 1, left, child);
                return left;
            }
        }

        Node right = loadNode(parent.children[i + 1]);
        if (right.keys.size() >= minDegree_) {
            borrowFromNext(parent, i, child, right);
            return child;
        }
        mergeChildren(parent, i, child, right);
        return child;
    }

    /** Rotação à direita: a última chave do irmão esquerdo sobe e a do pai desce. */
    void borrowFromPrevious(Node& parent, std::size_t i, Node& child, Node& sibling) {
        insertEntryAt(child, 0, entryAt(parent, i - 1));
        if (!child.isLeaf) {
            child.children.insert(child.children.begin(), sibling.children.back());
            sibling.children.pop_back();
        }

        setEntryAt(parent, i - 1, entryAt(sibling, sibling.keys.size() - 1));
        eraseEntryAt(sibling, sibling.keys.size() - 1);

        writeNode(child);
        writeNode(sibling);
        writeNode(parent);
    }

    /** Rotação à esquerda: a primeira chave do irmão direito sobe e a do pai desce. */
    void borrowFromNext(Node& parent, std::size_t i, Node& child, Node& sibling) {
        appendEntry(child, entryAt(parent, i));
        if (!child.isLeaf) {
            child.children.push_back(sibling.children.front());
            sibling.children.erase(sibling.children.begin());
        }

        setEntryAt(parent, i, entryAt(sibling, 0));
        eraseEntryAt(sibling, 0);

        writeNode(child);
        writeNode(sibling);
        writeNode(parent);
    }

    /**
     * Funde `right` e a chave separadora parent.keys[i] em `left`
     * (= parent.children[i]). Ambos têm t-1 chaves, então o resultado tem 2t-1.
     *
     * Se `parent` era a raiz e ficou sem chaves, `left` vira a nova raiz: uma
     * raiz interna sem chaves seria rejeitada por validateNode, então o pai
     * nem chega a ser regravado; sua página é liberada.
     */
    void mergeChildren(Node& parent, std::size_t i, Node& left, Node& right) {
        appendEntry(left, entryAt(parent, i));
        left.keys.insert(left.keys.end(), right.keys.begin(), right.keys.end());
        left.values.insert(left.values.end(), right.values.begin(), right.values.end());
        if (!left.isLeaf) {
            left.children.insert(left.children.end(), right.children.begin(), right.children.end());
        }

        eraseEntryAt(parent, i);
        parent.children.erase(parent.children.begin() + static_cast<std::ptrdiff_t>(i) + 1);

        if (parent.isRoot && parent.keys.empty()) {
            left.isRoot = true;
            writeNode(left);
            setRootPageId(left.nodeId);  // só depois de `left` estar persistido como raiz
            freeNode(parent.nodeId);
            freeNode(right.nodeId);
            return;
        }

        writeNode(left);
        writeNode(parent);
        freeNode(right.nodeId);  // só depois de o pai deixar de referenciá-la
    }

    // ------------------------------------------------------------------
    // Consulta por intervalo e percurso
    // ------------------------------------------------------------------

    void collectRange(const Node& node, Key lower, Key upper, std::vector<value_type>& out) const {
        const std::size_t count = node.keys.size();

        // Pula todas as entradas (e subárvores) estritamente menores que lower.
        for (std::size_t i = lowerIndex(node, lower); i < count; ++i) {
            if (!node.isLeaf) {
                collectRange(loadNode(node.children[i]), lower, upper, out);
            }
            if (upper < node.keys[i]) {
                return;  // a partir daqui tudo é maior que upper
            }
            out.push_back(entryAt(node, i));
        }

        if (!node.isLeaf) {
            collectRange(loadNode(node.children[count]), lower, upper, out);
        }
    }

    template <typename Visitor>
    void inOrderImpl(const Node& node, Visitor& visit) const {
        const std::size_t count = node.keys.size();

        for (std::size_t i = 0; i < count; ++i) {
            if (!node.isLeaf) {
                inOrderImpl(loadNode(node.children[i]), visit);
            }
            visit(node.keys[i], node.values[i]);
        }

        if (!node.isLeaf) {
            inOrderImpl(loadNode(node.children[count]), visit);
        }
    }
};

}  // namespace BTree

#endif  // BTREE_HPP