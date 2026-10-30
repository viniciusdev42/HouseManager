#ifndef STRUCTURES_HASH_HASH_TABLE_CPP
#define STRUCTURES_HASH_HASH_TABLE_CPP

#include "HashTable.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Structures::HashTable {

// ===========================================================================
// Construção, cópia, movimento e destruição
// ===========================================================================

template <typename K, typename V, typename H, typename E>
HashTable<K, V, H, E>::HashTable(CollisionStrategy collision_strategy,
                                 size_type initial_capacity,
                                 std::optional<double> max_load,
                                 H hash_fn,
                                 E equal_fn)
    : strategy_(collision_strategy),
      hasher_(std::move(hash_fn)),
      key_equal_(std::move(equal_fn)),
      max_load_factor_(max_load.value_or(default_load_factor(collision_strategy))) {
    validate_load_factor(max_load_factor_, strategy_);
    allocate(round_capacity(initial_capacity));
}

template <typename K, typename V, typename H, typename E>
HashTable<K, V, H, E>::HashTable(const HashTable& other)
    : strategy_(other.strategy_),
      hasher_(other.hasher_),
      key_equal_(other.key_equal_),
      max_load_factor_(other.max_load_factor_),
      capacity_(other.capacity_),
      size_(other.size_),
      tombstones_(other.tombstones_),
      collisions_(other.collisions_),
      rehashes_(other.rehashes_) {
    if (capacity_ == 0) {
        return;
    }

    if (strategy_ == CollisionStrategy::SeparateChaining) {
        buckets_ = std::make_unique<Node*[]>(capacity_);
        try {
            for (size_type i = 0; i < capacity_; ++i) {
                // Preserva a ordem original de cada cadeia.
                Node** tail = &buckets_[i];
                for (const Node* src = other.buckets_[i]; src != nullptr; src = src->next) {
                    *tail = new Node(src->entry.key, src->entry.value, src->hash);
                    tail = &(*tail)->next;
                }
            }
        } catch (...) {
            destroy_nodes();
            throw;
        }
    } else {
        slots_ = std::make_unique<Slot[]>(capacity_);
        for (size_type i = 0; i < capacity_; ++i) {
            const Slot& src = other.slots_[i];
            Slot& dst = slots_[i];
            if (src.entry.has_value()) {
                dst.entry.emplace(*src.entry);
            }
            dst.hash = src.hash;
            dst.state = src.state;
        }
    }
}

template <typename K, typename V, typename H, typename E>
HashTable<K, V, H, E>::HashTable(HashTable&& other) noexcept
    : strategy_(other.strategy_),
      hasher_(std::move(other.hasher_)),
      key_equal_(std::move(other.key_equal_)),
      max_load_factor_(other.max_load_factor_),
      capacity_(std::exchange(other.capacity_, 0)),
      size_(std::exchange(other.size_, 0)),
      tombstones_(std::exchange(other.tombstones_, 0)),
      collisions_(std::exchange(other.collisions_, 0)),
      rehashes_(std::exchange(other.rehashes_, 0)),
      buckets_(std::move(other.buckets_)),
      slots_(std::move(other.slots_)) {}

template <typename K, typename V, typename H, typename E>
auto HashTable<K, V, H, E>::operator=(const HashTable& other) -> HashTable& {
    if (this != &other) {
        HashTable copy(other);
        swap(copy);
    }
    return *this;
}

template <typename K, typename V, typename H, typename E>
auto HashTable<K, V, H, E>::operator=(HashTable&& other) noexcept -> HashTable& {
    if (this != &other) {
        HashTable moved(std::move(other));
        swap(moved);
    }
    return *this;
}

template <typename K, typename V, typename H, typename E>
HashTable<K, V, H, E>::~HashTable() {
    destroy_nodes();
}

template <typename K, typename V, typename H, typename E>
void HashTable<K, V, H, E>::swap(HashTable& other) noexcept {
    using std::swap;
    swap(strategy_, other.strategy_);
    swap(hasher_, other.hasher_);
    swap(key_equal_, other.key_equal_);
    swap(max_load_factor_, other.max_load_factor_);
    swap(capacity_, other.capacity_);
    swap(size_, other.size_);
    swap(tombstones_, other.tombstones_);
    swap(collisions_, other.collisions_);
    swap(rehashes_, other.rehashes_);
    swap(buckets_, other.buckets_);
    swap(slots_, other.slots_);
}

// ===========================================================================
// Operações públicas
// ===========================================================================

template <typename K, typename V, typename H, typename E>
bool HashTable<K, V, H, E>::insert(K key, V value) {
    const std::uint64_t hash_value = hash_of(key);
    if (lookup(key, hash_value) != nullptr) {
        return false;
    }
    emplace_new(std::move(key), std::move(value), hash_value);
    return true;
}

template <typename K, typename V, typename H, typename E>
bool HashTable<K, V, H, E>::insert_or_assign(K key, V value) {
    const std::uint64_t hash_value = hash_of(key);
    if (V* existing = lookup(key, hash_value)) {
        *existing = std::move(value);
        return false;
    }
    emplace_new(std::move(key), std::move(value), hash_value);
    return true;
}

template <typename K, typename V, typename H, typename E>
bool HashTable<K, V, H, E>::erase(const K& key) {
    if (capacity_ == 0) {
        return false;
    }
    const std::uint64_t hash_value = hash_of(key);

    if (strategy_ == CollisionStrategy::SeparateChaining) {
        // 'link' aponta para o ponteiro que referencia o nó atual,
        // o que dispensa tratar o primeiro nó como caso especial.
        Node** link = &buckets_[index_for(hash_value, capacity_)];
        while (*link != nullptr) {
            Node* node = *link;
            if (node->hash == hash_value && key_equal_(node->entry.key, key)) {
                *link = node->next;
                delete node;
                --size_;
                return true;
            }
            link = &node->next;
        }
        return false;
    }

    const size_type mask = capacity_ - 1;
    const size_type position = find_slot(key, hash_value);
    if (position == kNpos) {
        return false;
    }

    Slot& slot = slots_[position];
    slot.entry.reset();
    --size_;

    if (slots_[(position + 1) & mask].state == SlotState::Empty) {
        // Nenhuma sequência de sondagem atravessa este slot em direção ao
        // próximo, então ele pode voltar a Empty. Tombstones imediatamente
        // anteriores ficam na mesma situação e também são liberados.
        slot.state = SlotState::Empty;
        size_type previous = (position + capacity_ - 1) & mask;
        while (slots_[previous].state == SlotState::Deleted) {
            slots_[previous].state = SlotState::Empty;
            --tombstones_;
            previous = (previous + capacity_ - 1) & mask;
        }
    } else {
        slot.state = SlotState::Deleted;
        ++tombstones_;
    }
    return true;
}

template <typename K, typename V, typename H, typename E>
void HashTable<K, V, H, E>::clear() noexcept {
    if (strategy_ == CollisionStrategy::SeparateChaining) {
        destroy_nodes();
    } else if (slots_) {
        for (size_type i = 0; i < capacity_; ++i) {
            slots_[i].entry.reset();
            slots_[i].state = SlotState::Empty;
        }
    }
    size_ = 0;
    tombstones_ = 0;
}

template <typename K, typename V, typename H, typename E>
void HashTable<K, V, H, E>::reserve(size_type element_count) {
    const size_type required = capacity_for(element_count);
    if (required > capacity_) {
        rehash_to(required);
    }
}

template <typename K, typename V, typename H, typename E>
void HashTable<K, V, H, E>::rehash(size_type min_buckets) {
    rehash_to(std::max(round_capacity(min_buckets), capacity_for(size_)));
}

template <typename K, typename V, typename H, typename E>
V* HashTable<K, V, H, E>::find(const K& key) {
    return lookup(key, hash_of(key));
}

template <typename K, typename V, typename H, typename E>
const V* HashTable<K, V, H, E>::find(const K& key) const {
    return lookup(key, hash_of(key));
}

template <typename K, typename V, typename H, typename E>
bool HashTable<K, V, H, E>::contains(const K& key) const {
    return find(key) != nullptr;
}

template <typename K, typename V, typename H, typename E>
V& HashTable<K, V, H, E>::at(const K& key) {
    if (V* value = lookup(key, hash_of(key))) {
        return *value;
    }
    throw std::out_of_range("HashTable::at: chave inexistente");
}

template <typename K, typename V, typename H, typename E>
const V& HashTable<K, V, H, E>::at(const K& key) const {
    if (const V* value = lookup(key, hash_of(key))) {
        return *value;
    }
    throw std::out_of_range("HashTable::at: chave inexistente");
}

template <typename K, typename V, typename H, typename E>
V& HashTable<K, V, H, E>::operator[](const K& key) {
    const std::uint64_t hash_value = hash_of(key);
    if (V* existing = lookup(key, hash_value)) {
        return *existing;
    }
    return *emplace_new(K(key), V(), hash_value);
}

template <typename K, typename V, typename H, typename E>
template <typename Func>
void HashTable<K, V, H, E>::for_each(Func&& func) {
    if (strategy_ == CollisionStrategy::SeparateChaining) {
        for (size_type i = 0; i < capacity_; ++i) {
            for (Node* node = buckets_[i]; node != nullptr; node = node->next) {
                const K& key = node->entry.key;  // chave imutável
                func(key, node->entry.value);
            }
        }
    } else {
        for (size_type i = 0; i < capacity_; ++i) {
            Slot& slot = slots_[i];
            if (slot.state == SlotState::Occupied) {
                const K& key = slot.entry->key;
                func(key, slot.entry->value);
            }
        }
    }
}

template <typename K, typename V, typename H, typename E>
template <typename Func>
void HashTable<K, V, H, E>::for_each(Func&& func) const {
    if (strategy_ == CollisionStrategy::SeparateChaining) {
        for (size_type i = 0; i < capacity_; ++i) {
            for (const Node* node = buckets_[i]; node != nullptr; node = node->next) {
                const Entry& entry = node->entry;
                func(entry.key, entry.value);
            }
        }
    } else {
        for (size_type i = 0; i < capacity_; ++i) {
            const Slot& slot = slots_[i];
            if (slot.state == SlotState::Occupied) {
                func(slot.entry->key, slot.entry->value);
            }
        }
    }
}

template <typename K, typename V, typename H, typename E>
void HashTable<K, V, H, E>::set_max_load_factor(double new_limit) {
    validate_load_factor(new_limit, strategy_);
    max_load_factor_ = new_limit;

    const double limit = max_load_factor_ * static_cast<double>(capacity_);
    if (static_cast<double>(size_) > limit) {
        rehash_to(capacity_for(size_));
    } else if (strategy_ == CollisionStrategy::OpenAddressing &&
               static_cast<double>(size_ + tombstones_) > limit) {
        rehash_to(capacity_);
    }
}

template <typename K, typename V, typename H, typename E>
HashTableStats HashTable<K, V, H, E>::stats() const {
    HashTableStats result;
    result.size = size_;
    result.bucket_count = capacity_;
    result.load_factor = load_factor();
    result.tombstones = tombstones_;
    result.collisions = collisions_;
    result.rehashes = rehashes_;
    result.longest_probe = longest_probe();
    return result;
}

template <typename K, typename V, typename H, typename E>
void HashTable<K, V, H, E>::reset_stats() noexcept {
    collisions_ = 0;
    rehashes_ = 0;
}

// ===========================================================================
// Utilitários estáticos
// ===========================================================================

template <typename K, typename V, typename H, typename E>
double HashTable<K, V, H, E>::default_load_factor(CollisionStrategy policy) noexcept {
    return policy == CollisionStrategy::SeparateChaining ? kDefaultChainingLoadFactor
                                                         : kDefaultOpenAddressingLoadFactor;
}

template <typename K, typename V, typename H, typename E>
void HashTable<K, V, H, E>::validate_load_factor(double value, CollisionStrategy policy) {
    if (!std::isfinite(value) || value <= 0.0) {
        throw std::invalid_argument("HashTable: o fator de carga deve ser positivo e finito");
    }
    // Em endereçamento aberto precisa sobrar ao menos um slot vazio, caso
    // contrário a sondagem de buscas por chaves ausentes não terminaria.
    if (policy == CollisionStrategy::OpenAddressing && value > kMaxOpenAddressingLoadFactor) {
        throw std::invalid_argument("HashTable: em endereçamento aberto o fator de carga deve ser <= 0.95");
    }
}

template <typename K, typename V, typename H, typename E>
auto HashTable<K, V, H, E>::round_capacity(size_type requested) -> size_type {
    if (requested > kMaxCapacity) {
        throw std::length_error("HashTable: capacidade solicitada excede o máximo");
    }
    size_type capacity = kMinCapacity;
    while (capacity < requested) {
        capacity <<= 1;
    }
    return capacity;
}

// Finalizador do SplitMix64: espalha os bits de entrada por toda a palavra.
template <typename K, typename V, typename H, typename E>
std::uint64_t HashTable<K, V, H, E>::mix(std::uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

template <typename K, typename V, typename H, typename E>
auto HashTable<K, V, H, E>::index_for(std::uint64_t hash_value, size_type capacity) noexcept
    -> size_type {
    return static_cast<size_type>(hash_value) & (capacity - 1);  // capacidade é potência de dois
}

// ===========================================================================
// Núcleo interno
// ===========================================================================

template <typename K, typename V, typename H, typename E>
std::uint64_t HashTable<K, V, H, E>::hash_of(const K& key) const {
    return mix(static_cast<std::uint64_t>(hasher_(key)));
}

// Menor capacidade (potência de dois) que comporta 'element_count'
// elementos sem ultrapassar o fator de carga máximo.
template <typename K, typename V, typename H, typename E>
auto HashTable<K, V, H, E>::capacity_for(size_type element_count) const -> size_type {
    size_type capacity = kMinCapacity;
    while (static_cast<double>(element_count) > max_load_factor_ * static_cast<double>(capacity)) {
        if (capacity >= kMaxCapacity) {
            throw std::length_error("HashTable: capacidade solicitada excede o máximo");
        }
        capacity <<= 1;
    }
    return capacity;
}

template <typename K, typename V, typename H, typename E>
void HashTable<K, V, H, E>::allocate(size_type capacity) {
    if (strategy_ == CollisionStrategy::SeparateChaining) {
        buckets_ = std::make_unique<Node*[]>(capacity);  // inicializa com nullptr
        slots_.reset();
    } else {
        slots_ = std::make_unique<Slot[]>(capacity);  // todos Empty
        buckets_.reset();
    }
    capacity_ = capacity;
}

template <typename K, typename V, typename H, typename E>
void HashTable<K, V, H, E>::destroy_nodes() noexcept {
    if (!buckets_) {
        return;
    }
    for (size_type i = 0; i < capacity_; ++i) {
        Node* node = buckets_[i];
        while (node != nullptr) {
            Node* next = node->next;
            delete node;
            node = next;
        }
        buckets_[i] = nullptr;
    }
}

// Chamado antes de cada inserção de chave nova.
template <typename K, typename V, typename H, typename E>
void HashTable<K, V, H, E>::grow_if_needed() {
    const double limit = max_load_factor_ * static_cast<double>(capacity_);

    if (static_cast<double>(size_ + 1) > limit) {
        rehash_to(capacity_for(size_ + 1));
    } else if (strategy_ == CollisionStrategy::OpenAddressing &&
               static_cast<double>(size_ + tombstones_ + 1) > limit) {
        // Há espaço lógico, mas tombstones consomem slots: reconstrói
        // na mesma capacidade para eliminá-los.
        rehash_to(capacity_);
    }
}

// Reconstrói a tabela com 'new_capacity' (potência de dois). Se a
// alocação falhar, a tabela original permanece intacta.
template <typename K, typename V, typename H, typename E>
void HashTable<K, V, H, E>::rehash_to(size_type new_capacity) {
    if (strategy_ == CollisionStrategy::SeparateChaining) {
        auto new_buckets = std::make_unique<Node*[]>(new_capacity);
        // Reaproveita os nós existentes (sem realocar) e os hashes armazenados.
        for (size_type i = 0; i < capacity_; ++i) {
            Node* node = buckets_[i];
            while (node != nullptr) {
                Node* next = node->next;
                const size_type index = index_for(node->hash, new_capacity);
                node->next = new_buckets[index];
                new_buckets[index] = node;
                node = next;
            }
        }
        buckets_ = std::move(new_buckets);
    } else {
        auto new_slots = std::make_unique<Slot[]>(new_capacity);
        const size_type mask = new_capacity - 1;
        for (size_type i = 0; i < capacity_; ++i) {
            Slot& old_slot = slots_[i];
            if (old_slot.state != SlotState::Occupied) {
                continue;
            }
            size_type position = index_for(old_slot.hash, new_capacity);
            while (new_slots[position].state == SlotState::Occupied) {
                position = (position + 1) & mask;
            }
            Slot& target = new_slots[position];
            target.entry.emplace(std::move(*old_slot.entry));
            target.hash = old_slot.hash;
            target.state = SlotState::Occupied;
        }
        slots_ = std::move(new_slots);
        tombstones_ = 0;
    }
    capacity_ = new_capacity;
    ++rehashes_;
}

// Retorna ponteiro para o valor associado à chave, ou nullptr.
template <typename K, typename V, typename H, typename E>
V* HashTable<K, V, H, E>::lookup(const K& key, std::uint64_t hash_value) const {
    if (capacity_ == 0) {
        return nullptr;
    }

    if (strategy_ == CollisionStrategy::SeparateChaining) {
        for (Node* node = buckets_[index_for(hash_value, capacity_)]; node != nullptr;
             node = node->next) {
            if (node->hash == hash_value && key_equal_(node->entry.key, key)) {
                return &node->entry.value;
            }
        }
        return nullptr;
    }

    const size_type position = find_slot(key, hash_value);
    return position == kNpos ? nullptr : &slots_[position].entry->value;
}

// Endereçamento aberto: índice do slot que contém a chave, ou kNpos.
template <typename K, typename V, typename H, typename E>
auto HashTable<K, V, H, E>::find_slot(const K& key, std::uint64_t hash_value) const -> size_type {
    const size_type mask = capacity_ - 1;
    size_type position = index_for(hash_value, capacity_);

    // O limite de 'capacity_' passos é só uma salvaguarda: sempre existe
    // ao menos um slot Empty (ver validate_load_factor), que encerra a busca.
    for (size_type step = 0; step < capacity_; ++step) {
        const Slot& slot = slots_[position];
        if (slot.state == SlotState::Empty) {
            return kNpos;
        }
        if (slot.state == SlotState::Occupied && slot.hash == hash_value &&
            key_equal_(slot.entry->key, key)) {
            return position;
        }
        position = (position + 1) & mask;
    }
    return kNpos;
}

// Insere uma chave sabidamente ausente. Pode reconstruir a tabela.
template <typename K, typename V, typename H, typename E>
V* HashTable<K, V, H, E>::emplace_new(K&& key, V&& value, std::uint64_t hash_value) {
    grow_if_needed();
    const size_type index = index_for(hash_value, capacity_);

    if (strategy_ == CollisionStrategy::SeparateChaining) {
        Node*& head = buckets_[index];
        if (head != nullptr) {
            ++collisions_;
        }
        Node* node = new Node(std::move(key), std::move(value), hash_value);
        node->next = head;  // inserção no início da cadeia: O(1)
        head = node;
        ++size_;
        return &node->entry.value;
    }

    const size_type mask = capacity_ - 1;
    size_type position = index;
    if (slots_[position].state == SlotState::Occupied) {
        ++collisions_;
    }
    // Como a chave é nova, o primeiro slot não ocupado (Empty ou Deleted) serve.
    while (slots_[position].state == SlotState::Occupied) {
        position = (position + 1) & mask;
    }

    Slot& slot = slots_[position];
    slot.entry.emplace(Entry{std::move(key), std::move(value)});
    if (slot.state == SlotState::Deleted) {
        --tombstones_;
    }
    slot.state = SlotState::Occupied;
    slot.hash = hash_value;
    ++size_;
    return &slot.entry->value;
}

template <typename K, typename V, typename H, typename E>
auto HashTable<K, V, H, E>::longest_probe() const -> size_type {
    size_type longest = 0;

    if (strategy_ == CollisionStrategy::SeparateChaining) {
        for (size_type i = 0; i < capacity_; ++i) {
            size_type length = 0;
            for (const Node* node = buckets_[i]; node != nullptr; node = node->next) {
                ++length;
            }
            longest = std::max(longest, length);
        }
    } else {
        const size_type mask = capacity_ - 1;
        for (size_type i = 0; i < capacity_; ++i) {
            if (slots_[i].state != SlotState::Occupied) {
                continue;
            }
            const size_type home = index_for(slots_[i].hash, capacity_);
            const size_type distance = ((i + capacity_ - home) & mask) + 1;
            longest = std::max(longest, distance);
        }
    }
    return longest;
}

}  // namespace Structures::HashTable

#endif  // STRUCTURES_HASH_HASH_TABLE_CPP
