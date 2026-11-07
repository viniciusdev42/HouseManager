#include "Task.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace Task {

// ---------------------------------------------------------------------------
// TaskId
// ---------------------------------------------------------------------------

TaskId::TaskId(std::string value) : value_(std::move(value)) {
    if (value_.empty()) {
        throw std::invalid_argument("O ID da tarefa não pode ser vazio.");
    }
}

const std::string& TaskId::value() const noexcept { return value_; }

bool TaskId::operator==(const TaskId& other) const noexcept {
    return value_ == other.value_;
}

bool TaskId::operator!=(const TaskId& other) const noexcept {
    return !(*this == other);
}

bool TaskId::operator<(const TaskId& other) const noexcept {
    return value_ < other.value_;
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

Task::Task(TaskId id,
           std::string title,
           std::string description,
           Priority priority,
           TimePoint deadline,
           TimePoint createdAt)
    : id_(std::move(id)),
      title_(std::move(title)),
      description_(std::move(description)),
      priority_(priority),
      deadline_(deadline),
      status_(Status::Pending),
      createdAt_(createdAt),
      completedAt_(std::nullopt) {}

Task Task::create(TaskId id,
                  std::string title,
                  std::string description,
                  Priority priority,
                  TimePoint deadline,
                  TimePoint createdAt) {
    if (title.empty()) {
        throw std::invalid_argument("O título de uma tarefa não pode ser vazio.");
    }
    ensureDeadlineNotBefore(deadline, createdAt);

    return Task(std::move(id),
                std::move(title),
                std::move(description),
                priority,
                deadline,
                createdAt);
}

void Task::start() {
    if (status_ != Status::Pending) {
        throw std::logic_error(
            "Só tarefas pendentes podem ser iniciadas.");
    }
    status_ = Status::inProgress;
}

void Task::complete(TimePoint now) {
    if (status_ == Status::Completed) {
        throw std::logic_error("A tarefa já está concluída.");
    }
    if (now < createdAt_) {
        throw std::invalid_argument(
            "A conclusão de uma tarefa não pode ser anterior à sua criação.");
    }
    status_ = Status::Completed;
    completedAt_ = now;
}

void Task::updateName(std::string newName) {
    ensureNotCompleted("alterar o nome");
    if (newName.empty()) {
        throw std::invalid_argument("O nome de uma tarefa não pode estar vazio.");
    }
    title_ = std::move(newName);
}

void Task::updateDescription(std::string newDescription) {
    ensureNotCompleted("Alterar a descrição");
    description_ = std::move(newDescription);
}

void Task::updatePriority(Priority newPriority) {
    ensureNotCompleted("Alterar a prioridade");
    priority_ = newPriority;
}

void Task::updateDeadline(TimePoint newDeadline) {
    ensureNotCompleted("alterar o prazo");
    ensureDeadlineNotBefore(newDeadline, createdAt_);
    deadline_ = newDeadline;
}

bool Task::isCompleted() const noexcept {
    return status_ == Status::completed;
}

bool Task::isPending() const noexcept {
    return status_ == Status::pending;
}

bool Task::isOverdue(TimePoint now) const noexcept {
    return !isCompleted() && now > deadline_;
}

const TaskId& Task::id() const noexcept { return id_; }

const std::string& Task::title() const noexcept { return title_; }

const std::string& Task::description() const noexcept { return description_; }

Priority Task::priority() const noexcept { return priority_; }

Task::TimePoint Task::deadline() const noexcept { return deadline_; }

Status Task::status() const noexcept { return status_; }

Task::TimePoint Task::createdAt() const noexcept { return createdAt_; }

std::optional<Task::TimePoint> Task::completedAt() const noexcept {
    return completedAt_;
}

bool Task::operator==(const Task& other) const noexcept {
    return id_ == other.id_;
}

bool Task::operator!=(const Task& other) const noexcept {
    return !(*this == other);
}

void Task::ensureNotCompleted(const char* operation) const {
    if (isCompleted()) {
        throw std::logic_error(std::string("Não é possível ") +
                               operation + " de uma tarefa concluída.");
    }
}

void Task::ensureDeadlineNotBefore(TimePoint deadline, TimePoint createdAt) {
    if (deadline < createdAt) {
        throw std::invalid_argument(
            "O prazo de uma tarefa não pode ser anterior à data de sua criação.");
    }
}

// ---------------------------------------------------------------------------
// TaskUrgencyComparator
// ---------------------------------------------------------------------------

bool TaskUrgencyComparator::operator()(const Task& a,
                                       const Task& b) const noexcept {
    // 1. Prioridade: o enum é declarado de Low a Critical, então o maior
    //    valor representa a maior prioridade.
    if (a.priority() != b.priority()) {
        return static_cast<int>(a.priority()) > static_cast<int>(b.priority());
    }

    // 2. Prazo: o mais próximo é mais urgente.
    if (a.deadline() != b.deadline()) {
        return a.deadline() < b.deadline();
    }

    // 3. Data de criação: a mais antiga espera há mais tempo.
    if (a.createdAt() != b.createdAt()) {
        return a.createdAt() < b.createdAt();
    }

    // 4. Desempate final pela identidade, garantindo ordem total.
    return a.id() < b.id();
}

}  // namespace Task
