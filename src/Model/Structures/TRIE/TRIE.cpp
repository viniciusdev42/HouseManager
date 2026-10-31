#ifndef STRUCTURES_TRIE_CPP
#define STRUCTURES_TRIE_CPP

#include "Model/Structures/TRIE.hpp"

#include <stdexcept>
#include <utility>

Trie::Trie() : root_(std::make_unique<Node>()) {}

bool Trie::insert(std::string_view word) {
    if (word.empty()) {
        throw std::invalid_argument("Trie::insert: a palavra não pode ser vazia.");
    }

    Node* current = root_.get();
    for (char ch : word) {
        auto& child = current->children[ch];
        if (!child) {
            child = std::make_unique<Node>();
        }
        current = child.get();
    }

    if (current->isEndOfWord) {
        return false;
    }

    current->isEndOfWord = true;
    ++wordCount_;
    return true;
}

bool Trie::remove(std::string_view word) {
    if (word.empty()) {
        return false;
    }

    // Guardamos o caminho percorrido (pai + caractere) para poder podar
    // de baixo para cima sem recursão, evitando estouro de pilha em palavras longas.
    std::vector<std::pair<Node*, char>> path;
    path.reserve(word.size());

    Node* current = root_.get();
    for (char ch : word) {
        auto it = current->children.find(ch);
        if (it == current->children.end()) {
            return false;
        }
        path.emplace_back(current, ch);
        current = it->second.get();
    }

    if (!current->isEndOfWord) {
        return false;
    }

    current->isEndOfWord = false;
    --wordCount_;

    // Poda: remove nós que não terminam palavra e não têm filhos.
    // Para ao encontrar um nó que ainda seja útil para outras palavras.
    for (auto it = path.rbegin(); it != path.rend(); ++it) {
        Node* parent = it->first;
        Node* child = parent->children[it->second].get();

        if (child->isEndOfWord || !child->children.empty()) {
            break;
        }
        parent->children.erase(it->second);
    }

    return true;
}

bool Trie::search(std::string_view word) const {
    if (word.empty()) {
        return false;
    }
    const Node* node = findNode(word);
    return node != nullptr && node->isEndOfWord;
}

bool Trie::startsWith(std::string_view prefix) const {
    if (prefix.empty()) {
        return !empty();
    }
    return findNode(prefix) != nullptr;
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
}

const Trie::Node* Trie::findNode(std::string_view prefix) const {
    const Node* current = root_.get();
    for (char ch : prefix) {
        auto it = current->children.find(ch);
        if (it == current->children.end()) {
            return nullptr;
        }
        current = it->second.get();
    }
    return current;
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

    if (node->isEndOfWord) {
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
