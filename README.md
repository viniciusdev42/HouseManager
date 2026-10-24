# HouseManager

## Visão Geral

### Descrição

O House Manager é um gerenciador de tarefas domésticas desenvolvido em C++ que serve como aplicação e laboratório de estruturas de dados. Nele,cada estrutura de dados existe para resolver uma necessidade concreta do sistema. Cada estrutura possuí a sua própria implementação,testes unitários e testes de carga.

Por fora,o House Manager é um gerenciador de tarefas: o usuário cria tarefas com título,descrição,prioridade e prazo e estado(concluída,em andamento,em espera,cancelada). Por dentro,o House Manager é um projeto de engenharia de software e de algoritmos: todo o armazenamento,indexação,busca e persistência são implementados do zero,sem delegar às estruturas de dados e bibliotecas padrão.

O House Manager utiliza as seguintes estruturas de dados:

1. `Hash Table`: Permite o **acesso direto** à tarefa pelo ID
2. `Fila de Prioridade (Heap)`: Permite o **Agendamento de Tarefas**
3. `Árvore AVL`: Permite **índices ordenados** (título,prazo,prioridade)
4. `Árvore TRIE`: Permite a busca por **prefixo** e **autocomplete**
5. `Processamento de Cadeia de Caracteres`: Permite a **normalização,a tokenização e a busca textual**.
6. `Árvore B`: Permite a **persistência** e a **indexação em disco**
7. `Grafos`: Permite gerenciar a **dependência entre tarefas**,a **detecção de ciclos** e a **ordenação topológica**
8. `Busca Digital`: Permite a busca baseada em **dígitos** (Digital Search Tree)

### Arquitetura

O House Manager utiliza o padrão arquitetural **MVC (Model-View-Controller)**. Ele separa a representação dos dados e as regras da aplicação,a interface apresentada ao usuário e o tratamento das interações. Essa divisão busca reduzir o acoplamento, facilitar os testes e permitir que cada parte evolua sem exigir alterações desnecessárias nas demais.

O House Manager segue os seguintes princípios:

1. **Separação de Responsabilidades**: cada componente possui um propósito claro

2. **Baixo Acoplamento**: alterações na GUI não devem exigir mudanças nas estruturas de dados

3. **Alta Coesão**: funcionalidades relacionadas ficam agrupadas em componentes especializados

4.  **Encapsulamento:** detalhes internos de algoritmos e armazenamento não são expostos à interface

5.  **Independência da GUI:** o núcleo pode ser compilado e testado sem iniciar a interface

6.  **Testabilidade:** as camadas e estruturas devem poder ser testadas isoladamente

7.  **Evolução Incremental:** novas funcionalidades devem ser integradas sem concentração de responsabilidades

#### Model

O Model representa os dados do domínio, aplica as regras de negócio e coordena o acesso aos dados necessários ao funcionamento do sistema. A suas responsabilidade são:

1. Representar as entidades do domínio,como `Task` e `Dependency`

2. Aplicar e validar regras de negócio relacionadas às tarefas e às suas dependências

3. Coordenar o acesso aos registros e aos índices utilizados pela aplicação

4. Integrar as estruturas de dados às funcionalidades que necessitam delas

5. Gerenciar a persistência e a recuperação dos dados por meio dos componentes de armazenamento

6. Expor operações e resultados sem depender de elementos da interface gráfica

Task e Dependency são as entidades do domínio,representando os principais conceitos do sistema. 

Task representa uma tarefa,possuindo identificador,título,descrição,prioridade,prazo e estado.

Dependency representa uma relação de dependência entre tarefas.

#### View

A View é responsável pela apresentação das informações e pela captura visual das interações do usuário. As suas responsabilidades são: 

1. Exibir tarefas, projetos, prioridades, prazos e estados

2. Apresentar formulários de criação e edição

3. Exibir resultados de pesquisa e mensagens compreensíveis

4. Apresentar dependências entre tarefas de forma visual

5. Encaminhar as ações do usuário aos Controllers correspondentes

6. Atualizar a apresentação a partir dos resultados fornecidos pela aplicação

A View não executa algoritmos de estruturas de dados,manipula diretamente os arquivos de persistência,e nem implementa regras de negócio.

#### Controller

O Controller recebe as ações originadas na View e coordena a execução das operações correspondentes no Model. As suas responsabilidades são:

1.  Receber comandos da interface,como criar,editar,concluir ou remover uma tarefa

2.  Verificar a validade dos dados de entrada no contexto da operação

3.  Invocar as operações apropriadas do Model

4.  Tratar resultados e erros da operação

5.  Fornecer à View os dados necessários para atualizar a apresentação

O Controller não implementa as operações internas da AVL,TRIE,Hash Table ou qualquer outra estrutura. O seu papel é coordenar o fluxo da interação,não substituir os componentes especializados.

### Framework

O framework gráfico escolhido foi o **Qt6/Qt Widgets** devido a escolha de arquitetura e linguagem. O Qt permite desenvolver a interface gráfica sem a necessidade de introduzir outra linguagem principal no projeto. Além disso,os slots do Qt facilitam a comunicação entre a interface e o Controller,sem exigir que as estruturas de dados dependam da GUI. Por fim,o Qt é fácil de ser integrado com o **CMake**,o que facilita a organização e compilação modular do projeto.

### Estrutura

``` Text
TaskManager/
├── src/
│   ├── Model/
│   │   ├── Entities
│   │   ├── Services
│   │   ├── Repositories
│   │   ├── Structures/
│   │   │   ├── Hash Table/
│   │   │   ├── Tree/
│   │   │   ├── TRIE/
│   │   │   ├── Graph/
│   │   │   ├── Queue/
│   │   │   ├── Strings/
│   │   │   └── DS/
│   │   │ 
│   ├── View/
│   │   ├── Windows/
│   │   ├── Widgets/
│   │   └── Dialogs/
│   ├── Controller/
├── Tests/
│   ├── Model/
│   ├── Controller/
│   ├── View/
│   └── Integration/

```
