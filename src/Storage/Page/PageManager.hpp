#ifndef STORAGE_PAGE_MANAGER_HPP
#define STORAGE_PAGE_MANAGER_HPP

// PageManager.hpp
// Gerencia o ciclo de vida das páginas (alocar, recuperar, atualizar, liberar)
// sobre um PageFile. Mantém em memória apenas uma tabela de estados por página;
// o conteúdo é sempre lido e gravado diretamente no arquivo (write-through).

#include "Page.hpp"
#include "PageFile.hpp"

#include <cstddef>
#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace PageSystem  {

struct IntegrityReport {
    std::size_t pagesChecked = 0;
    std::vector<std::string> problems;

    bool ok() const noexcept { return problems.empty(); }
};

class PageManager {
public:
    // Assume a posse do arquivo e reconstrói a tabela de estados a partir dos cabeçalhos.
    explicit PageManager(PageFile file) : file_(std::move(file)) { loadMetadata(); }

    static PageManager create(const std::filesystem::path& path,
                              std::uint32_t pageSize = kDefaultPageSize,
                              bool overwrite = false) {
        return PageManager(PageFile::create(path, pageSize, overwrite));
    }

    static PageManager open(const std::filesystem::path& path) {
        return PageManager(PageFile::open(path));
    }

    // --- Tamanho e quantidade -------------------------------------------------

    std::uint32_t pageSize() const noexcept { return file_.pageSize(); }
    std::size_t pageCount() const noexcept { return states_.size(); }
    std::size_t freePageCount() const noexcept { return freeIds_.size(); }

    // --- Validação de identificadores -----------------------------------------

    bool isValidPageId(PageId id) const noexcept {
        return id != kInvalidPageId && static_cast<std::size_t>(id) < states_.size();
    }

    void validatePageId(PageId id) const {
        if (!isValidPageId(id)) {
            throw PageError(ErrorCode::InvalidPageId,
                            "identificador inválido: " + std::to_string(id) +
                                " (total de páginas: " + std::to_string(states_.size()) + ")");
        }
    }

    // --- Consulta de estado ---------------------------------------------------

    PageState stateOf(PageId id) const {
        validatePageId(id);
        return states_[id];
    }

    bool isFree(PageId id) const { return stateOf(id) == PageState::Free; }
    bool isInitialized(PageId id) const { return stateOf(id) == PageState::Initialized; }

    // Alocada, mas nunca escrita.
    bool isUninitialized(PageId id) const { return stateOf(id) == PageState::Allocated; }

    // Páginas disponíveis para reutilização, em ordem crescente de id.
    std::vector<PageId> freePages() const {
        return std::vector<PageId>(freeIds_.begin(), freeIds_.end());
    }

    std::vector<PageId> uninitializedPages() const {
        std::vector<PageId> result;
        for (std::size_t id = 0; id < states_.size(); ++id) {
            if (states_[id] == PageState::Allocated) {
                result.push_back(static_cast<PageId>(id));
            }
        }
        return result;
    }

    // --- Operações ------------------------------------------------------------

    // Reaproveita a página livre de menor id; se não houver, adiciona uma ao fim do arquivo.
    // A nova página nasce não inicializada.
    PageId allocatePage() {
        states_.reserve(states_.size() + 1);  // evita falha de alocação depois da escrita em disco

        if (!freeIds_.empty()) {
            const PageId id = *freeIds_.begin();
            file_.writePage(Page(id, pageSize(), PageState::Allocated));
            freeIds_.erase(freeIds_.begin());
            states_[id] = PageState::Allocated;
            return id;
        }

        const PageId id = file_.pageCount();
        file_.writePage(Page(id, pageSize(), PageState::Allocated));
        states_.push_back(PageState::Allocated);
        return id;
    }


    // Retorna uma cópia da página. Páginas livres não podem ser recuperadas.
    Page getPage(PageId id) {
        requireInUse(id, "recuperação");
        return file_.readPage(id);
    }

    // Substitui todo o conteúdo da página (o restante é zerado).
    void updatePage(PageId id, const std::vector<Byte>& content) {
        requireInUse(id, "atualização");
        if (content.size() > pageSize()) {
            throw PageError(ErrorCode::OutOfBounds,
                            "conteúdo de " + std::to_string(content.size()) +
                                " bytes excede o tamanho da página (" +
                                std::to_string(pageSize()) + ")");
        }
        // Como o conteúdo é totalmente substituído, a página não precisa ser lida antes;
        // isso também permite sobrescrever uma página com checksum corrompido.
        Page page(id, pageSize(), PageState::Allocated);
        page.assign(content);
        file_.writePage(page);
        states_[id] = PageState::Initialized;
    }

    // Atualiza um trecho da página, preservando o restante do conteúdo.
    void updatePage(PageId id, std::size_t offset, const Byte* source, std::size_t length) {
        requireInUse(id, "atualização");
        Page page = file_.readPage(id);
        page.write(offset, source, length);
        file_.writePage(page);
        states_[id] = page.state();
    }

    void updatePage(PageId id, std::size_t offset, const std::vector<Byte>& content) {
        updatePage(id, offset, content.data(), content.size());
    }

    // Libera a página, zerando seu conteúdo. Liberar duas vezes é uma operação inválida,
    // assim como liberar a página raiz: remova a raiz antes com setRootPageId(kInvalidPageId).
    void freePage(PageId id) {
        validatePageId(id);
        if (states_[id] == PageState::Free) {
            throw PageError(ErrorCode::InvalidOperation,
                            "página " + std::to_string(id) + " já está livre");
        }
        if (id == file_.rootPageId()) {
            throw PageError(ErrorCode::InvalidOperation,
                            "página " + std::to_string(id) +
                                " é a raiz; defina outra raiz ou remova-a antes de liberar");
        }
        file_.writePage(Page(id, pageSize(), PageState::Free));
        states_[id] = PageState::Free;
        freeIds_.insert(id);
    }

    // --- Página raiz ----------------------------------------------------------
    // A raiz é persistida no cabeçalho do arquivo e sobrevive ao fechamento da aplicação.

    // Retorna kInvalidPageId quando nenhuma raiz foi definida.
    PageId getRootPageId() const noexcept { return file_.rootPageId(); }

    bool hasRootPage() const noexcept { return file_.rootPageId() != kInvalidPageId; }

    // A raiz deve ser uma página em uso; kInvalidPageId remove a raiz.
    void setRootPageId(PageId id) {
        if (id != kInvalidPageId) {
            requireInUse(id, "definição de raiz");
        }
        file_.setRootPageId(id);
    }

    // --- Integridade ----------------------------------------------------------

    // Confere a estrutura do arquivo, o checksum de cada página e a consistência
    // entre a tabela de estados em memória e o que está gravado.
    IntegrityReport verifyIntegrity() {
        IntegrityReport report;

        try {
            file_.verifyStructure();
        } catch (const PageError& error) {
            report.problems.push_back(error.what());
        }

        if (states_.size() != file_.pageCount()) {
            report.problems.push_back("tabela de estados diverge do número de páginas do arquivo");
        }

        for (std::size_t i = 0; i < states_.size(); ++i) {
            const PageId id = static_cast<PageId>(i);
            ++report.pagesChecked;
            try {
                const Page page = file_.readPage(id);
                if (page.state() != states_[i]) {
                    report.problems.push_back("página " + std::to_string(id) +
                                              ": estado gravado difere do estado em memória");
                }
            } catch (const PageError& error) {
                report.problems.push_back("página " + std::to_string(id) + ": " + error.what());
            }
        }

        const PageId root = file_.rootPageId();
        if (root != kInvalidPageId) {
            if (!isValidPageId(root)) {
                report.problems.push_back("página raiz " + std::to_string(root) + " não existe");
            } else if (states_[root] == PageState::Free) {
                report.problems.push_back("página raiz " + std::to_string(root) + " está livre");
            }
        }

        std::size_t freeInTable = 0;
        for (PageState state : states_) {
            if (state == PageState::Free) {
                ++freeInTable;
            }
        }
        if (freeInTable != freeIds_.size()) {
            report.problems.push_back("lista de páginas livres inconsistente com a tabela de estados");
        }
        for (PageId id : freeIds_) {
            if (!isValidPageId(id) || states_[id] != PageState::Free) {
                report.problems.push_back("página " + std::to_string(id) +
                                          " consta como livre mas não está");
            }
        }
        return report;
    }

    // --- Persistência ---------------------------------------------------------

    void flush() { file_.flush(); }
    void close() { file_.close(); }

private:
    void loadMetadata() {
        const PageId total = file_.pageCount();
        states_.reserve(total);
        for (PageId id = 0; id < total; ++id) {
            const PageHeader header = file_.readPageHeader(id);
            states_.push_back(header.state);
            if (header.state == PageState::Free) {
                freeIds_.insert(id);
            }
        }
    }

    void requireInUse(PageId id, const char* operation) const {
        validatePageId(id);
        if (states_[id] == PageState::Free) {
            throw PageError(ErrorCode::InvalidOperation,
                            std::string("operação de ") + operation + " em página livre (id " +
                                std::to_string(id) + ")");
        }
    }

    PageFile file_;
    std::vector<PageState> states_;
    std::set<PageId> freeIds_;
};

}  // namespace PageSystem 

#endif