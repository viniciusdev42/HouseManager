/*
 *  Hash Table Genérica
 *  
 *   Estratégicas Escolhidas:
 *  
 *   - SeparateChaining: encadeamento separado (listas ligadas por bucket).
 *    - OpenAddressing:  endereçamento aberto com sondagem linear e remoção
 *                       por marcadores (tombstones).
 *
 * Características:
 *  - Capacidade sempre potência de dois; o hash do usuário passa por um
 *    misturador de bits (finalizador do SplitMix64), de modo que hashes
 *    ruins, como o std::hash<inteiro> que é a identidade, não degradam
 *    a distribuição.
 *  - Redimensionamento dinâmico controlado pelo fator de carga máximo.
 *  - Estatísticas (colisões, rehashes, maior sondagem) para benchmarks.
 *  - Os mecanismos internos (nós, slots, sondagem, rehash) são
 *    implementados aqui; nenhum contêiner da biblioteca padrão é usado
 *    como armazenamento. Só a função de hash (std::hash por padrão) é reaproveitada.
 *
 * Complexidade (caso médio, hash bem distribuído):
 *  insert / find / erase: O(1) amortizado.  Pior caso: O(n).
 *  rehash / reserve / clear / for_each: O(capacidade + n).
 *
 * Observações:
 *  - Não é thread-safe.
 *  - Ponteiros/referências para valores são invalidados por qualquer
 *    operação que insira ou redimensione a tabela.
 *  - Um objeto "movido de" permanece válido e vazio.
 *  - Exige C++17.
 *
 * Organização dos arquivos: por ser um template, as definições ficam em
 * HashTable.cpp, que é incluído no final deste header. Mantenha os dois
 * arquivos na mesma pasta; não é preciso listar o .cpp para compilação.
 */
#ifndef STRUCTURES_HASH_HASH_TABLE_HPP
#define STRUCTURES_HASH_HASH_TABLE_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace Structures::HashTable {

/// Estratégia de tratamento de colisões.
enum class CollisionStrategy {
    SeparateChaining,  ///< Encadeamento separado.
    OpenAddressing     ///< Endereçamento aberto (sondagem linear).
};

/// Instantâneo de métricas da tabela, útil para benchmarks e depuração.
struct HashTableStats {
    std::size_t size = 0;           ///< Número de elementos.
    std::size_t bucket_count = 0;   ///< Buckets (chaining) ou slots (open addressing).
    double load_factor = 0.0;       ///< size / bucket_count.
    std::size_t tombstones = 0;     ///< Slots removidos ainda marcados (só open addressing).
    std::size_t collisions = 0;     ///< Inserções cuja posição inicial já estava ocupada (acumulado).
    std::size_t rehashes = 0;       ///< Reconstruções da tabela (acumulado).
    std::size_t longest_probe = 0;  ///< Maior cadeia (chaining) ou maior deslocamento + 1 (open addressing).
};

template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class HashTable {
public:
    using key_type = Key;
    using mapped_type = Value;
    using size_type = std::size_t;
    using hasher = Hash;
    using key_equal = KeyEqual;

    static constexpr size_type kMinCapacity = 8;
    static constexpr double kDefaultChainingLoadFactor = 1.0;
    static constexpr double kDefaultOpenAddressingLoadFactor = 0.7;
    static constexpr double kMaxOpenAddressingLoadFactor = 0.95;

    /**
     * @param collision_strategy Estratégia de colisão.
     * @param initial_capacity   Buckets/slots iniciais (arredondado para potência de dois, mínimo 8).
     * @param max_load          Fator de carga máximo; se ausente, usa 1.0 (chaining) ou 0.7 (open addressing).
     *                          Open addressing exige valor em (0, 0.95].
     * @throws std::invalid_argument fator de carga inválido.
     * @throws std::length_error     capacidade solicitada grande demais.
     */
    explicit HashTable(CollisionStrategy collision_strategy = CollisionStrategy::SeparateChaining,
                       size_type initial_capacity = 16,
                       std::optional<double> max_load = std::nullopt,
                       Hash hash_fn = Hash(),
                       KeyEqual equal_fn = KeyEqual());

    HashTable(const HashTable& other);
    HashTable(HashTable&& other) noexcept;
    HashTable& operator=(const HashTable& other);
    HashTable& operator=(HashTable&& other) noexcept;
    ~HashTable();

    // ----- Modificadores -----------------------------------------------

    /// Insere somente se a chave não existir. Retorna true se inseriu.
    bool insert(Key key, Value value);

    /// Insere ou substitui o valor. Retorna true se inseriu, false se substituiu.
    bool insert_or_assign(Key key, Value value);

    /// Remove a chave. Retorna true se ela existia.
    bool erase(const Key& key);

    /// Remove todos os elementos, mantendo a capacidade atual.
    void clear() noexcept;

    /// Garante espaço para @p element_count elementos sem novo rehash.
    void reserve(size_type element_count);

    /// Reconstrói a tabela com pelo menos @p min_buckets buckets (e o
    /// suficiente para os elementos atuais). Também elimina tombstones.
    void rehash(size_type min_buckets);

    void swap(HashTable& other) noexcept;

    // ----- Acesso -------------------------------------------------------

    /// Ponteiro para o valor ou nullptr se a chave não existir.
    Value* find(const Key& key);
    const Value* find(const Key& key) const;

    bool contains(const Key& key) const;

    /// @throws std::out_of_range se a chave não existir.
    Value& at(const Key& key);
    const Value& at(const Key& key) const;

    /// Acessa o valor, inserindo Value{} se a chave não existir.
    Value& operator[](const Key& key);

    /// Percorre todos os pares em ordem não especificada.
    /// A tabela não deve ser modificada dentro do callback.
    /// Assinatura do callback: void(const Key&, Value&) ou void(const Key&, const Value&).
    template <typename Func>
    void for_each(Func&& func);
    template <typename Func>
    void for_each(Func&& func) const;

    // ----- Consulta de estado ------------------------------------------

    size_type size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    size_type bucket_count() const noexcept { return capacity_; }
    double load_factor() const noexcept {
        return capacity_ == 0 ? 0.0 : static_cast<double>(size_) / static_cast<double>(capacity_);
    }
    double max_load_factor() const noexcept { return max_load_factor_; }
    CollisionStrategy strategy() const noexcept { return strategy_; }

    /// Altera o fator de carga máximo; reconstrói a tabela se necessário.
    /// @throws std::invalid_argument valor inválido para a estratégia.
    void set_max_load_factor(double new_limit);

    /// Coleta as métricas atuais. Custo O(bucket_count).
    HashTableStats stats() const;

    /// Zera os contadores acumulados (colisões e rehashes).
    void reset_stats() noexcept;

    friend void swap(HashTable& a, HashTable& b) noexcept { a.swap(b); }

private:
    struct Entry {
        Key key;
        Value value;
    };

    // Nó da lista ligada (encadeamento separado). Guarda o hash para
    // acelerar comparações e evitar recalculá-lo no rehash.
    struct Node {
        Node(Key k, Value v, std::uint64_t h)
            : entry{std::move(k), std::move(v)}, hash(h), next(nullptr) {}
        Entry entry;
        std::uint64_t hash;
        Node* next;
    };

    enum class SlotState : std::uint8_t { Empty, Occupied, Deleted };

    // Slot (endereçamento aberto). optional permite Key/Value sem construtor padrão.
    struct Slot {
        SlotState state = SlotState::Empty;
        std::uint64_t hash = 0;
        std::optional<Entry> entry;
    };

    static constexpr size_type kNpos = static_cast<size_type>(-1);
    static constexpr size_type kMaxCapacity =
        size_type{1} << (std::numeric_limits<size_type>::digits - 2);

    // Utilitários estáticos
    static double default_load_factor(CollisionStrategy policy) noexcept;
    static void validate_load_factor(double value, CollisionStrategy policy);
    static size_type round_capacity(size_type requested);
    static std::uint64_t mix(std::uint64_t value) noexcept;
    static size_type index_for(std::uint64_t hash_value, size_type capacity) noexcept;

    // Núcleo
    std::uint64_t hash_of(const Key& key) const;
    size_type capacity_for(size_type element_count) const;
    void allocate(size_type capacity);
    void destroy_nodes() noexcept;
    void grow_if_needed();
    void rehash_to(size_type new_capacity);
    Value* lookup(const Key& key, std::uint64_t hash_value) const;
    size_type find_slot(const Key& key, std::uint64_t hash_value) const;
    Value* emplace_new(Key&& key, Value&& value, std::uint64_t hash_value);
    size_type longest_probe() const;

    // A ordem de declaração deve coincidir com as listas de inicialização.
    CollisionStrategy strategy_;
    Hash hasher_;
    KeyEqual key_equal_;
    double max_load_factor_;
    size_type capacity_ = 0;
    size_type size_ = 0;
    size_type tombstones_ = 0;
    size_type collisions_ = 0;
    size_type rehashes_ = 0;
    std::unique_ptr<Node*[]> buckets_;  // usado só em SeparateChaining
    std::unique_ptr<Slot[]> slots_;     // usado só em OpenAddressing
};
}  // namespace Structures::HashTable
#endif  // STRUCTURES_HASH_HASH_TABLE_HPP
