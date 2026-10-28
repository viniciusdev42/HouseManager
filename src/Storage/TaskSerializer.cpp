#include "Storage/TaskSerializer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace serializer {

SerializationError::SerializationError(Kind kind, const std::string& message)
    : std::runtime_error(message), kind_(kind) {}

namespace {

using model::Priority;
using model::Status;
using model::Task;
using model::TaskId;
using model::TimePoint;

// --- Constantes do formato ---------------------------------------------------

constexpr std::array<std::uint8_t, 4> kMagic = {'H', 'M', 'T', 'K'};

// Posições dos campos no cabeçalho.
constexpr std::size_t kVersionOffset = 4;
constexpr std::size_t kFlagsOffset = 6;
constexpr std::size_t kCountOffset = 8;
constexpr std::size_t kPayloadSizeOffset = 16;
constexpr std::size_t kChecksumOffset = 24;  // O CRC cobre os bytes [0, 24) + payload.

// id(8) + priority(1) + status(1) + hasDeadline(1) + deadline(8) + 2 x len(4).
constexpr std::size_t kMinRecordSize = 27;

static_assert(TaskSerializer::kHeaderSize == kChecksumOffset + 4,
              "O tamanho do cabecalho deve terminar logo apos o checksum.");

// --- CRC-32 (IEEE 802.3, polinômio refletido 0xEDB88320) ---------------------

constexpr std::array<std::uint32_t, 256> makeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kCrcTable = makeCrcTable();

std::uint32_t crcUpdate(std::uint32_t state, const std::uint8_t* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        state = kCrcTable[(state ^ data[i]) & 0xFFu] ^ (state >> 8);
    }
    return state;
}

/// CRC-32 do cabeçalho (sem o campo de checksum) somado ao payload.
std::uint32_t computeChecksum(const Bytes& data, std::size_t payloadSize) {
    std::uint32_t state = 0xFFFFFFFFu;
    state = crcUpdate(state, data.data(), kChecksumOffset);
    state = crcUpdate(state, data.data() + TaskSerializer::kHeaderSize, payloadSize);
    return state ^ 0xFFFFFFFFu;
}

// --- Escrita / leitura de inteiros little-endian -----------------------------

void putLE(Bytes& out, std::uint64_t value, unsigned byteCount) {
    for (unsigned i = 0; i < byteCount; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu));
    }
}

void patchLE(Bytes& out, std::size_t offset, std::uint64_t value, unsigned byteCount) {
    for (unsigned i = 0; i < byteCount; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (8u * i)) & 0xFFu);
    }
}

std::uint64_t loadLE(const std::uint8_t* bytes, unsigned byteCount) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < byteCount; ++i) {
        value |= static_cast<std::uint64_t>(bytes[i]) << (8u * i);
    }
    return value;
}

void putString(Bytes& out, const std::string& text) {
    if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw SerializationError(SerializationError::Kind::InvalidData,
                                 "Texto grande demais para ser serializado.");
    }
    putLE(out, text.size(), 4);
    out.insert(out.end(), text.begin(), text.end());
}

// --- Conversão de prazos -----------------------------------------------------

std::int64_t toMicroseconds(TimePoint timePoint) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               timePoint.time_since_epoch())
        .count();
}

TimePoint fromMicroseconds(std::int64_t microseconds) {
    using Duration = TimePoint::duration;
    const auto maxUs =
        std::chrono::duration_cast<std::chrono::microseconds>(Duration::max()).count();
    const auto minUs =
        std::chrono::duration_cast<std::chrono::microseconds>(Duration::min()).count();
    if (microseconds < minUs || microseconds > maxUs) {
        throw SerializationError(SerializationError::Kind::InvalidData,
                                 "Prazo fora do intervalo representavel.");
    }
    return TimePoint(std::chrono::duration_cast<Duration>(
        std::chrono::microseconds(microseconds)));
}

// --- Leitor com verificação de limites ---------------------------------------

/// Lê campos sequencialmente de [position, end), lançando SerializationError
/// (Truncated) em vez de ultrapassar o fim dos dados.
class Reader {
public:
    Reader(const Bytes& data, std::size_t& position, std::size_t end)
        : data_(data), position_(position), end_(end) {}

    std::uint8_t u8() { return static_cast<std::uint8_t>(readLE(1)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(readLE(4)); }
    std::uint64_t u64() { return readLE(8); }
    std::int64_t i64() { return static_cast<std::int64_t>(readLE(8)); }

    /// Lê uma string com prefixo de tamanho, rejeitando tamanhos acima de
    /// `maxLength` antes de alocar memória.
    std::string text(std::size_t maxLength, const char* fieldName) {
        const std::uint32_t length = u32();
        if (length > maxLength) {
            throw SerializationError(
                SerializationError::Kind::InvalidData,
                std::string("O campo '") + fieldName +
                    "' excede o tamanho maximo permitido.");
        }
        require(length);
        std::string value(reinterpret_cast<const char*>(data_.data() + position_), length);
        position_ += length;
        return value;
    }

private:
    void require(std::size_t byteCount) const {
        if (byteCount > end_ - position_) {
            throw SerializationError(SerializationError::Kind::Truncated,
                                     "Dados truncados: fim inesperado do arquivo.");
        }
    }

    std::uint64_t readLE(unsigned byteCount) {
        require(byteCount);
        const std::uint64_t value = loadLE(data_.data() + position_, byteCount);
        position_ += byteCount;
        return value;
    }

    const Bytes& data_;
    std::size_t& position_;
    std::size_t end_;
};

/// Lê um registro e o valida pelas invariantes do domínio.
Task readTask(Reader& reader) {
    const TaskId id = reader.u64();
    const std::uint8_t rawPriority = reader.u8();
    const std::uint8_t rawStatus = reader.u8();
    const std::uint8_t rawHasDeadline = reader.u8();
    const std::int64_t rawDeadline = reader.i64();
    std::string title = reader.text(Task::kMaxTitleLength, "titulo");
    std::string description = reader.text(Task::kMaxDescriptionLength, "descricao");

    if (rawPriority > static_cast<std::uint8_t>(Priority::Critical)) {
        throw SerializationError(SerializationError::Kind::InvalidData,
                                 "Prioridade invalida no arquivo.");
    }
    if (rawStatus > static_cast<std::uint8_t>(Status::Completed)) {
        throw SerializationError(SerializationError::Kind::InvalidData,
                                 "Estado invalido no arquivo.");
    }
    if (rawHasDeadline > 1 || (rawHasDeadline == 0 && rawDeadline != 0)) {
        throw SerializationError(SerializationError::Kind::InvalidData,
                                 "Indicador de prazo invalido no arquivo.");
    }

    std::optional<TimePoint> deadline;
    if (rawHasDeadline == 1) {
        deadline = fromMicroseconds(rawDeadline);
    }

    try {
        return Task(id, std::move(title), std::move(description),
                    static_cast<Priority>(rawPriority), deadline,
                    static_cast<Status>(rawStatus));
    } catch (const std::invalid_argument& e) {
        throw SerializationError(SerializationError::Kind::InvalidData,
                                 std::string("Tarefa invalida no arquivo: ") + e.what());
    }
}

}  // namespace

// --- API pública ---------------------------------------------------------------

void TaskSerializer::serializeTask(const Task& task, Bytes& out) {
    const std::size_t originalSize = out.size();
    try {
        putLE(out, task.id(), 8);
        putLE(out, static_cast<std::uint8_t>(task.priority()), 1);
        putLE(out, static_cast<std::uint8_t>(task.status()), 1);
        putLE(out, task.hasDeadline() ? 1 : 0, 1);
        const std::int64_t deadlineUs =
            task.hasDeadline() ? toMicroseconds(*task.deadline()) : 0;
        putLE(out, static_cast<std::uint64_t>(deadlineUs), 8);
        putString(out, task.title());
        putString(out, task.description());
    } catch (...) {
        out.resize(originalSize);
        throw;
    }
}

Task TaskSerializer::deserializeTask(const Bytes& data, std::size_t& offset) {
    if (offset > data.size()) {
        throw SerializationError(SerializationError::Kind::Truncated,
                                 "Deslocamento alem do fim dos dados.");
    }
    std::size_t position = offset;
    Reader reader(data, position, data.size());
    Task task = readTask(reader);
    offset = position;  // Só avança em caso de sucesso.
    return task;
}

Bytes TaskSerializer::serialize(const std::vector<Task>& tasks) {
    std::unordered_set<TaskId> ids;
    ids.reserve(tasks.size());
    for (const Task& task : tasks) {
        if (!ids.insert(task.id()).second) {
            throw SerializationError(
                SerializationError::Kind::DuplicateId,
                "ID de tarefa duplicado: " + std::to_string(task.id()));
        }
    }

    Bytes out;
    out.reserve(kHeaderSize + tasks.size() * 128);
    out.resize(kHeaderSize, 0);  // Cabeçalho preenchido ao final.

    for (const Task& task : tasks) {
        serializeTask(task, out);
    }

    const std::uint64_t payloadSize = out.size() - kHeaderSize;

    std::copy(kMagic.begin(), kMagic.end(), out.begin());
    patchLE(out, kVersionOffset, kFormatVersion, 2);
    patchLE(out, kFlagsOffset, 0, 2);
    patchLE(out, kCountOffset, tasks.size(), 8);
    patchLE(out, kPayloadSizeOffset, payloadSize, 8);
    patchLE(out, kChecksumOffset, computeChecksum(out, static_cast<std::size_t>(payloadSize)), 4);

    return out;
}

std::vector<Task> TaskSerializer::deserialize(const Bytes& data) {
    if (data.size() < kHeaderSize) {
        throw SerializationError(SerializationError::Kind::Truncated,
                                 "Arquivo menor que o cabecalho esperado.");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), data.begin())) {
        throw SerializationError(SerializationError::Kind::InvalidMagic,
                                 "O arquivo nao e um arquivo de tarefas valido.");
    }

    const std::uint64_t version = loadLE(data.data() + kVersionOffset, 2);
    if (version != kFormatVersion) {
        throw SerializationError(
            SerializationError::Kind::UnsupportedVersion,
            "Versao do formato nao suportada: " + std::to_string(version) + ".");
    }
    if (loadLE(data.data() + kFlagsOffset, 2) != 0) {
        throw SerializationError(SerializationError::Kind::InvalidData,
                                 "Flags reservadas com valor inesperado.");
    }

    const std::uint64_t count = loadLE(data.data() + kCountOffset, 8);
    const std::uint64_t payloadSize = loadLE(data.data() + kPayloadSizeOffset, 8);
    const std::uint64_t available = data.size() - kHeaderSize;

    if (payloadSize > available) {
        throw SerializationError(SerializationError::Kind::Truncated,
                                 "Dados truncados: o conteudo e menor que o declarado.");
    }
    if (payloadSize < available) {
        throw SerializationError(SerializationError::Kind::InvalidData,
                                 "Existem bytes excedentes apos o conteudo declarado.");
    }

    const auto storedChecksum =
        static_cast<std::uint32_t>(loadLE(data.data() + kChecksumOffset, 4));
    if (storedChecksum != computeChecksum(data, static_cast<std::size_t>(payloadSize))) {
        throw SerializationError(SerializationError::Kind::ChecksumMismatch,
                                 "Checksum invalido: os dados estao corrompidos.");
    }

    // Impede reservas de memória absurdas caso `count` esteja adulterado.
    if (count > payloadSize / kMinRecordSize) {
        throw SerializationError(SerializationError::Kind::InvalidData,
                                 "Quantidade de tarefas incompativel com o tamanho dos dados.");
    }

    std::size_t position = kHeaderSize;
    Reader reader(data, position, data.size());

    std::vector<Task> tasks;
    tasks.reserve(static_cast<std::size_t>(count));
    std::unordered_set<TaskId> ids;
    ids.reserve(static_cast<std::size_t>(count));

    for (std::uint64_t i = 0; i < count; ++i) {
        Task task = readTask(reader);
        if (!ids.insert(task.id()).second) {
            throw SerializationError(
                SerializationError::Kind::DuplicateId,
                "ID de tarefa duplicado no arquivo: " + std::to_string(task.id()));
        }
        tasks.push_back(std::move(task));
    }

    if (position != data.size()) {
        throw SerializationError(SerializationError::Kind::InvalidData,
                                 "Bytes nao utilizados apos o ultimo registro.");
    }

    return tasks;
}

}  // namespace serializer
