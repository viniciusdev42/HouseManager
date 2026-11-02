#ifndef  CORE_DYNAMIC_ARRAY
#define  CORE_DYNAMIC_ARRAY

// Dynamic Array (C++17, header-only) para armazenar caracteres, resultados de
// operações e tabelas auxiliares de KMP, Boyer-Moore, Rabin-Karp e Levenshtein.
//
// Organização:
//   1. DynamicArray<T>        -> contêiner genérico (núcleo)
//   2. CharArray + text::     -> sequências de caracteres
//   3. Matrix<T>              -> tabela 2D (Levenshtein) sobre DynamicArray
//   4. Resultados             -> Match, MatchArray, IndexArray, DistanceArray
//   5. Tabelas auxiliares     -> KmpTable, BoyerMooreTables, RabinKarpTable,
//                                LevenshteinMatrix e suas fábricas em tables::
//
// Os algoritmos em si NÃO são implementados aqui: este arquivo fornece apenas
// o armazenamento que eles utilizam.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace Core::DynamicArray {

// ============================================================================
// 1. DynamicArray<T>
// ============================================================================

template <typename T>
class DynamicArray {
public:
    using value_type      = T;
    using size_type       = std::size_t;
    using reference       = T&;
    using const_reference = const T&;
    using pointer         = T*;
    using const_pointer   = const T*;
    using iterator        = T*;
    using const_iterator  = const T*;

    static constexpr size_type kDefaultCapacity = 8;
    static constexpr size_type kGrowthFactor    = 2;

    // --- Construção, cópia e movimentação ----------------------------------

    DynamicArray() noexcept = default;

    // Os construtores abaixo delegam ao padrão: assim, se uma exceção ocorrer
    // no corpo, o objeto já está "construído" e o destrutor libera a memória.
    explicit DynamicArray(size_type count) : DynamicArray() { resize(count); }

    DynamicArray(size_type count, const T& value) : DynamicArray() {
        resize(count, value);
    }

    DynamicArray(std::initializer_list<T> values) : DynamicArray() {
        reserve(values.size());
        for (const T& element : values) emplace_back(element);
    }

    DynamicArray(const DynamicArray& other) : DynamicArray() {
        reserve(other.size_);
        for (const T& element : other) emplace_back(element);
    }

    DynamicArray(DynamicArray&& other) noexcept
        : data_(std::exchange(other.data_, nullptr)),
          size_(std::exchange(other.size_, 0)),
          capacity_(std::exchange(other.capacity_, 0)) {}

    // Copy-and-swap: garante garantia forte de exceção e trata auto-atribuição.
    DynamicArray& operator=(const DynamicArray& other) {
        DynamicArray copy(other);
        swap(copy);
        return *this;
    }

    DynamicArray& operator=(DynamicArray&& other) noexcept {
        DynamicArray moved(std::move(other));
        swap(moved);
        return *this;
    }

    ~DynamicArray() {
        destroy_range(data_, size_);
        deallocate(data_, capacity_);
    }

    // --- Capacidade ---------------------------------------------------------

    size_type size() const noexcept { return size_; }
    size_type capacity() const noexcept { return capacity_; }
    bool empty() const noexcept { return size_ == 0; }

    static constexpr size_type max_size() noexcept {
        return std::numeric_limits<size_type>::max() / sizeof(T);
    }

    // Capacidade exata solicitada; nunca diminui.
    void reserve(size_type new_capacity) {
        if (new_capacity <= capacity_) return;
        if (new_capacity > max_size()) {
            throw std::length_error("DynamicArray::reserve: capacidade acima do maximo");
        }
        reallocate(new_capacity);
    }

    void shrink_to_fit() {
        if (size_ == capacity_) return;
        if (size_ == 0) {
            deallocate(data_, capacity_);
            data_ = nullptr;
            capacity_ = 0;
            return;
        }
        reallocate(size_);
    }

    // Redimensiona o tamanho lógico; novos elementos são value-initialized.
    void resize(size_type count) {
        resize_with(count, [](pointer slot) { ::new (static_cast<void*>(slot)) T(); });
    }

    void resize(size_type count, const T& value) {
        // Cópia local: 'value' pode referenciar um elemento deste próprio array,
        // que ficaria inválido caso ocorra realocação.
        const T copy(value);
        resize_with(count, [&copy](pointer slot) {
            ::new (static_cast<void*>(slot)) T(copy);
        });
    }

    // --- Acesso -------------------------------------------------------------

    // Sem verificação de limites (como std::vector); use at() quando preciso.
    reference operator[](size_type index) noexcept { return data_[index]; }
    const_reference operator[](size_type index) const noexcept { return data_[index]; }

    reference at(size_type index) {
        check_index(index);
        return data_[index];
    }
    const_reference at(size_type index) const {
        check_index(index);
        return data_[index];
    }

    reference front() { return at(0); }
    const_reference front() const { return at(0); }
    reference back() { return at(size_ == 0 ? 0 : size_ - 1); }
    const_reference back() const { return at(size_ == 0 ? 0 : size_ - 1); }

    pointer data() noexcept { return data_; }
    const_pointer data() const noexcept { return data_; }

    iterator begin() noexcept { return data_; }
    iterator end() noexcept { return data_ + size_; }
    const_iterator begin() const noexcept { return data_; }
    const_iterator end() const noexcept { return data_ + size_; }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    // --- Modificadores ------------------------------------------------------

    template <typename... Args>
    reference emplace_back(Args&&... args) {
        if (size_ == capacity_) {
            // Constrói antes de realocar: os argumentos podem referenciar
            // elementos do próprio array (ex.: push_back(arr[0])).
            T temporary(std::forward<Args>(args)...);
            reallocate(grown_capacity(size_ + 1));
            ::new (static_cast<void*>(data_ + size_)) T(std::move(temporary));
        } else {
            ::new (static_cast<void*>(data_ + size_)) T(std::forward<Args>(args)...);
        }
        ++size_;
        return data_[size_ - 1];
    }

    void push_back(const T& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(std::move(value)); }

    void pop_back() {
        if (size_ == 0) throw std::out_of_range("DynamicArray::pop_back: array vazio");
        --size_;
        destroy_range(data_ + size_, 1);
    }

    // Insere 'value' na posição 'index' (0 <= index <= size()).
    void insert(size_type index, const T& value) {
        if (index > size_) {
            throw std::out_of_range("DynamicArray::insert: indice " + std::to_string(index) +
                                    " fora do intervalo [0, " + std::to_string(size_) + "]");
        }
        emplace_back(value);
        std::rotate(data_ + index, data_ + size_ - 1, data_ + size_);
    }

    void erase(size_type index) {
        check_index(index);
        std::move(data_ + index + 1, data_ + size_, data_ + index);
        pop_back();
    }

    // Anexa 'count' elementos a partir de 'source'. Aceita faixas do próprio array.
    void append(const T* source, size_type count) {
        if (count == 0) return;
        if (source == nullptr) {
            throw std::invalid_argument("DynamicArray::append: origem nula com count > 0");
        }
        if (count > max_size() - size_) {
            throw std::length_error("DynamicArray::append: tamanho acima do maximo");
        }

        const std::less<const T*> before;
        const bool inside_self = !before(source, data_) && before(source, data_ + size_);
        const size_type offset = inside_self ? static_cast<size_type>(source - data_) : 0;

        if (size_ + count > capacity_) reallocate(grown_capacity(size_ + count));
        if (inside_self) source = data_ + offset;  // buffer pode ter mudado

        for (size_type i = 0; i < count; ++i) {
            ::new (static_cast<void*>(data_ + size_)) T(source[i]);
            ++size_;
        }
    }

    void append(const DynamicArray& other) { append(other.data_, other.size_); }

    // Destrói os elementos mas mantém a capacidade para reaproveitamento.
    void clear() noexcept {
        destroy_range(data_, size_);
        size_ = 0;
    }

    void swap(DynamicArray& other) noexcept {
        std::swap(data_, other.data_);
        std::swap(size_, other.size_);
        std::swap(capacity_, other.capacity_);
    }

    friend void swap(DynamicArray& a, DynamicArray& b) noexcept { a.swap(b); }

    friend bool operator==(const DynamicArray& a, const DynamicArray& b) {
        return a.size_ == b.size_ && std::equal(a.begin(), a.end(), b.begin());
    }
    friend bool operator!=(const DynamicArray& a, const DynamicArray& b) { return !(a == b); }

private:
    pointer data_ = nullptr;
    size_type size_ = 0;
    size_type capacity_ = 0;

    static pointer allocate(size_type count) {
        return count == 0 ? nullptr : std::allocator<T>().allocate(count);
    }

    static void deallocate(pointer block, size_type count) noexcept {
        if (block != nullptr) std::allocator<T>().deallocate(block, count);
    }

    static void destroy_range(pointer first, size_type count) noexcept {
        if constexpr (!std::is_trivially_destructible_v<T>) {
            for (size_type i = 0; i < count; ++i) first[i].~T();
        }
    }

    void check_index(size_type index) const {
        if (index >= size_) {
            throw std::out_of_range("DynamicArray: indice " + std::to_string(index) +
                                    " fora do intervalo (size = " + std::to_string(size_) + ")");
        }
    }

    // Crescimento geométrico (x2) => push_back com custo amortizado O(1).
    size_type grown_capacity(size_type min_required) const {
        if (min_required > max_size()) {
            throw std::length_error("DynamicArray: capacidade acima do maximo");
        }
        size_type candidate;
        if (capacity_ == 0) {
            candidate = kDefaultCapacity;
        } else if (capacity_ > max_size() / kGrowthFactor) {
            candidate = max_size();
        } else {
            candidate = capacity_ * kGrowthFactor;
        }
        return std::max(candidate, min_required);
    }

    // Move os elementos para um novo bloco; mantém o array intacto se algo lançar.
    void reallocate(size_type new_capacity) {
        pointer new_block = allocate(new_capacity);
        size_type constructed = 0;
        try {
            for (; constructed < size_; ++constructed) {
                ::new (static_cast<void*>(new_block + constructed))
                    T(std::move_if_noexcept(data_[constructed]));
            }
        } catch (...) {
            destroy_range(new_block, constructed);
            deallocate(new_block, new_capacity);
            throw;
        }
        destroy_range(data_, size_);
        deallocate(data_, capacity_);
        data_ = new_block;
        capacity_ = new_capacity;
    }

    template <typename Construct>
    void resize_with(size_type count, Construct construct) {
        if (count <= size_) {
            destroy_range(data_ + count, size_ - count);
            size_ = count;
            return;
        }
        if (count > capacity_) reallocate(grown_capacity(count));
        while (size_ < count) {
            construct(data_ + size_);
            ++size_;  // incrementa a cada elemento: estado consistente se lançar
        }
    }
};

// ============================================================================
// 2. Sequências de caracteres
// ============================================================================

using CharArray = DynamicArray<char>;

// Funções livres em vez de herança: DynamicArray não tem destrutor virtual e
// herdar dele convidaria a erros de uso polimórfico.
namespace DynamicArray::Text {

inline CharArray make_char_array(std::string_view content) {
    CharArray result;
    result.append(content.data(), content.size());
    return result;
}

inline void append_text(CharArray& target, std::string_view content) {
    target.append(content.data(), content.size());
}

inline std::string_view as_view(const CharArray& array) noexcept {
    return std::string_view(array.data(), array.size());
}

inline std::string to_string(const CharArray& array) {
    return std::string(array.data(), array.size());
}

inline CharArray substring(const CharArray& array, std::size_t position,
                           std::size_t length = std::string_view::npos) {
    if (position > array.size()) {
        throw std::out_of_range("text::substring: posicao " + std::to_string(position) +
                                " maior que o tamanho " + std::to_string(array.size()));
    }
    const std::size_t effective = std::min(length, array.size() - position);
    CharArray result;
    result.append(array.data() + position, effective);
    return result;
}

}  //  namespace DynamicArray::Text

// ============================================================================
// 3. Matrix<T> — tabela 2D em ordem de linha (row-major)
// ============================================================================

template <typename T>
class Matrix {
public:
    Matrix() = default;

    Matrix(std::size_t rows, std::size_t cols, const T& value = T()) {
        assign(rows, cols, value);
    }

    // Redefine dimensões e preenche todas as células com 'value'.
    void assign(std::size_t rows, std::size_t cols, const T& value = T()) {
        if (cols != 0 && rows > std::numeric_limits<std::size_t>::max() / cols) {
            throw std::length_error("Matrix::assign: dimensoes excedem o limite");
        }
        rows_ = 0;
        cols_ = 0;
        cells_.clear();
        cells_.resize(rows * cols, value);
        rows_ = rows;
        cols_ = cols;
    }

    void fill(const T& value) { std::fill(cells_.begin(), cells_.end(), value); }

    std::size_t rows() const noexcept { return rows_; }
    std::size_t cols() const noexcept { return cols_; }
    std::size_t size() const noexcept { return cells_.size(); }
    bool empty() const noexcept { return cells_.empty(); }

    // Sem verificação de limites.
    T& operator()(std::size_t row, std::size_t col) noexcept { return cells_[row * cols_ + col]; }
    const T& operator()(std::size_t row, std::size_t col) const noexcept {
        return cells_[row * cols_ + col];
    }

    T& at(std::size_t row, std::size_t col) {
        check_bounds(row, col);
        return cells_[row * cols_ + col];
    }
    const T& at(std::size_t row, std::size_t col) const {
        check_bounds(row, col);
        return cells_[row * cols_ + col];
    }

private:
    DynamicArray<T> cells_;
    std::size_t rows_ = 0;
    std::size_t cols_ = 0;

    void check_bounds(std::size_t row, std::size_t col) const {
        if (row >= rows_ || col >= cols_) {
            throw std::out_of_range("Matrix: posicao (" + std::to_string(row) + ", " +
                                    std::to_string(col) + ") fora de " + std::to_string(rows_) +
                                    "x" + std::to_string(cols_));
        }
    }
};

// ============================================================================
// 4. Armazenamento de resultados
// ============================================================================

using IndexArray    = DynamicArray<std::size_t>;  // posições de ocorrências
using DistanceArray = DynamicArray<std::size_t>;  // distâncias de edição

struct Match {
    std::size_t position;  // índice inicial da ocorrência no texto
    std::size_t length;    // tamanho do trecho encontrado
};
using MatchArray = DynamicArray<Match>;

// ============================================================================
// 5. Tabelas auxiliares dos algoritmos de busca e distância
// ============================================================================

// KMP: failure[i] = tamanho do maior prefixo próprio de pattern[0..i] que
// também é sufixo (função de prefixo / LPS).
struct KmpTable {
    IndexArray failure;
};

// Boyer-Moore:
//   bad_character     -> última posição de cada byte no padrão (-1 se ausente)
//   good_suffix_shift -> deslocamento da regra do bom sufixo, tamanho m + 1
//   suffix_border     -> bordas usadas no pré-processamento do bom sufixo
struct BoyerMooreTables {
    static constexpr std::size_t kAlphabetSize = 256;

    DynamicArray<std::ptrdiff_t> bad_character;
    IndexArray good_suffix_shift;
    IndexArray suffix_border;

    // Indexa pelo valor sem sinal: char pode ser negativo em algumas plataformas.
    static std::size_t index_of(char symbol) noexcept {
        return static_cast<unsigned char>(symbol);
    }
};

// Rabin-Karp: parâmetros do hash rolante e o hash de cada janela do texto.
struct RabinKarpTable {
    std::uint64_t base = 256;
    std::uint64_t modulus = 1'000'000'007ULL;
    std::uint64_t pattern_hash = 0;
    std::uint64_t high_order_factor = 1;  // base^(m-1) mod modulus
    DynamicArray<std::uint64_t> window_hashes;
};

// Levenshtein: matriz (|a|+1) x (|b|+1) de custos.
using LevenshteinMatrix = Matrix<std::size_t>;

// Fábricas: apenas alocam e inicializam o armazenamento; o preenchimento
// com os valores do algoritmo fica a cargo de quem o implementa.
 namespace DynamicArray::Tables {

inline KmpTable make_kmp_table(std::size_t pattern_length) {
    KmpTable table;
    table.failure.resize(pattern_length, 0);
    return table;
}

inline BoyerMooreTables make_boyer_moore_tables(std::size_t pattern_length) {
    BoyerMooreTables tables;
    tables.bad_character.resize(BoyerMooreTables::kAlphabetSize, -1);
    tables.good_suffix_shift.resize(pattern_length + 1, 0);
    tables.suffix_border.resize(pattern_length + 1, 0);
    return tables;
}

inline RabinKarpTable make_rabin_karp_table(std::size_t text_length, std::size_t pattern_length,
                                            std::uint64_t base = 256,
                                            std::uint64_t modulus = 1'000'000'007ULL) {
    if (modulus == 0) {
        throw std::invalid_argument("make_rabin_karp_table: modulo nao pode ser zero");
    }
    RabinKarpTable table;
    table.base = base;
    table.modulus = modulus;
    const std::size_t windows = text_length >= pattern_length ? text_length - pattern_length + 1 : 0;
    table.window_hashes.resize(windows, 0);
    return table;
}

inline LevenshteinMatrix make_levenshtein_matrix(std::size_t length_a, std::size_t length_b) {
    constexpr std::size_t kMax = std::numeric_limits<std::size_t>::max();
    if (length_a == kMax || length_b == kMax) {
        throw std::length_error("make_levenshtein_matrix: tamanho excede o limite");
    }
    return LevenshteinMatrix(length_a + 1, length_b + 1, 0);
}

}  // namespace DynamicArray::Tables

}  // namespace Core::DynamicArray

#endif  // CORE_DYNAMIC_ARRAY
