#ifndef  SERVICES_DEPENDENCY_GRAPH_HPP
#define  SERVICES_DEPENDENCY_GRAPH_HPP

#include <cstddef>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "Model/Entities/Dependency.hpp"
#include "Model/Structures/Graph/Graph.hpp"

namespace Graph::Services{

/// Gerencia o conjunto de dependências entre tarefas sobre um grafo genérico.
///
/// Esta classe é o único ponto que conhece, ao mesmo tempo, as entidades de
/// domínio (Dependency, TaskId) e o módulo de grafos. Ela traduz cada TaskId
/// para um índice de vértice e cada Dependency para a aresta dirigida
/// prerequisiteId -> dependentId. Assim, Graph permanece reutilizável
/// e sem qualquer referência ao modelo de tarefas.
///
/// Regras aplicadas ao adicionar:
///  - dependências duplicadas são rejeitadas (AlreadyExists);
///  - dependências que fechariam um ciclo são rejeitadas (WouldCreateCycle),
///    mantendo o grafo sempre acíclico e, portanto, ordenável topologicamente.
///
/// Tarefas sem nenhuma dependência não ocupam espaço no grafo; um vértice é
/// criado apenas quando a tarefa aparece pela primeira vez em uma dependência.
class DependencyGraph {
public:
    using Dependency = Entities::Dependency::Dependency;

    // Deduzido da entidade para não depender de onde TaskId foi declarado.
    using TaskId = std::decay_t<decltype(std::declval<const Dependency&>().prerequisiteId())>;

    enum class AddResult {
        Added,
        AlreadyExists,
        WouldCreateCycle
    };

    DependencyGraph() : graph_(0, graph::GraphType::Directed) {}

    // --- Adição ------------------------------------------------------------

    /// Adiciona a dependência, se ela for nova e não formar ciclo.
    [[nodiscard]] AddResult addDependency(const Dependency& dependency) {
        const std::optional<graph::Graph::Vertex> prerequisite =
            findVertex(dependency.prerequisiteId());
        const std::optional<graph::Graph::Vertex> dependent =
            findVertex(dependency.dependentId());

        // Se alguma das tarefas ainda não está no grafo, ela não possui arestas
        // e, portanto, não há duplicata nem ciclo possível.
        if (prerequisite && dependent) {
            if (graph_.hasEdge(*prerequisite, *dependent)) {
                return AddResult::AlreadyExists;
            }
            // A nova aresta prerequisite -> dependent fecha um ciclo se, e somente
            // se, dependent já alcança prerequisite.
            if (graph_.hasPath(*dependent, *prerequisite)) {
                return AddResult::WouldCreateCycle;
            }
        }

        graph_.addEdge(obtainVertex(dependency.prerequisiteId()),
                       obtainVertex(dependency.dependentId()));
        return AddResult::Added;
    }

    /// Atalho que constrói a Dependency a partir dos IDs.
    /// @throws std::invalid_argument se os IDs forem inválidos ou iguais
    ///         (validação feita pela própria entidade).
    [[nodiscard]] AddResult addDependency(TaskId prerequisiteId, TaskId dependentId) {
        return addDependency(Dependency(prerequisiteId, dependentId));
    }

    // --- Remoção -----------------------------------------------------------

    /// Remove a dependência.
    /// @return true se ela existia e foi removida; false caso contrário.
    bool removeDependency(const Dependency& dependency) {
        return removeDependency(dependency.prerequisiteId(), dependency.dependentId());
    }

    /// Remove a dependência identificada pelo par de IDs. Pares inválidos ou
    /// inexistentes simplesmente retornam false, sem lançar exceção.
    bool removeDependency(TaskId prerequisiteId, TaskId dependentId) {
        const auto prerequisite = findVertex(prerequisiteId);
        const auto dependent = findVertex(dependentId);
        if (!prerequisite || !dependent) return false;

        return graph_.removeEdge(*prerequisite, *dependent);
    }

    // --- Consultas ---------------------------------------------------------

    [[nodiscard]] bool hasDependency(const Dependency& dependency) const {
        const auto prerequisite = findVertex(dependency.prerequisiteId());
        const auto dependent = findVertex(dependency.dependentId());
        return prerequisite && dependent && graph_.hasEdge(*prerequisite, *dependent);
    }

    [[nodiscard]] std::size_t dependencyCount() const noexcept { return graph_.edgeCount(); }

private:
    [[nodiscard]] std::optional<graph::Graph::Vertex> findVertex(TaskId taskId) const {
        const auto found = vertexByTask_.find(taskId);
        if (found == vertexByTask_.end()) return std::nullopt;
        return found->second;
    }

    // Retorna o vértice da tarefa, criando-o no primeiro uso.
    graph::Graph::Vertex obtainVertex(TaskId taskId) {
        if (const auto existing = findVertex(taskId)) return *existing;

        const graph::Graph::Vertex created = graph_.addVertex();
        vertexByTask_.emplace(taskId, created);
        return created;
    }

    graph::Graph graph_;
    std::unordered_map<TaskId, graph::Graph::Vertex> vertexByTask_;
};

}  // namespace Graph::Services

#endif  //  SERVICES_DEPENDENCY_GRAPH_HPP
