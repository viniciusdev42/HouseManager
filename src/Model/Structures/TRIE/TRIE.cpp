#ifndef STRUCTURES_TRIE_CPP
#define STRUCTURES_TRIE_CPP

#include "Model/Structures/TRIE.hpp"

#include <stdexcept>
#include <utility>

namespace {

// Remove a primeira ocorrência de `value`, preservando a ordem do vetor.
template <typename T, typename U>
bool eraseValue(std::vector<T>& values, const U& value) {
    const auto it = std::find(values.begin(), values.end(), value);
    if (it == values.end()) {
        return false;
    }
    values.erase(it);
    return true;
}

}  // namespace

Trie::Trie() : root_(std::make_unique<Node>()) {}

bool Trie::insert(std::string_view word, TaskId taskId) {
    if (word.empty()) {
        throw std::invalid_argument("Trie::insert: a palavra não pode ser vazia.");
    }

    Node* current = root_.get();
    for (char ch : word) {
        auto& child = current->children[ch];
        if (!child) {
            child = std::make_unique<Node>(ch);
        }
        current = child.get();
    }

    if (std::find(current->taskIds.begin(), current->taskIds.end(), taskId) !=
        current->taskIds.end()) {
        return false;
    }

    if (!current->hasWord()) {
        ++wordCount_;
    }
    current->taskIds.push_back(taskId);
    reverseIndex_[taskId].emplace_back(word);
    return true;
}

bool Trie::remove(std::string_view word, TaskId taskId) {
    if (word.empty()) {
        return false;
    }

    Node* node = findNode(word);
    if (node == nullptr || !eraseValue(node->taskIds, taskId)) {
        return false;
    }

    unlinkFromReverseIndex(taskId, word);

    if (!node->hasWord()) {
        --wordCount_;
        pruneBranch(word);
    }
    return true;
}

bool Trie::removeWord(std::string_view word) {
    if (word.empty()) {
        return false;
    }

    Node* node = findNode(word);
    if (node == nullptr || !node->hasWord()) {
        return false;
    }

    for (TaskId taskId : node->taskIds) {
        unlinkFromReverseIndex(taskId, word);
    }
    node->taskIds.clear();

    --wordCount_;
    pruneBranch(word);
    return true;
}

bool Trie::removeTask(TaskId taskId) {
    const auto it = reverseIndex_.find(taskId);
    if (it == reverseIndex_.end()) {
        return false;
    }

    // Movemos a lista e apagamos a entrada antes do loop: assim não é preciso
    // atualizar o índice reverso a cada palavra removida.
    const std::vector<std::string> words = std::move(it->second);
    reverseIndex_.erase(it);

    for (const std::string& word : words) {
        Node* node = findNode(word);
        // O índice reverso e a árvore são mantidos em sincronia; a checagem
        // protege contra nó ausente sem derrubar a operação inteira.
        if (node == nullptr || !eraseValue(node->taskIds, taskId)) {
            continue;
        }
        if (!node->hasWord()) {
            --wordCount_;
            pruneBranch(word);
        }
    }
    return true;
}

void Trie::updateTask(TaskId taskId, const std::vector<std::string>& newWords) {
    // Valida tudo antes de alterar qualquer estado.
    for (const std::string& word : newWords) {
        if (word.empty()) {
            throw std::invalid_argument(
                "Trie::updateTask: a lista contém uma palavra vazia.");
        }
    }

    // Cópia necessária: remove() altera o vetor do índice reverso durante o loop.
    const std::vector<std::string> oldWords = wordsOf(taskId);
    for (const std::string& oldWord : oldWords) {
        if (std::find(newWords.begin(), newWords.end(), oldWord) == newWords.end()) {
            remove(oldWord, taskId);
        }
    }

    // insert() ignora associações que já existem, preservando as palavras mantidas.
    for (const std::string& word : newWords) {
        insert(word, taskId);
    }
}

bool Trie::search(std::string_view word) const {
    if (word.empty()) {
        return false;
    }
    const Node* node = findNode(word);
    return node != nullptr && node->hasWord();
}

bool Trie::startsWith(std::string_view prefix) const {
    if (prefix.empty()) {
        return !empty();
    }
    return findNode(prefix) != nullptr;
}

std::vector<TaskId> Trie::findTaskIds(std::string_view word) const {
    if (word.empty()) {
        return {};
    }
    const Node* node = findNode(word);
    return node != nullptr ? node->taskIds : std::vector<TaskId>{};
}

std::vector<std::string> Trie::wordsOf(TaskId taskId) const {
    const auto it = reverseIndex_.find(taskId);
    return it != reverseIndex_.end() ? it->second : std::vector<std::string>{};
}

std::vector<std::string> Trie::autocomplete(std::string_view prefix,
                                            std::size_t maxResults) const {
    std::vector<std::string> results;

    const Node* start = findNode(prefix);
    if (start == nullptr) {
        return results;
    }

    std::string currentWord(prefix);
    collectWords(start, currentWord, results, maxResults);
    return results;
}

std::size_t Trie::size() const noexcept {
    return wordCount_;
}

bool Trie::empty() const noexcept {
    return wordCount_ == 0;
}

void Trie::clear() {
    root_ = std::make_unique<Node>();
    wordCount_ = 0;
    reverseIndex_.clear();
}

const Trie::Node* Trie::findNode(std::string_view prefix) const {
    const Node* current = root_.get();
    for (char ch : prefix) {
        const auto it = current->children.find(ch);
        if (it == current->children.end()) {
            return nullptr;
        }
        current = it->second.get();
    }
    return current;
}

Trie::Node* Trie::findNode(std::string_view prefix) {
    // Reaproveita a versão const para não duplicar a lógica de navegação.
    return const_cast<Node*>(std::as_const(*this).findNode(prefix));
}

void Trie::pruneBranch(std::string_view word) {
    // Caminho iterativo (sem recursão) para não estourar a pilha em palavras longas.
    std::vector<Node*> path;
    path.reserve(word.size() + 1);

    Node* current = root_.get();
    path.push_back(current);
    for (char ch : word) {
        const auto it = current->children.find(ch);
        if (it == current->children.end()) {
            break;
        }
        current = it->second.get();
        path.push_back(current);
    }

    // Para no primeiro nó ainda útil para outra palavra. A raiz nunca é removida.
    for (std::size_t i = path.size() - 1; i > 0; --i) {
        Node* node = path[i];
        if (node->hasWord() || !node->children.empty()) {
            break;
        }
        path[i - 1]->children.erase(node->character);
    }
}

void Trie::unlinkFromReverseIndex(TaskId taskId, std::string_view word) {
    const auto it = reverseIndex_.find(taskId);
    if (it == reverseIndex_.end()) {
        return;
    }

    eraseValue(it->second, word);
    if (it->second.empty()) {
        reverseIndex_.erase(it);
    }
}

void Trie::collectWords(const Node* node,
                        std::string& currentWord,
                        std::vector<std::string>& results,
                        std::size_t maxResults) const {
    const auto limitReached = [&]() {
        return maxResults != 0 && results.size() >= maxResults;
    };

    if (limitReached()) {
        return;
    }

    if (node->hasWord()) {
        results.push_back(currentWord);
    }

    for (const auto& [ch, child] : node->children) {
        if (limitReached()) {
            return;
        }
        currentWord.push_back(ch);
        collectWords(child.get(), currentWord, results, maxResults);
        currentWord.pop_back();
    }
}
