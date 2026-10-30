#ifndef TASKMANAGER_MODEL_ENTITIES_PROJECT_HPP
#define TASKMANAGER_MODEL_ENTITIES_PROJECT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Entities::Project {

// Aliases compartilhados com a camada de domínio. Redeclarações idênticas
// (por exemplo, em Task.hpp) são válidas em C++.
using ProjectId = std::uint64_t;
using TaskId = std::uint64_t;
using Clock = std::chrono::system_clock;
using TimePoint = Clock::time_point;

/// Estados possíveis de um projeto.
enum class ProjectStatus : std::uint8_t {
    Active,     ///< Em andamento; aceita alterações.
    Completed,  ///< Concluído; metadados editáveis, mas sem novas tarefas.
    Archived    ///< Arquivado; somente leitura até ser desarquivado.
};

/// Representação textual do estado (útil para GUI, logs e serialização).
std::string_view toString(ProjectStatus status) noexcept;

/// Entidade de domínio que agrupa tarefas relacionadas.
///
/// O projeto referencia as tarefas apenas pelo identificador (`TaskId`),
/// sem duplicar objetos `Task`, e não depende de GUI, armazenamento ou
/// estruturas de dados da infraestrutura.
///
/// Tratamento de erros:
///  - `std::invalid_argument`: dados inválidos (nome vazio, id zero, etc.).
///  - `std::logic_error`: operação incompatível com o estado atual.
class Project {
public:
    static constexpr ProjectId kInvalidId = 0;
    static constexpr std::size_t kMaxNameLength = 120;
    static constexpr std::size_t kMaxDescriptionLength = 4096;

    /// Cria um projeto ativo e sem tarefas.
    /// O nome é normalizado (espaços nas extremidades são removidos).
    /// @throws std::invalid_argument se id, nome, descrição ou prazo forem inválidos.
    Project(ProjectId id,
            std::string name,
            std::string description = {},
            std::optional<TimePoint> deadline = std::nullopt,
            TimePoint createdAt = Clock::now());

    /// Reconstrói um projeto a partir de dados já persistidos, validando
    /// todos os invariantes. Destinada à camada de serialização.
    /// @throws std::invalid_argument se algum dado for inconsistente.
    static Project restore(ProjectId id,
                           std::string name,
                           std::string description,
                           ProjectStatus status,
                           std::optional<TimePoint> deadline,
                           TimePoint createdAt,
                           TimePoint updatedAt,
                           std::vector<TaskId> taskIds);

    // --- Consultas ---------------------------------------------------------

    ProjectId id() const noexcept { return id_; }
    const std::string& name() const noexcept { return name_; }
    const std::string& description() const noexcept { return description_; }
    ProjectStatus status() const noexcept { return status_; }
    const std::optional<TimePoint>& deadline() const noexcept { return deadline_; }
    TimePoint createdAt() const noexcept { return createdAt_; }
    TimePoint updatedAt() const noexcept { return updatedAt_; }

    /// Identificadores das tarefas, na ordem de associação.
    const std::vector<TaskId>& taskIds() const noexcept { return taskIds_; }
    std::size_t taskCount() const noexcept { return taskIds_.size(); }
    bool hasTask(TaskId taskId) const noexcept;

    bool isActive() const noexcept { return status_ == ProjectStatus::Active; }
    bool isCompleted() const noexcept { return status_ == ProjectStatus::Completed; }
    bool isArchived() const noexcept { return status_ == ProjectStatus::Archived; }

    /// Verdadeiro se há prazo vencido e o projeto ainda está ativo.
    bool isOverdue(TimePoint now = Clock::now()) const noexcept;

    // --- Edição de metadados (exige projeto não arquivado) -----------------

    void rename(std::string newName);
    void setDescription(std::string newDescription);
    void setDeadline(TimePoint deadline);
    void clearDeadline();

    // --- Tarefas -----------------------------------------------------------

    /// Associa uma tarefa ao projeto (exige projeto ativo).
    /// @return false se a tarefa já estava associada.
    bool addTask(TaskId taskId);

    /// Remove a associação com uma tarefa (exige projeto não arquivado).
    /// @return false se a tarefa não estava associada.
    bool removeTask(TaskId taskId);

    // --- Transições de estado ---------------------------------------------
    // Active -> Completed -> Active ; Active/Completed -> Archived -> Active
    //
    // A verificação de que todas as tarefas estão concluídas antes de
    // completar o projeto é regra da camada de aplicação, pois o projeto
    // conhece apenas os identificadores das tarefas.

    void complete();
    void reopen();
    void archive();
    void unarchive();

    /// Dois projetos são iguais quando possuem o mesmo identificador.
    friend bool operator==(const Project& a, const Project& b) noexcept {
        return a.id_ == b.id_;
    }
    friend bool operator!=(const Project& a, const Project& b) noexcept {
        return !(a == b);
    }

private:
    void ensureNotArchived(std::string_view operation) const;
    void touch() noexcept;

    ProjectId id_;
    std::string name_;
    std::string description_;
    ProjectStatus status_{ProjectStatus::Active};
    std::optional<TimePoint> deadline_;
    TimePoint createdAt_;
    TimePoint updatedAt_;
    std::vector<TaskId> taskIds_;
};

}  // namespace Entities::Project
