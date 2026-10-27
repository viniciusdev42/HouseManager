#ifndef TASKMANAGER_MODEL_ENTITIES_TASK_HPP
#define TASKMANAGER_MODEL_ENTITIES_TASK_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace model {

/// Identificador estável e único de uma tarefa.
/// O valor 0 é reservado como "inválido". A geração de IDs é responsabilidade
/// da camada de serviços/repositórios, mantendo a entidade livre de estado global.
using TaskId = std::uint64_t;

/// Instante de tempo utilizado pelo modelo (ex.: prazo de uma tarefa).
using TimePoint = std::chrono::system_clock::time_point;

/// Nível de prioridade de uma tarefa. Valores maiores = mais prioritário,
/// o que facilita a comparação em heaps e ordenações.
enum class Priority : std::uint8_t {
    Low = 0,
    Medium = 1,
    High = 2,
    Critical = 3
};

/// Estado de andamento de uma tarefa.
/// "Atrasada" não é um estado: é derivado do prazo e do estado atual.
enum class Status : std::uint8_t {
    Pending = 0,
    InProgress = 1,
    Completed = 2
};

/// Representação textual da prioridade ("Low", "Medium", "High", "Critical").
/// Retorna "Unknown" para valores fora do enumerador.
std::string_view toString(Priority priority) noexcept;

/// Representação textual do estado ("Pending", "InProgress", "Completed").
/// Retorna "Unknown" para valores fora do enumerador.
std::string_view toString(Status status) noexcept;

/// Entidade que representa uma tarefa.
///
/// A identidade da entidade é definida exclusivamente pelo seu ID.
/// Esta classe não depende de View, Controller, serialização nem de
/// estruturas de dados.
///
/// Invariantes garantidas:
///  - o ID é diferente de zero e imutável;
///  - o título, sem espaços nas extremidades, não é vazio e tem no máximo
///    kMaxTitleLength bytes;
///  - a descrição tem no máximo kMaxDescriptionLength bytes;
///  - a prioridade e o estado são sempre valores válidos dos seus enums.
///
/// Violações lançam std::invalid_argument. Os métodos modificadores oferecem
/// garantia forte de exceção: em caso de falha, o objeto permanece inalterado.
class Task {
public:
    static constexpr TaskId kInvalidId = 0;
    static constexpr std::size_t kMaxTitleLength = 200;
    static constexpr std::size_t kMaxDescriptionLength = 5000;

    /// Cria uma tarefa. O prazo é opcional (sem prazo por padrão) e o estado
    /// inicial padrão é Status::Pending.
    /// Prazos no passado são aceitos, pois permitem identificar tarefas atrasadas.
    /// @throws std::invalid_argument se alguma invariante for violada.
    Task(TaskId id,
         std::string title,
         std::string description = {},
         Priority priority = Priority::Medium,
         std::optional<TimePoint> deadline = std::nullopt,
         Status status = Status::Pending);

    // --- Consultas ---------------------------------------------------------

    TaskId id() const noexcept { return id_; }
    const std::string& title() const noexcept { return title_; }
    const std::string& description() const noexcept { return description_; }
    Priority priority() const noexcept { return priority_; }
    const std::optional<TimePoint>& deadline() const noexcept { return deadline_; }
    Status status() const noexcept { return status_; }

    bool hasDeadline() const noexcept { return deadline_.has_value(); }
    bool isCompleted() const noexcept { return status_ == Status::Completed; }

    // --- Modificadores -----------------------------------------------------

    /// @throws std::invalid_argument se o título for vazio ou muito longo.
    void setTitle(std::string title);

    /// @throws std::invalid_argument se a descrição for muito longa.
    void setDescription(std::string description);

    /// @throws std::invalid_argument se a prioridade não for um valor válido.
    void setPriority(Priority priority);

    void setDeadline(TimePoint deadline) noexcept;
    void clearDeadline() noexcept;

    /// @throws std::invalid_argument se o estado não for um valor válido.
    void setStatus(Status status);

    /// Atalho para marcar a tarefa como concluída.
    void markAsCompleted() noexcept;

    // --- Identidade --------------------------------------------------------

    /// Duas tarefas são iguais se possuem o mesmo ID.
    friend bool operator==(const Task& lhs, const Task& rhs) noexcept {
        return lhs.id_ == rhs.id_;
    }
    friend bool operator!=(const Task& lhs, const Task& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    TaskId id_;
    std::string title_;
    std::string description_;
    Priority priority_;
    std::optional<TimePoint> deadline_;
    Status status_;
};

}  // namespace model

#endif  // TASKMANAGER_MODEL_ENTITIES_TASK_HPP
