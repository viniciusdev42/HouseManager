#include "Model/Entities/Dependency.hpp"

#include <stdexcept>

namespace model {

namespace {

/// Mistura de bits (estilo boost::hash_combine) para que pares simétricos,
/// como (1, 2) e (2, 1), não produzam o mesmo hash.
std::size_t combineHash(std::size_t seed, std::size_t value) noexcept {
    constexpr std::size_t kGoldenRatio = static_cast<std::size_t>(0x9e3779b97f4a7c15ULL);
    return seed ^ (value + kGoldenRatio + (seed << 6) + (seed >> 2));
}

/// Valida o par de IDs, lançando std::invalid_argument se violar as invariantes.
void validatePair(TaskId prerequisiteId, TaskId dependentId) {
    if (prerequisiteId == Task::kInvalidId) {
        throw std::invalid_argument("O ID da tarefa pre-requisito deve ser diferente de zero.");
    }
    if (dependentId == Task::kInvalidId) {
        throw std::invalid_argument("O ID da tarefa dependente deve ser diferente de zero.");
    }
    if (prerequisiteId == dependentId) {
        throw std::invalid_argument("Uma tarefa nao pode depender de si mesma.");
    }
}

}  // namespace

Dependency::Dependency(TaskId prerequisiteId, TaskId dependentId)
    : prerequisiteId_(prerequisiteId), dependentId_(dependentId) {
    validatePair(prerequisiteId, dependentId);
}

bool Dependency::isValidPair(TaskId prerequisiteId, TaskId dependentId) noexcept {
    return prerequisiteId != Task::kInvalidId &&
           dependentId != Task::kInvalidId &&
           prerequisiteId != dependentId;
}

std::size_t Dependency::hashValue() const noexcept {
    const std::size_t seed = std::hash<TaskId>{}(prerequisiteId_);
    return combineHash(seed, std::hash<TaskId>{}(dependentId_));
}

std::string Dependency::toString() const {
    return std::to_string(prerequisiteId_) + " -> " + std::to_string(dependentId_);
}

}  // namespace model
