#ifndef STRUCTURES_BTREE_NODE
#define STRUCTURES_BTREE_NODE

/**
 * btree_node.hpp
 *
 * Nó de Árvore B orientado a disco, com serialização binária, desserialização,
 * validação estrutural e cálculo de tamanho. Header-only, C++17.
 *
 * Convenções:
 *  - Grau mínimo t (minDegree) >= 2.
 *  - Todo nó possui no máximo 2t - 1 chaves e (exceto a raiz) no mínimo t - 1.
 *  - Nó interno com n chaves possui exatamente n + 1 filhos.
 *  - Filhos são referenciados por PageId (identificador de página em disco),
 *    não por ponteiros, para que o nó possa ser persistido.
 *
 * Layout binário (todos os inteiros em little-endian, independente da máquina):
 *
 *   Offset  Tamanho  Campo
 *   ------  -------  ------------------------------------------------
 *   0       4        magic ("BTND")
 *   4       2        versão do formato
 *   6       1        flags (bit0 = folha, bit1 = raiz)
 *   7       1        reservado (sempre 0)
 *   8       4        minDegree
 *   12      4        keyCount
 *   16      8        nodeId
 *   24      8*n      chaves
 *   ..      8*n      valores
 *   ..      8*(n+1)  filhos (somente em nós internos)
 *   fim-4   4        CRC-32 de todos os bytes anteriores
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace BTree {

using KeyType = std::int64_t;
using ValueType = std::int64_t;
using PageId = std::uint64_t;

// A página 0 é reservada como "sem página", o que evita confundir um
// identificador não inicializado com um filho válido.
constexpr PageId kInvalidPageId = 0;

constexpr std::uint32_t kMinAllowedDegree = 2;

// Limite defensivo: impede que um cabeçalho corrompido peça alocações enormes.
constexpr std::uint32_t kMaxAllowedDegree = 65536;

// ---------------------------------------------------------------------------
// Estruturas públicas
// ---------------------------------------------------------------------------

struct BTreeNode {
    PageId nodeId = kInvalidPageId;
    std::uint32_t minDegree = kMinAllowedDegree;
    bool isLeaf = true;
    bool isRoot = false;
    std::vector<KeyType> keys;
    std::vector<ValueType> values;   // values[i] está associado a keys[i]
    std::vector<PageId> children;    // vazio em folhas; keys.size() + 1 em internos

    // Só devem ser chamadas depois de minDegree ter sido validado (>= 2).
    std::size_t maxKeys() const { return static_cast<std::size_t>(minDegree) * 2 - 1; }
    std::size_t minKeys() const { return static_cast<std::size_t>(minDegree) - 1; }
};

struct ValidationResult {
    bool valid = true;
    std::string message;

    explicit operator bool() const { return valid; }
};

// Lançada quando os bytes recebidos não representam um nó íntegro.
class NodeFormatError : public std::runtime_error {
public:
    explicit NodeFormatError(const std::string& message) : std::runtime_error(message) {}
};

// ---------------------------------------------------------------------------
// Detalhes internos
// ---------------------------------------------------------------------------

namespace detail {

constexpr std::uint32_t kMagic = 0x444E5442u;  // "BTND" em little-endian
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::size_t kHeaderSize = 24;
constexpr std::size_t kChecksumSize = 4;
constexpr std::uint8_t kFlagLeaf = 0x01;
constexpr std::uint8_t kFlagRoot = 0x02;
constexpr std::uint8_t kKnownFlags = kFlagLeaf | kFlagRoot;

inline const std::array<std::uint32_t, 256>& crcTable() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    const auto& table = crcTable();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

// Escrita byte a byte: o formato em disco não depende da endianness da CPU.
class ByteWriter {
public:
    explicit ByteWriter(std::vector<std::uint8_t>& buffer) : buffer_(buffer) {}

    void writeU8(std::uint8_t v) { buffer_.push_back(v); }
    void writeU16(std::uint16_t v) { writeUnsigned(v, 2); }
    void writeU32(std::uint32_t v) { writeUnsigned(v, 4); }
    void writeU64(std::uint64_t v) { writeUnsigned(v, 8); }
    void writeI64(std::int64_t v) { writeUnsigned(static_cast<std::uint64_t>(v), 8); }

private:
    void writeUnsigned(std::uint64_t v, std::size_t bytes) {
        for (std::size_t i = 0; i < bytes; ++i) {
            buffer_.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
        }
    }

    std::vector<std::uint8_t>& buffer_;
};

// Leitura com verificação de limites: nunca lê além do buffer informado.
class ByteReader {
public:
    ByteReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::uint8_t readU8() { return static_cast<std::uint8_t>(readUnsigned(1)); }
    std::uint16_t readU16() { return static_cast<std::uint16_t>(readUnsigned(2)); }
    std::uint32_t readU32() { return static_cast<std::uint32_t>(readUnsigned(4)); }
    std::uint64_t readU64() { return readUnsigned(8); }
    std::int64_t readI64() { return static_cast<std::int64_t>(readUnsigned(8)); }

    std::size_t remaining() const { return size_ - position_; }

private:
    std::uint64_t readUnsigned(std::size_t bytes) {
        if (bytes > remaining()) {
            throw NodeFormatError("Buffer truncado: dados insuficientes para ler o nó.");
        }
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < bytes; ++i) {
            v |= static_cast<std::uint64_t>(data_[position_ + i]) << (8 * i);
        }
        position_ += bytes;
        return v;
    }

    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
};

inline ValidationResult invalid(const std::string& message) {
    return ValidationResult{false, message};
}

}  // namespace detail

// ---------------------------------------------------------------------------
// calculateNodeSize
// ---------------------------------------------------------------------------

/**
 * Retorna quantos bytes o nó ocupa quando serializado (cabeçalho + chaves +
 * valores + filhos + checksum). Baseia-se no conteúdo real dos vetores, de modo
 * que reflete a ocupação atual do nó.
 */
inline std::size_t calculateNodeSize(const BTreeNode& node) {
    return detail::kHeaderSize
         + node.keys.size() * sizeof(KeyType)
         + node.values.size() * sizeof(ValueType)
         + node.children.size() * sizeof(PageId)
         + detail::kChecksumSize;
}

/**
 * Tamanho máximo que um nó de grau mínimo `minDegree` pode atingir. Útil para
 * dimensionar páginas de disco. Nós internos são sempre os maiores.
 */
inline std::size_t calculateMaxNodeSize(std::uint32_t minDegree) {
    if (minDegree < kMinAllowedDegree || minDegree > kMaxAllowedDegree) {
        throw std::invalid_argument("minDegree fora do intervalo permitido.");
    }
    const std::size_t maxKeys = static_cast<std::size_t>(minDegree) * 2 - 1;
    return detail::kHeaderSize
         + maxKeys * (sizeof(KeyType) + sizeof(ValueType))
         + (maxKeys + 1) * sizeof(PageId)
         + detail::kChecksumSize;
}

// ---------------------------------------------------------------------------
// validateNode
// ---------------------------------------------------------------------------

/**
 * Verifica a integridade estrutural do nó. Não consulta outros nós; checa
 * apenas as invariantes que podem ser garantidas localmente.
 */
inline ValidationResult validateNode(const BTreeNode& node) {
    using detail::invalid;

    if (node.minDegree < kMinAllowedDegree || node.minDegree > kMaxAllowedDegree) {
        return invalid("minDegree fora do intervalo permitido.");
    }
    if (node.nodeId == kInvalidPageId) {
        return invalid("nodeId inválido (0 é reservado).");
    }

    const std::size_t keyCount = node.keys.size();

    if (keyCount > node.maxKeys()) {
        return invalid("Número de chaves excede o máximo (2t - 1).");
    }
    if (node.isRoot) {
        if (!node.isLeaf && keyCount < 1) {
            return invalid("Raiz interna deve ter ao menos 1 chave.");
        }
    } else if (keyCount < node.minKeys()) {
        return invalid("Nó não-raiz com menos chaves que o mínimo (t - 1).");
    }

    if (node.values.size() != keyCount) {
        return invalid("Quantidade de valores difere da quantidade de chaves.");
    }

    // Ordem estritamente crescente também garante a ausência de duplicatas.
    for (std::size_t i = 1; i < keyCount; ++i) {
        if (node.keys[i - 1] >= node.keys[i]) {
            return invalid("Chaves não estão em ordem estritamente crescente.");
        }
    }

    if (node.isLeaf) {
        if (!node.children.empty()) {
            return invalid("Nó folha não pode ter filhos.");
        }
        return {};
    }

    if (node.children.size() != keyCount + 1) {
        return invalid("Nó interno deve ter exatamente (chaves + 1) filhos.");
    }
    for (PageId child : node.children) {
        if (child == kInvalidPageId) {
            return invalid("Nó interno contém referência de filho inválida.");
        }
        if (child == node.nodeId) {
            return invalid("Nó referencia a si mesmo como filho.");
        }
    }
    std::vector<PageId> sortedChildren(node.children);
    std::sort(sortedChildren.begin(), sortedChildren.end());
    if (std::adjacent_find(sortedChildren.begin(), sortedChildren.end()) != sortedChildren.end()) {
        return invalid("Nó interno contém filhos duplicados.");
    }

    return {};
}

// ---------------------------------------------------------------------------
// serializeNode
// ---------------------------------------------------------------------------

/**
 * Converte o nó em uma sequência de bytes.
 * @throws std::invalid_argument se o nó não passar em validateNode.
 */
inline std::vector<std::uint8_t> serializeNode(const BTreeNode& node) {
    const ValidationResult validation = validateNode(node);
    if (!validation.valid) {
        throw std::invalid_argument("Não é possível serializar nó inválido: " + validation.message);
    }

    std::vector<std::uint8_t> buffer;
    buffer.reserve(calculateNodeSize(node));
    detail::ByteWriter writer(buffer);

    std::uint8_t flags = 0;
    if (node.isLeaf) flags |= detail::kFlagLeaf;
    if (node.isRoot) flags |= detail::kFlagRoot;

    writer.writeU32(detail::kMagic);
    writer.writeU16(detail::kFormatVersion);
    writer.writeU8(flags);
    writer.writeU8(0);  // reservado
    writer.writeU32(node.minDegree);
    writer.writeU32(static_cast<std::uint32_t>(node.keys.size()));
    writer.writeU64(node.nodeId);

    for (KeyType key : node.keys) writer.writeI64(key);
    for (ValueType value : node.values) writer.writeI64(value);
    for (PageId child : node.children) writer.writeU64(child);

    writer.writeU32(detail::crc32(buffer.data(), buffer.size()));
    return buffer;
}

// ---------------------------------------------------------------------------
// deserializeNode
// ---------------------------------------------------------------------------

/**
 * Reconstrói um nó a partir de bytes gerados por serializeNode.
 * @throws NodeFormatError se os bytes estiverem corrompidos, truncados,
 *         em versão desconhecida ou se o nó resultante for estruturalmente inválido.
 */
inline BTreeNode deserializeNode(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < detail::kHeaderSize + detail::kChecksumSize) {
        throw NodeFormatError("Buffer pequeno demais para conter um nó.");
    }

    // O checksum é verificado antes de interpretar qualquer campo: assim,
    // corrupção de disco é detectada sem confiar em valores possivelmente lixo.
    const std::size_t payloadSize = size - detail::kChecksumSize;
    detail::ByteReader checksumReader(data + payloadSize, detail::kChecksumSize);
    if (detail::crc32(data, payloadSize) != checksumReader.readU32()) {
        throw NodeFormatError("Checksum inválido: dados do nó corrompidos.");
    }

    detail::ByteReader reader(data, payloadSize);

    if (reader.readU32() != detail::kMagic) {
        throw NodeFormatError("Magic number inválido: não é um nó de Árvore B.");
    }
    if (reader.readU16() != detail::kFormatVersion) {
        throw NodeFormatError("Versão de formato não suportada.");
    }

    const std::uint8_t flags = reader.readU8();
    if ((flags & ~detail::kKnownFlags) != 0) {
        throw NodeFormatError("Flags desconhecidas no cabeçalho do nó.");
    }
    if (reader.readU8() != 0) {
        throw NodeFormatError("Campo reservado deve ser zero.");
    }

    BTreeNode node;
    node.isLeaf = (flags & detail::kFlagLeaf) != 0;
    node.isRoot = (flags & detail::kFlagRoot) != 0;
    node.minDegree = reader.readU32();

    const std::uint32_t keyCount = reader.readU32();
    node.nodeId = reader.readU64();

    // Valida os limites antes de alocar, para não confiar em keyCount corrompido.
    if (node.minDegree < kMinAllowedDegree || node.minDegree > kMaxAllowedDegree) {
        throw NodeFormatError("minDegree fora do intervalo permitido.");
    }
    if (keyCount > node.maxKeys()) {
        throw NodeFormatError("keyCount excede o máximo permitido para o grau do nó.");
    }

    node.keys.reserve(keyCount);
    node.values.reserve(keyCount);
    for (std::uint32_t i = 0; i < keyCount; ++i) node.keys.push_back(reader.readI64());
    for (std::uint32_t i = 0; i < keyCount; ++i) node.values.push_back(reader.readI64());

    if (!node.isLeaf) {
        node.children.reserve(static_cast<std::size_t>(keyCount) + 1);
        for (std::uint32_t i = 0; i <= keyCount; ++i) node.children.push_back(reader.readU64());
    }

    if (reader.remaining() != 0) {
        throw NodeFormatError("Bytes excedentes após o fim do nó.");
    }

    const ValidationResult validation = validateNode(node);
    if (!validation.valid) {
        throw NodeFormatError("Nó desserializado é inválido: " + validation.message);
    }
    return node;
}

inline BTreeNode deserializeNode(const std::vector<std::uint8_t>& bytes) {
    return deserializeNode(bytes.data(), bytes.size());
}

// ---------------------------------------------------------------------------
// Leitura a partir de páginas de tamanho fixo
// ---------------------------------------------------------------------------

/**
 * Descobre, apenas pelo cabeçalho, quantos bytes o nó serializado no início de
 * `data` ocupa. Serve para páginas de tamanho fixo, em que o nó é seguido por
 * bytes de preenchimento e, portanto, o checksum não está no fim do buffer.
 * @throws NodeFormatError se o cabeçalho for inválido ou o buffer for menor
 *         que o nó declarado.
 */
inline std::size_t serializedNodeSize(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < detail::kHeaderSize + detail::kChecksumSize) {
        throw NodeFormatError("Buffer pequeno demais para conter um nó.");
    }

    detail::ByteReader reader(data, size);
    if (reader.readU32() != detail::kMagic) {
        throw NodeFormatError("Magic number inválido: não é um nó de Árvore B.");
    }
    reader.readU16();                          // versão (validada em deserializeNode)
    const std::uint8_t flags = reader.readU8();
    reader.readU8();                           // reservado
    reader.readU32();                          // minDegree
    const std::uint64_t keyCount = reader.readU32();

    // Em 64 bits não há overflow: keyCount < 2^32 e cada item tem 8 bytes.
    const bool isLeaf = (flags & detail::kFlagLeaf) != 0;
    std::uint64_t total = detail::kHeaderSize
                        + keyCount * (sizeof(KeyType) + sizeof(ValueType))
                        + detail::kChecksumSize;
    if (!isLeaf) {
        total += (keyCount + 1) * sizeof(PageId);
    }

    if (total > size) {
        throw NodeFormatError("Buffer menor que o nó declarado no cabeçalho.");
    }
    return static_cast<std::size_t>(total);
}

/**
 * Como deserializeNode, mas ignora bytes de preenchimento após o nó. O
 * checksum e a validação estrutural continuam sendo aplicados ao nó inteiro.
 */
inline BTreeNode deserializeNodeFromPage(const std::uint8_t* data, std::size_t size) {
    return deserializeNode(data, serializedNodeSize(data, size));
}

inline BTreeNode deserializeNodeFromPage(const std::vector<std::uint8_t>& bytes) {
    return deserializeNodeFromPage(bytes.data(), bytes.size());
}
}  // namespace BTree

#endif  // STRUCTURES_BTREE_NODE
