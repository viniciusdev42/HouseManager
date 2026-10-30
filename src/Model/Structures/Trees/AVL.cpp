#include "Model/Structures/Trees/AVL.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace Structures::AVL {

// ============================================================================
// Altura e fator de balanceamento
// ============================================================================

template <typename T>
int AVLTree<T>::heightOf(const Node* node) noexcept {
    return node ? node->height : 0;
}

// Fator = altura(esquerda) - altura(direita).
// > 1 => desbalanceada à esquerda; < -1 => desbalanceada à direita.
template <typename T>
int AVLTree<T>::balanceFactor(const Node* node) noexcept {
    return node ? heightOf(node->left.get()) - heightOf(node->right.get()) : 0;
}

template <typename T>
void AVLTree<T>::updateHeight(Node* node) noexcept {
    node->height = 1 + std::max(heightOf(node->left.get()),
                                heightOf(node->right.get()));
}

// ============================================================================
// Rotações
// ============================================================================

/*
 *       y                x
 *      / \              / \
 *     x   C    ==>     A   y
 *    / \                  / \
 *   A   B                B   C
 */
template <typename T>
typename AVLTree<T>::NodePtr AVLTree<T>::rotateRight(NodePtr y) {
    NodePtr x = std::move(y->left);
    y->left = std::move(x->right);
    updateHeight(y.get());  // y agora é filho: sua altura deve ser atualizada primeiro
    x->right = std::move(y);
    updateHeight(x.get());
    return x;
}

/*
 *     x                    y
 *    / \                  / \
 *   A   y      ==>       x   C
 *      / \              / \
 *     B   C            A   B
 */
template <typename T>
typename AVLTree<T>::NodePtr AVLTree<T>::rotateLeft(NodePtr x) {
    NodePtr y = std::move(x->right);
    x->right = std::move(y->left);
    updateHeight(x.get());
    y->left = std::move(x);
    updateHeight(y.get());
    return y;
}

// Caso LR: o filho esquerdo está pesado à direita. Uma rotação simples não
// resolveria, então primeiro alinhamos o filho (esquerda) e depois rotacionamos
// a raiz (direita).
template <typename T>
typename AVLTree<T>::NodePtr AVLTree<T>::rotateLeftRight(NodePtr node) {
    node->left = rotateLeft(std::move(node->left));
    return rotateRight(std::move(node));
}

// Caso RL: simétrico ao LR.
template <typename T>
typename AVLTree<T>::NodePtr AVLTree<T>::rotateRightLeft(NodePtr node) {
    node->right = rotateRight(std::move(node->right));
    return rotateLeft(std::move(node));
}

// Restaura a invariante AVL em um nó cujas subárvores já são AVL e cuja
// diferença de altura é, no máximo, 2 (situação garantida após uma única
// inserção ou remoção).
template <typename T>
typename AVLTree<T>::NodePtr AVLTree<T>::rebalance(NodePtr node) {
    updateHeight(node.get());
    const int balance = balanceFactor(node.get());

    if (balance > 1) {
        // "<" 0 => caso LR. Com 0 (possível só na remoção) a rotação simples basta.
        if (balanceFactor(node->left.get()) < 0) {
            return rotateLeftRight(std::move(node));
        }
        return rotateRight(std::move(node));
    }

    if (balance < -1) {
        if (balanceFactor(node->right.get()) > 0) {
            return rotateRightLeft(std::move(node));
        }
        return rotateLeft(std::move(node));
    }

    return node;
}

// ============================================================================
// Inserção
// ============================================================================

template <typename T>
typename AVLTree<T>::NodePtr AVLTree<T>::insertNode(NodePtr node, const T& key,
                                                    bool& inserted) {
    if (!node) {
        inserted = true;
        return std::make_unique<Node>(key);
    }

    if (key < node->key) {
        node->left = insertNode(std::move(node->left), key, inserted);
    } else if (node->key < key) {
        node->right = insertNode(std::move(node->right), key, inserted);
    } else {
        inserted = false;  // duplicata: nada muda, não precisa rebalancear
        return node;
    }

    return rebalance(std::move(node));
}

template <typename T>
bool AVLTree<T>::insert(const T& key) {
    bool inserted = false;
    root_ = insertNode(std::move(root_), key, inserted);
    if (inserted) {
        ++count_;
    }
    return inserted;
}

// ============================================================================
// Remoção
// ============================================================================

// Desliga da subárvore o nó de menor chave, devolvendo-o em `detached`, e
// retorna a subárvore restante já rebalanceada.
template <typename T>
typename AVLTree<T>::NodePtr AVLTree<T>::detachMin(NodePtr node,
                                                   NodePtr& detached) {
    if (!node->left) {
        detached = std::move(node);
        return std::move(detached->right);  // o filho direito assume o lugar
    }
    node->left = detachMin(std::move(node->left), detached);
    return rebalance(std::move(node));
}

template <typename T>
typename AVLTree<T>::NodePtr AVLTree<T>::removeNode(NodePtr node, const T& key,
                                                    bool& removed) {
    if (!node) {
        removed = false;
        return nullptr;
    }

    if (key < node->key) {
        node->left = removeNode(std::move(node->left), key, removed);
    } else if (node->key < key) {
        node->right = removeNode(std::move(node->right), key, removed);
    } else {
        removed = true;

        // Zero ou um filho: o filho (ou nulo) substitui o nó.
        if (!node->left) {
            return std::move(node->right);
        }
        if (!node->right) {
            return std::move(node->left);
        }

        // Dois filhos: o sucessor em ordem (menor da subárvore direita) ocupa
        // o lugar do nó removido, preservando a ordenação da árvore.
        NodePtr successor;
        NodePtr newRight = detachMin(std::move(node->right), successor);
        successor->left = std::move(node->left);
        successor->right = std::move(newRight);
        node = std::move(successor);
    }

    return rebalance(std::move(node));
}

template <typename T>
bool AVLTree<T>::remove(const T& key) {
    bool removed = false;
    root_ = removeNode(std::move(root_), key, removed);
    if (removed) {
        --count_;
    }
    return removed;
}

// ============================================================================
// Busca, mínimo e máximo
// ============================================================================

template <typename T>
const T* AVLTree<T>::find(const T& key) const {
    const Node* current = root_.get();
    while (current) {
        if (key < current->key) {
            current = current->left.get();
        } else if (current->key < key) {
            current = current->right.get();
        } else {
            return &current->key;
        }
    }
    return nullptr;
}

template <typename T>
bool AVLTree<T>::contains(const T& key) const {
    return find(key) != nullptr;
}

template <typename T>
const typename AVLTree<T>::Node* AVLTree<T>::leftmost(const Node* node) noexcept {
    while (node->left) {
        node = node->left.get();
    }
    return node;
}

template <typename T>
const typename AVLTree<T>::Node* AVLTree<T>::rightmost(const Node* node) noexcept {
    while (node->right) {
        node = node->right.get();
    }
    return node;
}

template <typename T>
const T& AVLTree<T>::min() const {
    if (!root_) {
        throw std::out_of_range("AVLTree::min: a arvore esta vazia");
    }
    return leftmost(root_.get())->key;
}

template <typename T>
const T& AVLTree<T>::max() const {
    if (!root_) {
        throw std::out_of_range("AVLTree::max: a arvore esta vazia");
    }
    return rightmost(root_.get())->key;
}

// ============================================================================
// Percurso em ordem
// ============================================================================

template <typename T>
void AVLTree<T>::inOrderVisit(const Node* node,
                              const std::function<void(const T&)>& visit) {
    if (!node) {
        return;
    }
    inOrderVisit(node->left.get(), visit);
    visit(node->key);
    inOrderVisit(node->right.get(), visit);
}

template <typename T>
void AVLTree<T>::inOrder(const std::function<void(const T&)>& visit) const {
    inOrderVisit(root_.get(), visit);
}

template <typename T>
std::vector<T> AVLTree<T>::toSortedVector() const {
    std::vector<T> result;
    result.reserve(count_);
    inOrder([&result](const T& key) { result.push_back(key); });
    return result;
}

// ============================================================================
// Informações gerais
// ============================================================================

template <typename T>
std::size_t AVLTree<T>::size() const noexcept {
    return count_;
}

template <typename T>
bool AVLTree<T>::empty() const noexcept {
    return count_ == 0;
}

template <typename T>
int AVLTree<T>::height() const noexcept {
    return heightOf(root_.get());
}

template <typename T>
void AVLTree<T>::clear() noexcept {
    root_.reset();
    count_ = 0;
}

} // Namespace Structures::AVL 

// ============================================================================
// Instanciações explícitas (necessárias porque as definições estão neste .cpp)
// Para outro tipo, acrescente: template class AVLTree<MeuTipo>;
// ============================================================================

template class AVLTree<int>;
template class AVLTree<long>;
template class AVLTree<long long>;
template class AVLTree<double>;
template class AVLTree<std::string>;
