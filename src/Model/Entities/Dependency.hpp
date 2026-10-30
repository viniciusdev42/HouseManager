#ifndef TASKMANAGER_MODEL_ENTITIES_DEPENDENCY_HPP
#define TASKMANAGER_MODEL_ENTITIES_DEPENDENCY_HPP

#include <cstddef>
#include <functional>
#include <string>

#include "Model/Entities/Task.hpp"

namespace Entities::Dependency {

/// Entidade que representa uma dependência entre duas tarefas.
///
/// Expressa a relação "a tarefa `dependentId` só pode ser concluída depois
/// que a tarefa `prerequisiteId` for concluída". Em termos de grafo, é a
/// aresta dirigida  prerequisiteId -> dependentId:
///  - prerequisiteId é o predecessor;
///  - dependentId é o sucessor.
///
/// A entidade referencia as tarefas apenas pelo TaskId, sem armazenar
/// ponteiros ou cópias de Task, mantendo-se compatível com a representação
/// por listas de adjacência e identificadores estáveis.
///
/// A identidade da entidade é definida pelo par (prerequisiteId, dependentId).
/// Esta classe não depende de View, Controller, serialização nem de
/// estruturas de dados.
///
/// Invariantes garantidas:
///  - ambos os IDs são diferentes de Task::kInvalidId;
///  - uma tarefa não pode depender de si mesma.
///
/// A entidade é imutável: para "alterar" uma dependência, remova a existente
/// e crie outra. A detecção de dependências circulares envolve várias arestas
/// e é responsabilidade da camada de serviços, em conjunto com o grafo.
///
/// Violações lançam std::invalid_argument.
class Dependency {
public:
    /// Cria uma dependência: `dependentId` depende de `prerequisiteId`.
    /// @throws std::invalid_argument se algum ID for inválido ou se
    ///         prerequisiteId == dependentId.
    Dependency(TaskId prerequisiteId, TaskId dependentId);

    /// Indica se o par forma uma dependência válida, sem lançar exceções.
    /// Útil para a camada de serviços validar a entrada antes de construir.
    static bool isValidPair(TaskId prerequisiteId, TaskId dependentId) noexcept;

    // --- Consultas ---------------------------------------------------------

    /// Tarefa que precisa ser concluída primeiro (predecessor).
    TaskId prerequisiteId() const noexcept { return prerequisiteId_; }

    /// Tarefa bloqueada até a conclusão do pré-requisito (sucessor).
    TaskId dependentId() const noexcept { return dependentId_; }

    /// Indica se a tarefa participa desta dependência, em qualquer dos lados.
    bool involves(TaskId taskId) const noexcept {
        return taskId == prerequisiteId_ || taskId == dependentId_;
    }

    /// Valor de hash que combina os dois IDs (a ordem do par importa).
    std::size_t hashValue() const noexcept;

    /// Representação textual para logs e depuração, ex.: "1 -> 2".
    std::string toString() const;

    // --- Identidade --------------------------------------------------------

    /// Duas dependências são iguais se possuem o mesmo par de IDs.
    friend bool operator==(const Dependency& lhs, const Dependency& rhs) noexcept {
        return lhs.prerequisiteId_ == rhs.prerequisiteId_ &&
               lhs.dependentId_ == rhs.dependentId_;
    }
    friend bool operator!=(const Dependency& lhs, const Dependency& rhs) noexcept {
        return !(lhs == rhs);
    }

    /// Ordem lexicográfica por (prerequisiteId, dependentId), útil para
    /// armazenar dependências em árvores de busca balanceadas.
    friend bool operator<(const Dependency& lhs, const Dependency& rhs) noexcept {
        if (lhs.prerequisiteId_ != rhs.prerequisiteId_) {
            return lhs.prerequisiteId_ < rhs.prerequisiteId_;
        }
        return lhs.dependentId_ < rhs.dependentId_;
    }

private:
    TaskId prerequisiteId_;
    TaskId dependentId_;
};

}  // namespace Entities::Dependency

#endif  // TASKMANAGER_MODEL_ENTITIES_DEPENDENCY_HPP
