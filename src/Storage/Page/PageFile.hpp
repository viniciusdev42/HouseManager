#ifndef STORAGE_PAGE_FILE_HPP
#define STORAGE_PAGE_FILE_HPP

// PageFile.hpp
// Persistência de páginas em um arquivo binário.
//
// Layout do arquivo:
//   [cabeçalho do arquivo: 24 bytes]
//   [página 0][página 1]...[página N-1]
// Cada página ocupa Page::kHeaderSize + pageSize bytes.
//
// Cabeçalho do arquivo (little-endian):
//   magic | versão | pageSize | pageCount | rootPageId | checksum dos 20 bytes anteriores
// rootPageId vale kInvalidPageId quando não há página raiz definida.

#include "Page.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

namespace PageSystem::PageFile  {

class PageFile {
public:
    static constexpr std::uint32_t kMagic = 0x31464750u;  // "PGF1"
    static constexpr std::uint32_t kVersion = 2;
    // A versão 1 tinha esse campo reservado (sempre 0); é aberta como "sem raiz"
    // e passa a versão 2 na próxima gravação do cabeçalho.
    static constexpr std::uint32_t kLegacyVersion = 1;
    static constexpr std::size_t kFileHeaderSize = 24;

    PageFile(const PageFile&) = delete;
    PageFile& operator=(const PageFile&) = delete;
    PageFile(PageFile&&) = default;
    PageFile& operator=(PageFile&&) = default;

    ~PageFile() {
        try {
            close();
        } catch (...) {
            // Destrutores não podem lançar; chame close() explicitamente para tratar falhas.
        }
    }

    // Cria um novo arquivo de armazenamento vazio (zero páginas).
    static PageFile create(const std::filesystem::path& path,
                           std::uint32_t pageSize = kDefaultPageSize,
                           bool overwrite = false) {
        validatePageSize(pageSize);

        std::error_code ec;
        if (!overwrite && std::filesystem::exists(path, ec)) {
            throw PageError(ErrorCode::FileAlreadyExists,
                            "o arquivo já existe: " + path.string());
        }

        PageFile file(path, pageSize, 0, kInvalidPageId);
        file.stream_.open(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
        if (!file.stream_.is_open()) {
            throw PageError(ErrorCode::IoError, "não foi possível criar o arquivo: " + path.string());
        }
        file.writeFileHeader();
        return file;
    }

    // Abre um arquivo existente, validando cabeçalho e tamanho total.
    static PageFile open(const std::filesystem::path& path) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            throw PageError(ErrorCode::FileNotFound,
                            "arquivo inexistente ou inválido: " + path.string());
        }
        const std::uintmax_t actualSize = std::filesystem::file_size(path, ec);
        if (ec) {
            throw PageError(ErrorCode::IoError,
                            "não foi possível obter o tamanho de " + path.string());
        }
        if (actualSize < kFileHeaderSize) {
            throw PageError(ErrorCode::FileCorrupted,
                            "arquivo menor que o cabeçalho: " + path.string());
        }

        std::fstream stream(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!stream.is_open()) {
            throw PageError(ErrorCode::IoError, "não foi possível abrir o arquivo: " + path.string());
        }

        Byte buffer[kFileHeaderSize];
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(kFileHeaderSize));
        if (!stream) {
            throw PageError(ErrorCode::IoError, "falha ao ler o cabeçalho de " + path.string());
        }

        const FileHeader header = parseFileHeader(buffer);
        if (actualSize != expectedFileSize(header.pageSize, header.pageCount)) {
            throw PageError(ErrorCode::FileCorrupted,
                            "tamanho do arquivo incompatível com o número de páginas declarado");
        }

        if (header.rootPageId != kInvalidPageId && header.rootPageId >= header.pageCount) {
            throw PageError(ErrorCode::FileCorrupted,
                            "página raiz " + std::to_string(header.rootPageId) +
                                " fora do intervalo de páginas do arquivo");
        }

        PageFile file(path, header.pageSize, header.pageCount, header.rootPageId);
        file.stream_ = std::move(stream);
        return file;
    }

    const std::filesystem::path& path() const noexcept { return path_; }
    std::uint32_t pageSize() const noexcept { return pageSize_; }
    PageId pageCount() const noexcept { return pageCount_; }
    bool isOpen() const noexcept { return stream_.is_open(); }
    PageId rootPageId() const noexcept { return rootPageId_; }

    // Lê uma página completa, validando assinatura, id e checksum.
    Page readPage(PageId id) {
        requireOpen();
        requireExistingId(id);
        std::vector<Byte> buffer(slotSize());
        readAt(offsetOf(id), buffer.data(), buffer.size());
        return Page::deserialize(buffer.data(), buffer.size(), pageSize_, id);
    }

    // Lê somente o cabeçalho da página (rápido; não valida o checksum do conteúdo).
    PageHeader readPageHeader(PageId id) {
        requireOpen();
        requireExistingId(id);
        Byte buffer[Page::kHeaderSize];
        readAt(offsetOf(id), buffer, sizeof(buffer));
        return Page::parseHeader(buffer, sizeof(buffer), id);
    }

    // Grava a página na posição indicada por seu id. Escrever no id == pageCount()
    // adiciona uma nova página ao fim; ids maiores deixariam lacunas e são rejeitados.
    void writePage(const Page& page) {
        requireOpen();
        if (page.size() != pageSize_) {
            throw PageError(ErrorCode::InvalidOperation,
                            "página de " + std::to_string(page.size()) +
                                " bytes em arquivo com páginas de " + std::to_string(pageSize_));
        }
        const PageId id = page.id();
        if (id > pageCount_) {
            throw PageError(ErrorCode::InvalidPageId,
                            "escrita em " + std::to_string(id) + " deixaria lacunas (total: " +
                                std::to_string(pageCount_) + ")");
        }
        const bool grows = (id == pageCount_);
        if (grows && pageCount_ >= kInvalidPageId) {
            throw PageError(ErrorCode::InvalidOperation, "limite de páginas do arquivo atingido");
        }

        const std::vector<Byte> bytes = page.serialize();
        writeAt(offsetOf(id), bytes.data(), bytes.size());

        if (grows) {
            // A página é gravada antes do cabeçalho: se algo falhar no meio, o tamanho do
            // arquivo deixa de bater com o cabeçalho e open() reporta corrupção.
            ++pageCount_;
            try {
                writeFileHeader();
            } catch (...) {
                --pageCount_;
                throw;
            }
        }
    }

    // Define a página raiz e grava o cabeçalho. kInvalidPageId remove a raiz.
    void setRootPageId(PageId id) {
        requireOpen();
        if (id != kInvalidPageId) {
            requireExistingId(id);
        }
        const PageId previous = rootPageId_;
        rootPageId_ = id;
        try {
            writeFileHeader();
        } catch (...) {
            rootPageId_ = previous;
            throw;
        }
    }

    // Reconfere no disco o cabeçalho e o tamanho do arquivo.
    void verifyStructure() {
        requireOpen();
        Byte buffer[kFileHeaderSize];
        readAt(0, buffer, sizeof(buffer));
        const FileHeader header = parseFileHeader(buffer);
        if (header.pageSize != pageSize_ || header.pageCount != pageCount_ ||
            header.rootPageId != rootPageId_) {
            throw PageError(ErrorCode::FileCorrupted,
                            "cabeçalho em disco diverge do estado em memória");
        }
        std::error_code ec;
        const std::uintmax_t actualSize = std::filesystem::file_size(path_, ec);
        if (ec) {
            throw PageError(ErrorCode::IoError, "não foi possível obter o tamanho de " + path_.string());
        }
        if (actualSize != expectedFileSize(pageSize_, pageCount_)) {
            throw PageError(ErrorCode::FileCorrupted,
                            "tamanho do arquivo incompatível com o número de páginas");
        }
    }

    // Empurra os dados para o sistema operacional.
    void flush() {
        requireOpen();
        stream_.flush();
        if (!stream_) {
            stream_.clear();
            throw PageError(ErrorCode::IoError, "falha ao descarregar dados em " + path_.string());
        }
    }

    void close() {
        if (!stream_.is_open()) {
            return;
        }
        stream_.flush();
        const bool flushed = static_cast<bool>(stream_);
        stream_.close();
        if (!flushed) {
            throw PageError(ErrorCode::IoError, "falha ao fechar " + path_.string());
        }
    }

private:
    struct FileHeader {
        std::uint32_t version;
        std::uint32_t pageSize;
        std::uint32_t pageCount;
        PageId rootPageId;
    };

    PageFile(std::filesystem::path path, std::uint32_t pageSize, PageId pageCount,
             PageId rootPageId)
        : path_(std::move(path)),
          pageSize_(pageSize),
          pageCount_(pageCount),
          rootPageId_(rootPageId) {}

    static std::uintmax_t expectedFileSize(std::uint32_t pageSize, PageId pageCount) noexcept {
        const std::uintmax_t slot = Page::kHeaderSize + pageSize;
        return kFileHeaderSize + static_cast<std::uintmax_t>(pageCount) * slot;
    }

    static FileHeader parseFileHeader(const Byte* buffer) {
        if (detail::getU32(buffer) != kMagic) {
            throw PageError(ErrorCode::FileCorrupted, "assinatura do arquivo inválida");
        }
        if (detail::getU32(buffer + 20) != detail::fnv1a(buffer, 20)) {
            throw PageError(ErrorCode::FileCorrupted, "checksum do cabeçalho do arquivo inválido");
        }
        FileHeader header;
        header.version = detail::getU32(buffer + 4);
        header.pageSize = detail::getU32(buffer + 8);
        header.pageCount = detail::getU32(buffer + 12);
        header.rootPageId = (header.version == kLegacyVersion) ? kInvalidPageId
                                                                : detail::getU32(buffer + 16);
        if (header.version != kVersion && header.version != kLegacyVersion) {
            throw PageError(ErrorCode::FileCorrupted,
                            "versão de arquivo não suportada: " + std::to_string(header.version));
        }
        if (!isValidPageSize(header.pageSize)) {
            throw PageError(ErrorCode::FileCorrupted, "tamanho de página inválido no cabeçalho");
        }
        return header;
    }

    void writeFileHeader() {
        Byte buffer[kFileHeaderSize] = {};
        detail::putU32(buffer, kMagic);
        detail::putU32(buffer + 4, kVersion);
        detail::putU32(buffer + 8, pageSize_);
        detail::putU32(buffer + 12, pageCount_);
        detail::putU32(buffer + 16, rootPageId_);
        detail::putU32(buffer + 20, detail::fnv1a(buffer, 20));
        writeAt(0, buffer, sizeof(buffer));
    }

    std::size_t slotSize() const noexcept { return Page::kHeaderSize + pageSize_; }

    std::streamoff offsetOf(PageId id) const noexcept {
        return static_cast<std::streamoff>(kFileHeaderSize +
                                           static_cast<std::uint64_t>(id) * slotSize());
    }

    void requireOpen() const {
        if (!stream_.is_open()) {
            throw PageError(ErrorCode::InvalidOperation, "arquivo fechado: " + path_.string());
        }
    }

    void requireExistingId(PageId id) const {
        if (id == kInvalidPageId || id >= pageCount_) {
            throw PageError(ErrorCode::InvalidPageId,
                            "página " + std::to_string(id) + " não existe (total: " +
                                std::to_string(pageCount_) + ")");
        }
    }

    // Limpa o estado do stream antes de cada operação para que uma falha anterior
    // não impeça o uso posterior do arquivo.
    void readAt(std::streamoff offset, Byte* destination, std::size_t length) {
        stream_.clear();
        stream_.seekg(offset);
        stream_.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(length));
        if (!stream_) {
            stream_.clear();
            throw PageError(ErrorCode::IoError,
                            "falha de leitura em " + path_.string() + " (offset " +
                                std::to_string(static_cast<long long>(offset)) + ")");
        }
    }

    void writeAt(std::streamoff offset, const Byte* source, std::size_t length) {
        stream_.clear();
        stream_.seekp(offset);
        stream_.write(reinterpret_cast<const char*>(source), static_cast<std::streamsize>(length));
        stream_.flush();
        if (!stream_) {
            stream_.clear();
            throw PageError(ErrorCode::IoError,
                            "falha de escrita em " + path_.string() + " (offset " +
                                std::to_string(static_cast<long long>(offset)) + ")");
        }
    }

    std::filesystem::path path_;
    std::uint32_t pageSize_;
    PageId pageCount_;
    PageId rootPageId_;
    std::fstream stream_;
};

}  // namespace PageSystem::PageFile 

#endif // STORAGE_PAGE_FILE_HPP