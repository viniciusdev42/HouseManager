/*
  * Descricao
  * Lista de Prioridades (Heap Binario) em C++
  * com funcoes de insercao,consulta por prioridade
  * remocao por prioridade
  * atualizacao por prioridade
  * remocao por id
  * Decisoes de projeto
  * Mapa id → posicao: sem ele, atualizar ou remover um elemento exigiria busca linear O(n). 
  * Por isso cada Element tem um id único, e ids duplicados lançam std::invalid_argument.
  * Reequilíbrio após mudança: depois de atualizar ou remover, o nó só pode violar o heap em uma direção. 
  * Por isso o código tenta siftUp e só chama siftDown se o nó não subiu.
  * Erros: peek() e extractTop() em lista vazia lançam std::out_of_range. Um id inexistente lança std::invalid_argument.
  * Debito Tecnico
  * Tipo Element fixo (id:int, name:string, priority:int); não é template.
  * Apenas max-heap; sem comparador configurável (min-heap).
  * Sem desempate determinístico para prioridades iguais.
*/

#ifndef STRUCTURES_PRIORITY_LIST_HPP
#define STRUCTURES_PRIORITY_LIST_HPP

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace Structures::PriorityList {

/**
 * Elemento armazenado na lista de prioridades.
 * O `id` identifica o elemento de forma única e permite localizá-lo em O(1)
 * para atualização de prioridade e remoção.
 */
struct Element {
    int id;
    std::string name;
    int priority;
};

/**
 * Lista de Prioridades implementada sobre um Heap Binário (max-heap):
 * o elemento com MAIOR valor de `priority` está sempre no topo.
 *
 * Complexidades:
 *   - construção a partir de coleção ... O(n)
 *   - insert ........................... O(log n)
 *   - peek ............................. O(1)
 *   - extractTop ....................... O(log n)
 *   - updatePriority ................... O(log n)
 *   - remove ........................... O(log n)
 *
 * Erros:
 *   - std::invalid_argument: id duplicado ou inexistente.
 *   - std::out_of_range: consulta/extração em lista vazia.
 */
class PriorityList {
public:
    PriorityList() = default;

    /** Constrói o heap a partir de uma coleção (heapify bottom-up, O(n)). */
    explicit PriorityList(std::vector<Element> elements);

    /** Insere um novo elemento. Lança std::invalid_argument se o id já existir. */
    void insert(const Element& element);

    /** Consulta o elemento de maior prioridade sem removê-lo. */
    const Element& peek() const;

    /** Remove e retorna o elemento de maior prioridade. */
    Element extractTop();

    /** Altera a prioridade do elemento `id` e reorganiza o heap. */
    void updatePriority(int id, int newPriority);

    /** Remove o elemento `id` (em qualquer posição) e o retorna. */
    Element remove(int id);

    bool contains(int id) const;
    std::size_t size() const noexcept;
    bool empty() const noexcept;

private:
    std::vector<Element> heap_;
    // Mapa id -> posição no vetor; evita busca linear O(n) em update/remove.
    std::unordered_map<int, std::size_t> positionById_;

    static std::size_t parentOf(std::size_t index) noexcept;
    static std::size_t leftChildOf(std::size_t index) noexcept;

    bool hasHigherPriority(std::size_t a, std::size_t b) const;
    void swapNodes(std::size_t a, std::size_t b);

    std::size_t siftUp(std::size_t index);
    void siftDown(std::size_t index);
    void restoreHeapProperty(std::size_t index);

    std::size_t findPosition(int id) const;
    Element removeAt(std::size_t index);
};

} // Namespace Structures::PriorityList

#endif // PRIORITY_LIST_HPP
