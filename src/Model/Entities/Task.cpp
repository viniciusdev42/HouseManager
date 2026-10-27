#include "Model/Entities/Task.hpp"

#include <stdexcept>
#include <utility>

namespace model {

namespace {

constexpr const char* kWhitespace = " \t\n\r\f\v";

/// Remove espaços em branco das extremidades da string.
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

void validateId(TaskId id) {
    if (id == Task::kInvalidId) {
        throw std::invalid_argument("O ID deve ser diferente de zero.");
    }
}

/// Normaliza (trim) e valida o título, retornando o valor a ser armazenado.
std::string normalizeTitle(const std::string& rawTitle) {
    std::string title = trim(rawTitle);
    if (title.empty()) {
        throw std::invalid_argument("O titulo nao pode ser vazio.");
    }
    if (title.size() > Task::kMaxTitleLength) {
        throw std::invalid_argument("O titulo excede o tamanho maximo permitido.");
    }
    return title;
}

void validateDescription(const std::string& description) {
    if (description.size() > Task::kMaxDescriptionLength) {
        throw std::invalid_argument("A descricao excede o tamanho maximo permitido.");
    }
}

void validatePriority(Priority priority) {
    if (priority < Priority::Low || priority > Priority::Critical) {
        throw std::invalid_argument("Prioridade invalida.");
    }
}

void validateStatus(Status status) {
    if (status < Status::Pending || status > Status::Completed) {
        throw std::invalid_argument("Estado invalido.");
    }
}

}  // namespace

std::string_view toString(Priority priority) noexcept {
    switch (priority) {
        case Priority::Low:      return "Low";
        case Priority::Medium:   return "Medium";
        case Priority::High:     return "High";
        case Priority::Critical: return "Critical";
    }
    return "Unknown";
}

std::string_view toString(Status status) noexcept {
    switch (status) {
        case Status::Pending:    return "Pending";
        case Status::InProgress: return "InProgress";
        case Status::Completed:  return "Completed";
    }
    return "Unknown";
}

Task::Task(TaskId id,
           std::string title,
           std::string description,
           Priority priority,
           std::optional<TimePoint> deadline,
           Status status)
    : id_(id), priority_(priority), deadline_(deadline), status_(status) {
    validateId(id);
    validatePriority(priority);
    validateStatus(status);
    title_ = normalizeTitle(title);
    validateDescription(description);
    description_ = std::move(description);
}

void Task::setTitle(std::string title) {
    // Valida antes de modificar: garantia forte de exceção.
    title_ = normalizeTitle(title);
}

void Task::setDescription(std::string description) {
    validateDescription(description);
    description_ = std::move(description);
}

void Task::setPriority(Priority priority) {
    validatePriority(priority);
    priority_ = priority;
}

void Task::setDeadline(TimePoint deadline) noexcept {
    deadline_ = deadline;
}

void Task::clearDeadline() noexcept {
    deadline_.reset();
}

void Task::setStatus(Status status) {
    validateStatus(status);
    status_ = status;
}

void Task::markAsCompleted() noexcept {
    status_ = Status::Completed;
}

}  // namespace model
