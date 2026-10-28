#include "Storage/TaskFileStorage.hpp"
#include "Storage/TaskSerializer.hpp"

#include <fstream>
#include <ios>
#include <limits>
#include <system_error>
#include <utility>

namespace storage {

namespace fs = std::filesystem;

StorageError::StorageError(Kind kind, const std::string& message, fs::path filePath)
    : std::runtime_error(message + " [" + filePath.string() + "]"),
      kind_(kind),
      filePath_(std::move(filePath)) {}

namespace {

/// Detalhe do std::error_code para anexar às mensagens (vazio se não houver erro).
std::string detail(const std::error_code& ec) {
    return ec ? ": " + ec.message() : std::string();
}

/// Remove o arquivo temporário ao sair do escopo, a menos que a operação
/// tenha sido concluída com sucesso (release()).
class TempFileGuard {
public:
    explicit TempFileGuard(fs::path path) : path_(std::move(path)) {}
    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;

    ~TempFileGuard() {
        if (armed_) {
            std::error_code ec;
            fs::remove(path_, ec);  // Melhor esforço: erros são ignorados.
        }
    }

    void release() noexcept { armed_ = false; }

private:
    fs::path path_;
    bool armed_ = true;
};

}  // namespace

TaskFileStorage::TaskFileStorage(fs::path filePath) : filePath_(std::move(filePath)) {}

bool TaskFileStorage::exists() const noexcept {
    std::error_code ec;
    return fs::is_regular_file(filePath_, ec);
}

std::vector<model::Task> TaskFileStorage::load() const {
    using Kind = StorageError::Kind;

    std::error_code ec;
    const fs::file_status status = fs::status(filePath_, ec);
    if (status.type() == fs::file_type::not_found) {
        throw StorageError(Kind::NotFound, "Arquivo de tarefas nao encontrado", filePath_);
    }
    if (ec) {
        throw StorageError(Kind::OpenFailed,
                           "Nao foi possivel acessar o arquivo" + detail(ec), filePath_);
    }
    if (!fs::is_regular_file(status)) {
        throw StorageError(Kind::OpenFailed, "O caminho nao e um arquivo regular", filePath_);
    }

    const std::uintmax_t fileSize = fs::file_size(filePath_, ec);
    if (ec) {
        throw StorageError(Kind::ReadFailed,
                           "Nao foi possivel obter o tamanho do arquivo" + detail(ec),
                           filePath_);
    }
    if (fileSize > kMaxFileSize) {
        throw StorageError(Kind::Corrupted,
                           "O arquivo excede o tamanho maximo aceito", filePath_);
    }

    std::ifstream in(filePath_, std::ios::binary);
    if (!in) {
        throw StorageError(Kind::OpenFailed, "Nao foi possivel abrir o arquivo para leitura",
                           filePath_);
    }

    Bytes buffer(static_cast<std::size_t>(fileSize));
    if (!buffer.empty()) {
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
    }
    if (in.bad() || in.gcount() != static_cast<std::streamsize>(buffer.size())) {
        throw StorageError(Kind::ReadFailed, "Falha ao ler o conteudo do arquivo", filePath_);
    }
    // O arquivo não deve ter crescido entre a consulta do tamanho e a leitura.
    if (in.peek() != std::char_traits<char>::eof()) {
        throw StorageError(Kind::ReadFailed, "O arquivo foi modificado durante a leitura",
                           filePath_);
    }

    try {
        return TaskSerializer::deserialize(buffer);
    } catch (const SerializationError& e) {
        const Kind kind = (e.kind() == SerializationError::Kind::UnsupportedVersion)
                              ? Kind::UnsupportedVersion
                              : Kind::Corrupted;
        throw StorageError(kind, e.what(), filePath_);
    }
}

std::vector<model::Task> TaskFileStorage::loadOrEmpty() const {
    try {
        return load();
    } catch (const StorageError& e) {
        if (e.kind() == StorageError::Kind::NotFound) {
            return {};
        }
        throw;
    }
}

void TaskFileStorage::save(const std::vector<model::Task>& tasks) const {
    using Kind = StorageError::Kind;

    // 1. Serializa em memória: se a coleção for inválida, o disco nem é tocado.
    Bytes bytes;
    try {
        bytes = TaskSerializer::serialize(tasks);
    } catch (const SerializationError& e) {
        throw StorageError(Kind::InvalidData, e.what(), filePath_);
    }

    // 2. Garante que o diretório de destino exista.
    std::error_code ec;
    const fs::path parent = filePath_.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        if (ec) {
            throw StorageError(Kind::WriteFailed,
                               "Nao foi possivel criar o diretorio de destino" + detail(ec),
                               filePath_);
        }
    }

    // 3. Grava em arquivo temporário e verifica cada etapa da escrita.
    fs::path tempPath = filePath_;
    tempPath += ".tmp";
    TempFileGuard guard(tempPath);

    {
        std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw StorageError(Kind::OpenFailed,
                               "Nao foi possivel criar o arquivo temporario", tempPath);
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            throw StorageError(Kind::WriteFailed, "Falha ao gravar os dados (disco cheio?)",
                               tempPath);
        }
        out.close();
        if (out.fail()) {
            throw StorageError(Kind::WriteFailed, "Falha ao finalizar a gravacao", tempPath);
        }
    }

    // 4. Substitui o arquivo final pelo temporário.
    fs::rename(tempPath, filePath_, ec);
    if (ec) {
        throw StorageError(Kind::WriteFailed,
                           "Nao foi possivel substituir o arquivo de destino" + detail(ec),
                           filePath_);
    }
    guard.release();
}

}  // namespace storage
