#define STORAGE_PAGE_HPP

// Page.hpp
// Unidade de armazenamento do sistema de páginas, junto com os tipos
// compartilhados (identificador, estados, erros e checksum).

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace PageSystem::Page{

using PageId = std::uint32_t;
using Byte = std::uint8_t;

// O valor máximo de PageId é reservado como "identificador inválido".
inline constexpr PageId kInvalidPageId = std::numeric_limits<PageId>::max();

inline constexpr std::uint32_t kMinPageSize = 64;
inline constexpr std::uint32_t kMaxPageSize = 1u << 20;  // 1 MiB
inline constexpr std::uint32_t kDefaultPageSize = 4096;

// ---------------------------------------------------------------------------
// Erros
// ---------------------------------------------------------------------------

enum class ErrorCode {
    InvalidPageId,
    OutOfBounds,
    InvalidOperation,
    UninitializedPage,
    InvalidPageSize,
    ChecksumMismatch,
    FileNotFound,
    FileAlreadyExists,
    FileCorrupted,
    IoError
};

inline const char* toString(ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::InvalidPageId:     return "InvalidPageId";
        case ErrorCode::OutOfBounds:       return "OutOfBounds";
        case ErrorCode::InvalidOperation:  return "InvalidOperation";
        case ErrorCode::UninitializedPage: return "UninitializedPage";
        case ErrorCode::InvalidPageSize:   return "InvalidPageSize";
        case ErrorCode::ChecksumMismatch:  return "ChecksumMismatch";
        case ErrorCode::FileNotFound:      return "FileNotFound";
        case ErrorCode::FileAlreadyExists: return "FileAlreadyExists";
        case ErrorCode::FileCorrupted:     return "FileCorrupted";
        case ErrorCode::IoError:           return "IoError";
    }
    return "Unknown";
}

class PageError : public std::runtime_error {
public:
    PageError(ErrorCode code, const std::string& message)
        : std::runtime_error(std::string("[") + toString(code) + "] " + message),
          code_(code) {}

    ErrorCode code() const noexcept { return code_; }

private:
    ErrorCode code_;
};

// ---------------------------------------------------------------------------
// Tamanho de página
// ---------------------------------------------------------------------------

// Potências de dois mantêm as páginas alinhadas com blocos do sistema de arquivos.
inline bool isValidPageSize(std::uint32_t size) noexcept {
    return size >= kMinPageSize && size <= kMaxPageSize && (size & (size - 1)) == 0;
}

inline void validatePageSize(std::uint32_t size) {
    if (!isValidPageSize(size)) {
        throw PageError(ErrorCode::InvalidPageSize,
                        "tamanho de página inválido: " + std::to_string(size) +
                            " (deve ser potência de 2 entre " + std::to_string(kMinPageSize) +
                            " e " + std::to_string(kMaxPageSize) + ")");
    }
}

// ---------------------------------------------------------------------------
// Utilitários de serialização (little-endian explícito, independente de plataforma)
// ---------------------------------------------------------------------------

namespace detail {

inline std::uint32_t fnv1a(const Byte* data, std::size_t length,
                           std::uint32_t hash = 2166136261u) noexcept {
    for (std::size_t i = 0; i < length; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

inline void putU32(Byte* dst, std::uint32_t value) noexcept {
    dst[0] = static_cast<Byte>(value & 0xFFu);
    dst[1] = static_cast<Byte>((value >> 8) & 0xFFu);
    dst[2] = static_cast<Byte>((value >> 16) & 0xFFu);
    dst[3] = static_cast<Byte>((value >> 24) & 0xFFu);
}

inline std::uint32_t getU32(const Byte* src) noexcept {
    return static_cast<std::uint32_t>(src[0]) |
           (static_cast<std::uint32_t>(src[1]) << 8) |
           (static_cast<std::uint32_t>(src[2]) << 16) |
           (static_cast<std::uint32_t>(src[3]) << 24);
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Estado e cabeçalho da página
// ---------------------------------------------------------------------------

enum class PageState : std::uint32_t {
    Free = 0,         // disponível para alocação
    Allocated = 1,    // alocada, mas nunca escrita (não inicializada)
    Initialized = 2   // alocada e com conteúdo escrito
};

// Metadados gravados antes do conteúdo de cada página.
struct PageHeader {
    std::uint32_t magic;
    PageId id;
    PageState state;
    std::uint32_t checksum;
};

// ---------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------

class Page {
public:
    static constexpr std::uint32_t kMagic = 0x31454750u;  // "PGE1"
    static constexpr std::size_t kHeaderSize = 16;        // magic + id + estado + checksum

    Page(PageId id, std::uint32_t pageSize, PageState state = PageState::Allocated)
        : id_(validatedId(id)), state_(state), data_(validatedSize(pageSize), 0) {}

    PageId id() const noexcept { return id_; }
    std::uint32_t size() const noexcept { return static_cast<std::uint32_t>(data_.size()); }
    PageState state() const noexcept { return state_; }

    bool isFree() const noexcept { return state_ == PageState::Free; }
    bool isInitialized() const noexcept { return state_ == PageState::Initialized; }

    const Byte* data() const noexcept { return data_.data(); }

    // Lê um trecho do conteúdo. Páginas livres ou nunca escritas não podem ser lidas.
    std::vector<Byte> read(std::size_t offset, std::size_t length) const {
        requireInUse("leitura");
        if (state_ == PageState::Allocated) {
            throw PageError(ErrorCode::UninitializedPage,
                            "página " + std::to_string(id_) + " nunca foi inicializada");
        }
        checkRange(offset, length);
        const auto first = data_.begin() + static_cast<std::ptrdiff_t>(offset);
        return std::vector<Byte>(first, first + static_cast<std::ptrdiff_t>(length));
    }

    // Escreve um trecho a partir de `offset`. A primeira escrita inicializa a página.
    void write(std::size_t offset, const Byte* source, std::size_t length) {
        requireInUse("escrita");
        checkRange(offset, length);
        if (length == 0) {
            return;
        }
        if (source == nullptr) {
            throw PageError(ErrorCode::InvalidOperation, "origem da escrita é nula");
        }
        std::copy_n(source, length, data_.begin() + static_cast<std::ptrdiff_t>(offset));
        state_ = PageState::Initialized;
    }

    void write(std::size_t offset, const std::vector<Byte>& content) {
        write(offset, content.data(), content.size());
    }

    // Substitui todo o conteúdo; o que sobrar da página é zerado.
    void assign(const std::vector<Byte>& content) {
        requireInUse("escrita");
        checkRange(0, content.size());
        std::fill(data_.begin(), data_.end(), Byte{0});
        std::copy(content.begin(), content.end(), data_.begin());
        state_ = PageState::Initialized;
    }

    // O checksum cobre id, estado e conteúdo, então detecta corrupção dos metadados também.
    std::uint32_t computeChecksum() const noexcept {
        Byte meta[8];
        detail::putU32(meta, id_);
        detail::putU32(meta + 4, static_cast<std::uint32_t>(state_));
        const std::uint32_t partial = detail::fnv1a(meta, sizeof(meta));
        return detail::fnv1a(data_.data(), data_.size(), partial);
    }

    // Formato em disco: [magic][id][estado][checksum][conteúdo...]
    std::vector<Byte> serialize() const {
        std::vector<Byte> out(kHeaderSize + data_.size());
        detail::putU32(out.data(), kMagic);
        detail::putU32(out.data() + 4, id_);
        detail::putU32(out.data() + 8, static_cast<std::uint32_t>(state_));
        detail::putU32(out.data() + 12, computeChecksum());
        std::copy(data_.begin(), data_.end(), out.begin() + kHeaderSize);
        return out;
    }

    // Lê e valida apenas o cabeçalho (não confere o checksum do conteúdo).
    static PageHeader parseHeader(const Byte* buffer, std::size_t length, PageId expectedId) {
        if (buffer == nullptr || length < kHeaderSize) {
            throw PageError(ErrorCode::FileCorrupted,
                            "cabeçalho da página " + std::to_string(expectedId) + " incompleto");
        }
        PageHeader header;
        header.magic = detail::getU32(buffer);
        header.id = detail::getU32(buffer + 4);
        const std::uint32_t rawState = detail::getU32(buffer + 8);
        header.checksum = detail::getU32(buffer + 12);

        if (header.magic != kMagic) {
            throw PageError(ErrorCode::FileCorrupted,
                            "assinatura inválida na página " + std::to_string(expectedId));
        }
        if (header.id != expectedId) {
            throw PageError(ErrorCode::FileCorrupted,
                            "página na posição " + std::to_string(expectedId) +
                                " declara id " + std::to_string(header.id));
        }
        if (rawState > static_cast<std::uint32_t>(PageState::Initialized)) {
            throw PageError(ErrorCode::FileCorrupted,
                            "estado desconhecido na página " + std::to_string(expectedId));
        }
        header.state = static_cast<PageState>(rawState);
        return header;
    }

    // Reconstrói uma página a partir do formato em disco, validando tudo.
    static Page deserialize(const Byte* buffer, std::size_t length,
                            std::uint32_t pageSize, PageId expectedId) {
        validatePageSize(pageSize);
        if (length != kHeaderSize + pageSize) {
            throw PageError(ErrorCode::FileCorrupted,
                            "tamanho inesperado para a página " + std::to_string(expectedId));
        }
        const PageHeader header = parseHeader(buffer, length, expectedId);
        std::vector<Byte> content(buffer + kHeaderSize, buffer + length);
        Page page(expectedId, header.state, std::move(content));
        if (page.computeChecksum() != header.checksum) {
            throw PageError(ErrorCode::ChecksumMismatch,
                            "checksum divergente na página " + std::to_string(expectedId));
        }
        return page;
    }

private:
    Page(PageId id, PageState state, std::vector<Byte>&& content)
        : id_(id), state_(state), data_(std::move(content)) {}

    static PageId validatedId(PageId id) {
        if (id == kInvalidPageId) {
            throw PageError(ErrorCode::InvalidPageId, "identificador reservado não pode ser usado");
        }
        return id;
    }

    // Valida antes de a alocação do vetor acontecer.
    static std::size_t validatedSize(std::uint32_t size) {
        validatePageSize(size);
        return size;
    }

    void requireInUse(const char* operation) const {
        if (state_ == PageState::Free) {
            throw PageError(ErrorCode::InvalidOperation,
                            std::string("operação de ") + operation + " em página livre (id " +
                                std::to_string(id_) + ")");
        }
    }

    // Comparação feita sem somar offset + length para evitar overflow.
    void checkRange(std::size_t offset, std::size_t length) const {
        if (offset > data_.size() || length > data_.size() - offset) {
            throw PageError(ErrorCode::OutOfBounds,
                            "acesso fora dos limites da página " + std::to_string(id_) +
                                " (offset " + std::to_string(offset) + ", tamanho " +
                                std::to_string(length) + ", capacidade " +
                                std::to_string(data_.size()) + ")");
        }
    }

    PageId id_;
    PageState state_;
    std::vector<Byte> data_;
};

}  // namespace PageSystem::Page
