#ifndef ENTITIES_TASK_HPP
#define ENTITIES_TASK_HPP

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace Task {

// Value object que representa a identidade de uma Task.
// Imutável e sempre válido: não existe TaskId vazio.
class TaskId {
public:
    // Lança std::invalid_argument se value for vazio.
    explicit TaskId(std::string value);

    const std::string& value() const noexcept;

    bool operator==(const TaskId& other) const noexcept;
    bool operator!=(const TaskId& other) const noexcept;
    bool operator<(const TaskId& other) const noexcept;

private:
    std::string value_;
};

enum class Priority {
    Low,
    Medium,
    High,
    Critical
};

enum class Status {
    Pending,
    inProgress,
    Completed
};

class Task {
public:
    using Clock = std::chrono::system_clock;
    using TimePoint = Clock::time_point;

    // Factory method: único ponto de criação, garante que a entidade
    // nasce sempre em um estado válido (status = Pending).
    // Lança std::invalid_argument se title for vazio ou se deadline
    // for anterior a createdAt.
    static Task create(TaskId id,
                       std::string title,
                       std::string description,
                       Priority priority,
                       TimePoint deadline,
                       TimePoint createdAt = Clock::now());

    // Transição Pending -> inProgress.
    // Lança std::logic_error se a tarefa não estiver pendente.
    void start();

    // Transição Pending/inProgress -> Completed e registra completedAt.
    // Lança std::logic_error se a tarefa já estiver concluída.
    // Lança std::invalid_argument se "now" for anterior a createdAt.
    void complete(TimePoint now = Clock::now());

    // Atualizações: tarefas concluídas são imutáveis, então todas lançam
    // std::logic_error quando a tarefa já está concluída.

    // Atualiza o nome (title) da tarefa. Lança std::invalid_argument se vazio.
    void updateName(std::string newName);
    void updateDescription(std::string newDescription);
    void updatePriority(Priority newPriority);
    // Lança std::invalid_argument se o novo prazo for anterior a createdAt.
    void updateDeadline(TimePoint newDeadline);

    bool isCompleted() const noexcept;
    bool isPending() const noexcept;

    // Uma tarefa está atrasada se não foi concluída e o prazo já passou.
    // O parâmetro "now" permite injetar o tempo atual (determinismo).
    bool isOverdue(TimePoint now = Clock::now()) const noexcept;

    const TaskId& id() const noexcept;
    const std::string& title() const noexcept;
    const std::string& description() const noexcept;
    Priority priority() const noexcept;
    TimePoint deadline() const noexcept;
    Status status() const noexcept;
    TimePoint createdAt() const noexcept;
    // Vazio enquanto a tarefa não foi concluída.
    std::optional<TimePoint> completedAt() const noexcept;

    // Igualdade de entidade: duas Tasks são iguais se possuem o mesmo TaskId,
    // independentemente de prioridade, prazo ou qualquer outro atributo.
    bool operator==(const Task& other) const noexcept;
    bool operator!=(const Task& other) const noexcept;

private:
    Task(TaskId id,
         std::string title,
         std::string description,
         Priority priority,
         TimePoint deadline,
         TimePoint createdAt);

    void ensureNotCompleted(const char* operation) const;
    static void ensureDeadlineNotBefore(TimePoint deadline,
                                        TimePoint createdAt);

    TaskId id_;
    std::string title_;
    std::string description_;
    Priority priority_;
    TimePoint deadline_;
    Status status_;
    TimePoint createdAt_;
    std::optional<TimePoint> completedAt_;
};

// Ordenação estrita e total por urgência: "a" vem antes de "b" quando é mais
// urgente. Usável diretamente em std::sort, std::set, std::priority_queue etc.
//
// Critérios, em ordem de desempate:
//   1. Maior prioridade primeiro (Critical >High > Medium > Low).
//   2. Prazo mais próximo primeiro.
//   3. Criada há mais tempo primeiro (createdAt mais antigo).
//   4. Menor TaskId (desempate final, garante ordem total e determinística).
//
// Como o TaskId é único, duas Tasks distintas nunca são equivalentes, o que
// evita o descarte silencioso de elementos em contêineres ordenados.
struct TaskUrgencyComparator {
    bool operator()(const Task& a, const Task& b) const noexcept;
};

}  // namespace Task

// Permite usar TaskId como chave em std::unordered_map / std::unordered_set.
namespace std {
template <>
struct hash<Task::TaskId> {
    size_t operator()(const Task::TaskId& id) const noexcept {
        return hash<string>{}(id.value());
    }
};
}  // namespace std

#endif  // ENTITIES_TASK_HPP
