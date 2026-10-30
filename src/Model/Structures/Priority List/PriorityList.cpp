#ifndef STRUCTURES_PRIORITY_LIST_CPP
#define STRUCTURES_PRIORITY_LIST_CPP

#include "Model/Structures/PriorityList.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace Structures::PriorityList {


// ---------------------------------------------------------------------------
// Construção
// ---------------------------------------------------------------------------

PriorityList::PriorityList(std::vector<Element> elements)
    : heap_(std::move(elements)) {
    positionById_.reserve(heap_.size());

    for (std::size_t i = 0; i < heap_.size(); ++i) {
        const bool inserted = positionById_.emplace(heap_[i].id, i).second;
        if (!inserted) {
            throw std::invalid_argument(
                "PriorityList: id duplicado na colecao inicial: " +
                std::to_string(heap_[i].id));
        }
    }

    // Heapify bottom-up: começa no último nó interno e desce até a raiz.
    // É O(n), mais barato que n inserções sucessivas (O(n log n)).
    for (std::size_t i = heap_.size() / 2; i > 0; --i) {
        siftDown(i - 1);
    }
}

// ---------------------------------------------------------------------------
// Operações públicas
// ---------------------------------------------------------------------------

void PriorityList::insert(const Element& element) {
    if (contains(element.id)) {
        throw std::invalid_argument(
            "PriorityList::insert: id ja existe: " + std::to_string(element.id));
    }

    heap_.push_back(element);
    const std::size_t index = heap_.size() - 1;
    positionById_[element.id] = index;
    siftUp(index);
}

const Element& PriorityList::peek() const {
    if (heap_.empty()) {
        throw std::out_of_range("PriorityList::peek: lista vazia");
    }
    return heap_.front();
}

Element PriorityList::extractTop() {
    if (heap_.empty()) {
        throw std::out_of_range("PriorityList::extractTop: lista vazia");
    }
    return removeAt(0);
}

void PriorityList::updatePriority(int id, int newPriority) {
    const std::size_t index = findPosition(id);
    heap_[index].priority = newPriority;
    restoreHeapProperty(index);
}

Element PriorityList::remove(int id) {
    return removeAt(findPosition(id));
}

bool PriorityList::contains(int id) const {
    return positionById_.find(id) != positionById_.end();
}

std::size_t PriorityList::size() const noexcept {
    return heap_.size();
}

bool PriorityList::empty() const noexcept {
    return heap_.empty();
}

// ---------------------------------------------------------------------------
// Auxiliares privados
// ---------------------------------------------------------------------------

std::size_t PriorityList::parentOf(std::size_t index) noexcept {
    return (index - 1) / 2;
}

std::size_t PriorityList::leftChildOf(std::size_t index) noexcept {
    return 2 * index + 1;
}

bool PriorityList::hasHigherPriority(std::size_t a, std::size_t b) const {
    return heap_[a].priority > heap_[b].priority;
}

void PriorityList::swapNodes(std::size_t a, std::size_t b) {
    std::swap(heap_[a], heap_[b]);
    positionById_[heap_[a].id] = a;
    positionById_[heap_[b].id] = b;
}

// Sobe o nó enquanto ele tiver prioridade maior que a do pai.
// Retorna a posição final do nó.
std::size_t PriorityList::siftUp(std::size_t index) {
    while (index > 0) {
        const std::size_t parent = parentOf(index);
        if (!hasHigherPriority(index, parent)) {
            break;
        }
        swapNodes(index, parent);
        index = parent;
    }
    return index;
}

// Desce o nó trocando-o sempre com o filho de maior prioridade.
void PriorityList::siftDown(std::size_t index) {
    const std::size_t count = heap_.size();

    while (true) {
        const std::size_t left = leftChildOf(index);
        if (left >= count) {
            break;  // nó folha
        }

        std::size_t best = index;
        if (hasHigherPriority(left, best)) {
            best = left;
        }
        const std::size_t right = left + 1;
        if (right < count && hasHigherPriority(right, best)) {
            best = right;
        }

        if (best == index) {
            break;
        }
        swapNodes(index, best);
        index = best;
    }
}

// Após alterar o valor de um nó, ele só pode violar o heap em UMA direção:
// se subiu de prioridade, siftUp resolve; senão, siftDown resolve.
void PriorityList::restoreHeapProperty(std::size_t index) {
    if (siftUp(index) == index) {
        siftDown(index);
    }
}

std::size_t PriorityList::findPosition(int id) const {
    const auto it = positionById_.find(id);
    if (it == positionById_.end()) {
        throw std::invalid_argument(
            "PriorityList: id nao encontrado: " + std::to_string(id));
    }
    return it->second;
}

// Remove o nó da posição `index`: substitui-o pelo último elemento
// (mantém a árvore completa) e reequilibra a partir dessa posição.
Element PriorityList::removeAt(std::size_t index) {
    const std::size_t last = heap_.size() - 1;
    Element removed = std::move(heap_[index]);
    positionById_.erase(removed.id);

    if (index != last) {
        heap_[index] = std::move(heap_[last]);
        positionById_[heap_[index].id] = index;
    }
    heap_.pop_back();

    if (index < heap_.size()) {
        restoreHeapProperty(index);
    }
    return removed;
}

} // Namespace Structures::PriorityList
