/**
 * Árvore AVL (árvore binária de busca auto-balanceada).
 *
 * Invariante: para todo nó, |altura(esquerda) - altura(direita)| <= 1.
 * Essa invariante garante altura <= ~1,44 * log2(n + 2), portanto
 * inserção, remoção e busca executam em O(log n) no pior caso.
 *
 * Convenção de altura: árvore vazia = 0, folha = 1.
 *
 * Requisitos do tipo T:
 *   - copiável;
 *   - comparável por operator< (a igualdade é derivada: !(a<b) && !(b<a)).
 *
 * Chaves duplicadas não são inseridas (comportamento de conjunto).
 *
 * Observação sobre a separação .hpp/.cpp: como AVLTree é um template, suas
 * definições ficam em avl_tree.cpp e são instanciadas explicitamente ao final
 * daquele arquivo para os tipos int, long, long long, double e std::string.
 * Para usar outro tipo, adicione uma linha "template class AVLTree<Tipo>;"
 * no final de avl_tree.cpp.
 */

#ifndef STRUCTURES_AVL_HPP
#define STRUCTURES_AVL_HPP

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace Structures::AVL {

template <typename T>
class AVLTree {
public:
    AVLTree() = default;
    ~AVLTree() = default;

    // Cópia desabilitada para evitar cópias profundas acidentais (YAGNI).
    AVLTree(const AVLTree&) = delete;
    AVLTree& operator=(const AVLTree&) = delete;
    AVLTree(AVLTree&&) noexcept = default;
    AVLTree& operator=(AVLTree&&) noexcept = default;

    // --- Operações fundamentais: O(log n) ---

    /** Insere a chave. Retorna false se ela já existia. */
    bool insert(const T& key);

    /** Remove a chave. Retorna false se ela não existia. */
    bool remove(const T& key);

    /** Retorna ponteiro para a chave armazenada, ou nullptr se não existir. */
    const T* find(const T& key) const;

    /** Retorna true se a chave existe na árvore. */
    bool contains(const T& key) const;

    /** Menor chave. Lança std::out_of_range se a árvore estiver vazia. */
    const T& min() const;

    /** Maior chave. Lança std::out_of_range se a árvore estiver vazia. */
    const T& max() const;

    // --- Percurso: O(n) ---

    /** Percurso em ordem (esquerda, raiz, direita): chaves em ordem crescente. */
    void inOrder(const std::function<void(const T&)>& visit) const;

    /** Conveniência: devolve as chaves em ordem crescente. */
    std::vector<T> toSortedVector() const;

    // --- Informações: O(1) ---

    std::size_t size() const noexcept;
    bool empty() const noexcept;

    /** Altura da árvore (vazia = 0, folha = 1). */
    int height() const noexcept;

    /** Remove todos os elementos. */
    void clear() noexcept;

private:
    struct Node {
        T key;
        int height = 1;
        std::unique_ptr<Node> left;
        std::unique_ptr<Node> right;

        explicit Node(const T& k) : key(k) {}
    };
    using NodePtr = std::unique_ptr<Node>;

    // --- Auxiliares de altura e balanceamento ---
    static int heightOf(const Node* node) noexcept;
    static int balanceFactor(const Node* node) noexcept;
    static void updateHeight(Node* node) noexcept;

    // --- Rotações (cada uma devolve a nova raiz da subárvore) ---
    static NodePtr rotateRight(NodePtr node);      // rotação simples à direita (caso LL)
    static NodePtr rotateLeft(NodePtr node);       // rotação simples à esquerda (caso RR)
    static NodePtr rotateLeftRight(NodePtr node);  // rotação dupla esquerda-direita (caso LR)
    static NodePtr rotateRightLeft(NodePtr node);  // rotação dupla direita-esquerda (caso RL)
    static NodePtr rebalance(NodePtr node);

    // --- Operações recursivas (profundidade O(log n) graças ao balanceamento) ---
    static NodePtr insertNode(NodePtr node, const T& key, bool& inserted);
    static NodePtr removeNode(NodePtr node, const T& key, bool& removed);
    static NodePtr detachMin(NodePtr node, NodePtr& detached);

    static const Node* leftmost(const Node* node) noexcept;
    static const Node* rightmost(const Node* node) noexcept;
    static void inOrderVisit(const Node* node,
                             const std::function<void(const T&)>& visit);

    NodePtr root_;
    std::size_t count_ = 0;
};

} // Namespace Structures::AVL

#endif  //STRUCTURES_AVL_HPP
