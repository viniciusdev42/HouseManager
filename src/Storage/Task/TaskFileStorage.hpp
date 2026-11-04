#ifndef TASK_MANAGER_FILE_STORAGE_HPP
#define TASK_MANAGER_FILE_STORAGE_HPP

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "Model/Entities/Task.hpp"

namespace storage {

/// Erro de armazenamento (acesso ao arquivo ou conteúdo inválido).
///
/// what() inclui o caminho do arquivo envolvido.
class StorageError : public std::runtime_error {
public:
    enum class Kind {
        NotFound,            ///< O arquivo não existe.
        OpenFailed,          ///< Não foi possível abrir/criar o arquivo (ou não é um arquivo regular).
        ReadFailed,          ///< Falha de E/S durante a leitura.
        WriteFailed,         ///< Falha de E/S durante a escrita ou substituição do arquivo.
        Corrupted,           ///< O conteúdo está truncado, corrompido ou inválido.
        UnsupportedVersion,  ///< O arquivo foi gravado em uma versão não suportada.
        InvalidData          ///< A coleção a ser gravada é inválida (ex.: IDs duplicados).
    };

    StorageError(Kind kind, const std::string& message, std::filesystem::path filePath);

    Kind kind() const noexcept { return kind_; }
    const std::filesystem::path& filePath() const noexcept { return filePath_; }

private:
    Kind kind_;
    std::filesystem::path filePath_;
};

/// Salva e carrega a coleção de tarefas em um único arquivo no disco.
///
/// Responsabilidades:
///  - ler e escrever bytes no arquivo, tratando falhas de E/S;
///  - delegar o formato a TaskSerializer;
///  - traduzir erros de conteúdo e de E/S para StorageError.
///
/// Esta classe não conhece índices, estruturas de dados nem regras de
/// negócio: recebe e devolve apenas coleções de model::Task. Índices derivados
/// devem ser reconstruídos por quem chama a partir das tarefas carregadas.
///
/// Gravação segura: os dados são escritos em um arquivo temporário
/// ("<arquivo>.tmp") no mesmo diretório e, só depois de gravados com sucesso,
/// substituem o arquivo final. Assim, uma falha durante a escrita não destrói
/// a versão anterior. Não há garantias transacionais completas (por exemplo,
/// não é feito fsync) nem proteção contra gravações concorrentes.
class TaskFileStorage {
public:
    /// Tamanho máximo aceito ao carregar, para evitar alocações absurdas
    /// caso o caminho aponte para um arquivo que não é de tarefas.
    static constexpr std::uintmax_t kMaxFileSize = 256ull * 1024ull * 1024ull;

    explicit TaskFileStorage(std::filesystem::path filePath);

    const std::filesystem::path& filePath() const noexcept { return filePath_; }

    /// Indica se existe um arquivo regular no caminho configurado.
    bool exists() const noexcept;

    /// Carrega todas as tarefas do arquivo.
    /// @throws StorageError (NotFound) se o arquivo não existir;
    ///         (OpenFailed/ReadFailed) em falhas de E/S;
    ///         (Corrupted/UnsupportedVersion) se o conteúdo for inválido.
    std::vector<model::Task> load() const;

    /// Como load(), mas retorna uma coleção vazia se o arquivo não existir
    /// (caso típico da primeira execução). Demais falhas continuam sendo lançadas.
    std::vector<model::Task> loadOrEmpty() const;

    /// Grava a coleção, substituindo atomicamente o conteúdo anterior.
    /// Cria os diretórios ausentes do caminho. Em caso de falha, o arquivo
    /// anterior (se existir) permanece intacto.
    /// @throws StorageError (InvalidData) se a coleção for inválida;
    ///         (OpenFailed/WriteFailed) em falhas de E/S.
    void save(const std::vector<model::Task>& tasks) const;

private:
    std::filesystem::path filePath_;
};

}  // namespace storage

#endif  // TASK_MANAGER_FILE_STORAGE_HPP
