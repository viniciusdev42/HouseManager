#ifndef TASK_MANAGER_SERIALIZER_HPP
#define TASK_MANAGER_SERIALIZER_HPP

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "Model/Entities/Task.hpp"

namespace serializer {

/// Sequência de bytes resultante (ou fonte) da serialização.
using Bytes = std::vector<std::uint8_t>;

/// Erro de serialização ou desserialização.
///
/// Todos os problemas de conteúdo (dados truncados, corrompidos, versão
/// desconhecida, tarefas inválidas, IDs duplicados) são reportados com este
/// tipo, nunca com comportamento indefinido.
class SerializationError : public std::runtime_error {
public:
    enum class Kind {
        Truncated,           ///< Os dados terminam antes do esperado.
        InvalidMagic,        ///< O arquivo não é um arquivo de tarefas.
        UnsupportedVersion,  ///< Versão do formato desconhecida.
        ChecksumMismatch,    ///< Os dados não conferem com o checksum (corrupção).
        InvalidData,         ///< Campo fora do domínio ou estrutura inconsistente.
        DuplicateId          ///< Duas tarefas com o mesmo ID.
    };

    SerializationError(Kind kind, const std::string& message);

    Kind kind() const noexcept { return kind_; }

private:
    Kind kind_;
};

/// Converte tarefas (model::Task) de/para uma representação binária versionada.
///
/// Esta classe trabalha apenas com bytes em memória: não conhece arquivos.
/// O acesso ao disco é responsabilidade de TaskFileStorage.
///
/// Formato (versão 1), todos os inteiros em little-endian:
///
///   Cabeçalho (28 bytes)
///     [ 0..3 ]  magic        "HMTK"
///     [ 4..5 ]  version      uint16  (kFormatVersion)
///     [ 6..7 ]  flags        uint16  (reservado, deve ser 0)
///     [ 8..15]  count        uint64  (quantidade de tarefas)
///     [16..23]  payloadSize  uint64  (bytes após o cabeçalho)
///     [24..27]  checksum     uint32  (CRC-32 de [0..23] + payload)
///
///   Payload: `count` registros consecutivos
///     id           uint64
///     priority     uint8
///     status       uint8
///     hasDeadline  uint8   (0 ou 1)
///     deadline     int64   (microssegundos desde a época; 0 se não houver)
///     titleLen     uint32, seguido de `titleLen` bytes
///     descLen      uint32, seguido de `descLen` bytes
///
/// Os dados desserializados são sempre revalidados pelas invariantes de
/// model::Task: um arquivo adulterado nunca produz uma tarefa inválida.
///
/// Limitação: prazos são armazenados com precisão de microssegundos.
class TaskSerializer {
public:
    static constexpr std::uint16_t kFormatVersion = 1;
    static constexpr std::size_t kHeaderSize = 28;

    TaskSerializer() = delete;

    /// Serializa a coleção completa (cabeçalho + registros + checksum).
    /// @throws SerializationError (Kind::DuplicateId) se houver IDs repetidos.
    static Bytes serialize(const std::vector<model::Task>& tasks);

    /// Reconstrói a coleção a partir de uma sequência produzida por serialize().
    /// @throws SerializationError se os dados estiverem truncados, corrompidos,
    ///         em versão não suportada ou contiverem tarefas inválidas/duplicadas.
    static std::vector<model::Task> deserialize(const Bytes& data);

    /// Acrescenta o registro de uma única tarefa ao final de `out`
    /// (sem cabeçalho). Útil para futuras camadas baseadas em páginas.
    /// Em caso de falha, `out` permanece inalterado.
    static void serializeTask(const model::Task& task, Bytes& out);

    /// Lê um único registro a partir de `offset` e avança `offset` para o byte
    /// seguinte ao registro. Em caso de falha, `offset` permanece inalterado.
    /// @throws SerializationError se o registro for truncado ou inválido.
    static model::Task deserializeTask(const Bytes& data, std::size_t& offset);
};

}  // namespace serializer

#endif  // TASK_MANAGER_SERIALIZER_HPP
