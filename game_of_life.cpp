/**
 * ============================================================================
 *  Jogo da Vida de Conway — Paralelismo com Múltiplos Processos e Ghost Cells
 * ============================================================================
 *
 *  VISÃO GERAL DO PROJETO
 *  ─────────────────────
 *  Simulamos o Jogo da Vida de Conway de forma PARALELA, dividindo o tabuleiro
 *  entre vários "processos" (implementados como threads). Cada processo cuida
 *  de uma faixa horizontal do tabuleiro.
 *
 *  O problema de usar bordas: para calcular o próximo estado de uma célula na
 *  borda de uma faixa, precisamos saber o estado das células da faixa vizinha.
 *  A solução clássica em computação paralela (e em MPI) são as GHOST CELLS:
 *  cópias das linhas de borda dos vizinhos, mantidas localmente.
 *
 *  Layout da memória local de cada processo:
 *
 *    ┌──────────────────────┐
 *    │  GHOST linhas (top)  │  ← cópia das últimas GHOST linhas do processo anterior
 *    ├──────────────────────┤
 *    │                      │
 *    │   Região REAL        │  ← linhas que este processo realmente calcula
 *    │                      │
 *    ├──────────────────────┤
 *    │  GHOST linhas (bot)  │  ← cópia das primeiras GHOST linhas do próximo processo
 *    └──────────────────────┘
 *
 *  REGRAS DO JOGO DA VIDA (B3/S23)
 *  ─────────────────────────────────
 *   • Célula viva com < 2 vizinhos vivos → morre (solidão)
 *   • Célula viva com 2 ou 3 vizinhos    → sobrevive
 *   • Célula viva com > 3 vizinhos       → morre (superpopulação)
 *   • Célula morta com exatamente 3      → nasce (reprodução)
 *
 *  COMPILAÇÃO (requer C++20 pelo uso de std::barrier)
 *  ──────────
 *    g++ -O2 -std=c++20 -pthread -o game_of_life game_of_life.cpp
 *
 *  USO
 *  ───
 *    ./game_of_life [opções]
 *
 *    --pattern  <nome>    glider | blinker | pulsar | gosper | random | rle
 *    --rle      <str>     string RLE inline  (requer --pattern rle)
 *    --file     <path>    arquivo .rle ou .cells
 *    --rows     <n>       linhas do tabuleiro        (padrão: 64)
 *    --cols     <n>       colunas do tabuleiro       (padrão: 128)
 *    --gens     <n>       gerações                   (padrão: 200)
 *    --delay    <ms>      pausa entre frames         (padrão: 80)
 *    --ghost    <n>       ghost cells por lado       (padrão: 5)
 *    --procs    <n>       número de processos        (padrão: 16)
 *    --no-display         modo benchmark (sem saída visual)
 *    --seed     <n>       semente do random          (padrão: 42)
 * ============================================================================
 */

// Includes padrão do C++
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <thread>
#include <barrier>       // C++20 — barreira de sincronização entre threads
#include <chrono>
#include <mutex>
#include <iomanip>
#include <cassert>
#include <cctype>
#include <random>

// ============================================================================
// Constantes padrão — valores usados quando o usuário não passa argumentos
// ============================================================================
static constexpr int DEF_ROWS    = 64;    // linhas do tabuleiro
static constexpr int DEF_COLS    = 128;   // colunas do tabuleiro
static constexpr int DEF_GENS    = 200;   // número de gerações a simular
static constexpr int DEF_DELAY   = 80;    // ms entre frames na animação
static constexpr int DEF_GHOST   = 5;     // ghost cells por lado (quantas linhas de borda)
static constexpr int DEF_PROCS   = 16;    // número de processos paralelos (threads)

// ============================================================================
// Tipos
// ============================================================================
// Board é um vetor 1D que representa o tabuleiro 2D de forma plana.
// Para acessar a célula na linha r, coluna c: board[r * COLS + c]
// Usamos uint8_t (1 byte) para cada célula: 0 = morta, 1 = viva.
using Board = std::vector<uint8_t>;

// ============================================================================
// Argumentos de linha de comando
// ============================================================================
struct Args {
    std::string pattern  = "glider";  // padrão inicial a usar
    std::string rle_str;              // string RLE passada inline
    std::string file_path;            // caminho para arquivo de padrão
    int  rows    = DEF_ROWS;
    int  cols    = DEF_COLS;
    int  gens    = DEF_GENS;
    int  delay   = DEF_DELAY;
    int  ghost   = DEF_GHOST;
    int  procs   = DEF_PROCS;
    bool display = true;              // false = modo benchmark (sem animação)
    unsigned seed = 42;               // semente para padrão aleatório
};

// parse_args: lê os argumentos da linha de comando e preenche a struct Args.
// Exemplo: ./game_of_life --pattern random --procs 8 --gens 100
static Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        // Lambda que avança o índice e retorna o próximo argumento
        auto next = [&]{ return (i+1 < argc) ? argv[++i] : ""; };
        if      (s=="--pattern") a.pattern   = next();
        else if (s=="--rle")     a.rle_str   = next();
        else if (s=="--file")    a.file_path = next();
        else if (s=="--rows")    a.rows      = std::atoi(next());
        else if (s=="--cols")    a.cols      = std::atoi(next());
        else if (s=="--gens")    a.gens      = std::atoi(next());
        else if (s=="--delay")   a.delay     = std::atoi(next());
        else if (s=="--ghost")   a.ghost     = std::atoi(next());
        else if (s=="--procs")   a.procs     = std::atoi(next());
        else if (s=="--seed")    a.seed      = (unsigned)std::atoi(next());
        else if (s=="--no-display") a.display = false;
    }
    return a;
}

// ============================================================================
// Funções de inicialização de padrões
// Cada função recebe o tabuleiro (Board) e o preenche com um padrão clássico.
// ============================================================================

// Glider: estrutura de 5 células que "anda" pelo tabuleiro diagonalmente.
// É o padrão mais simples que se move indefinidamente.
static void pat_glider(Board& g, int R, int C) {
    auto set = [&](int r, int c){ if(r>=0&&r<R&&c>=0&&c<C) g[r*C+c]=1; };
    set(1,2); set(2,3); set(3,1); set(3,2); set(3,3);
}

// Blinker: oscilador de período 2 — alterna entre horizontal e vertical.
static void pat_blinker(Board& g, int R, int C) {
    int r=R/2, c=C/2;
    g[r*C+(c-1)]=1; g[r*C+c]=1; g[r*C+(c+1)]=1;
}

// Pulsar: oscilador de período 3, um dos mais reconhecidos no Jogo da Vida.
static void pat_pulsar(Board& g, int R, int C) {
    int r0=R/2-6, c0=C/2-6;
    int pts[][2]={
        {0,2},{0,3},{0,4},{0,8},{0,9},{0,10},
        {2,0},{3,0},{4,0},{2,5},{3,5},{4,5},
        {2,7},{3,7},{4,7},{2,12},{3,12},{4,12},
        {5,2},{5,3},{5,4},{5,8},{5,9},{5,10},
        {7,2},{7,3},{7,4},{7,8},{7,9},{7,10},
        {8,0},{9,0},{10,0},{8,5},{9,5},{10,5},
        {8,7},{9,7},{10,7},{8,12},{9,12},{10,12},
        {12,2},{12,3},{12,4},{12,8},{12,9},{12,10}
    };
    for(auto& p:pts){
        int r=r0+p[0], c=c0+p[1];
        if(r>=0&&r<R&&c>=0&&c<C) g[r*C+c]=1;
    }
}

// Gosper Glider Gun: estrutura famosa que gera gliders indefinidamente.
// Criada por Bill Gosper em 1970, foi a primeira estrutura de crescimento infinito descoberta.
static void pat_gosper(Board& g, int R, int C) {
    int r0=2, c0=2;
    int pts[][2]={
        {0,24},
        {1,22},{1,24},
        {2,12},{2,13},{2,20},{2,21},{2,34},{2,35},
        {3,11},{3,15},{3,20},{3,21},{3,34},{3,35},
        {4,0},{4,1},{4,10},{4,16},{4,20},{4,21},
        {5,0},{5,1},{5,10},{5,14},{5,16},{5,17},{5,22},{5,24},
        {6,10},{6,16},{6,24},
        {7,11},{7,15},
        {8,12},{8,13}
    };
    for(auto& p:pts){
        int r=r0+p[0], c=c0+p[1];
        if(r>=0&&r<R&&c>=0&&c<C) g[r*C+c]=1;
    }
}

// Padrão aleatório: cada célula tem ~1/3 de chance de nascer viva.
// Usa Mersenne Twister (mt19937) para reprodutibilidade com a mesma semente.
static void pat_random(Board& g, int R, int C, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_int_distribution<int> dist(0, 2);
    for(int i=0;i<R*C;++i) g[i] = (dist(rng)==0) ? 1 : 0;
}

// ============================================================================
// Parser RLE (Run-Length Encoding)
// ============================================================================
// RLE é o formato padrão para compartilhar padrões do Jogo da Vida.
// Exemplo de RLE para um Glider:
//   x = 3, y = 3, rule = B3/S23
//   bob$2bo$3o!
//
// Onde: 'b' = célula morta, 'o' = célula viva, '$' = nova linha, '!' = fim
// Um número antes de uma letra repete aquela letra: "3o" = "ooo"
static void apply_rle(Board& g, int R, int C, const std::string& src) {
    std::istringstream ss(src);
    std::string line, data;
    int pw=0, ph=0;
    // Lê o cabeçalho para obter as dimensões do padrão
    while(std::getline(ss, line)) {
        if(line.empty()||line[0]=='#') continue;  // pula comentários
        if(line[0]=='x'||line[0]=='X') {
            sscanf(line.c_str(),"x = %d, y = %d",&pw,&ph);
            continue;
        }
        data += line;  // concatena as linhas de dados RLE
    }
    // Centraliza o padrão no tabuleiro
    int or0 = (ph>0) ? (R-ph)/2 : R/4;
    int oc0 = (pw>0) ? (C-pw)/2 : C/4;
    // Decodifica o RLE: percorre cada caractere
    int row=or0, col=oc0, cnt=0;
    for(char ch:data){
        if(ch=='!') break;                         // fim do padrão
        if(std::isdigit(ch)){ cnt=cnt*10+(ch-'0'); continue; }  // acumula repetições
        int rep=(cnt==0)?1:cnt; cnt=0;
        if(ch=='$'){ row+=rep; col=oc0; }          // avança 'rep' linhas
        else if(ch=='b'){ col+=rep; }              // pula 'rep' células mortas
        else if(ch=='o'){
            for(int i=0;i<rep;++i){
                if(row>=0&&row<R&&col>=0&&col<C) g[row*C+col]=1;
                ++col;
            }
        }
    }
}

// Parser para formato .cells (plaintext)
// Formato mais simples: 'O' ou '*' = viva, '.' = morta, '!' = comentário
static void apply_cells(Board& g, int R, int C, const std::string& src) {
    std::istringstream ss(src);
    std::string line;
    std::vector<std::string> lines;
    while(std::getline(ss,line))
        if(!line.empty()&&line[0]!='!') lines.push_back(line);
    int maxw=0;
    for(auto& l:lines) maxw=std::max(maxw,(int)l.size());
    int or0=(R-(int)lines.size())/2;  // centraliza verticalmente
    int oc0=(C-maxw)/2;               // centraliza horizontalmente
    for(int r=0;r<(int)lines.size();++r)
        for(int c=0;c<(int)lines[r].size();++c){
            int rr=or0+r, cc=oc0+c;
            if(rr>=0&&rr<R&&cc>=0&&cc<C)
                g[rr*C+cc]=(lines[r][c]=='O'||lines[r][c]=='*')?1:0;
        }
}

// ============================================================================
// Renderização no terminal
// ============================================================================
// Usa códigos de escape ANSI para:
//   - Mover o cursor para o topo da tela (\033[H)
//   - Limpar a tela (\033[2J)
//   - Colorir o texto (ex: \033[1;32m = verde brilhante)
// Isso cria a animação sem abrir janela gráfica.
static void render(const Board& g, int R, int C, int gen, int pop,
                   double avg_ms, int procs, int ghost) {
    std::cout << "\033[H\033[2J\033[?25l";  // posiciona cursor, limpa tela, esconde cursor

    // Cabeçalho colorido
    std::cout << "\033[1;36m";  // ciano brilhante
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║      🌱 Jogo da Vida de Conway — Paralelismo com Processos       ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";
    std::cout << "\033[0m";

    // Linha de métricas: geração atual, população, número de processos, etc.
    std::cout << "\033[36m║\033[0m"
              << "  Geração: \033[1;33m" << std::setw(5) << gen << "\033[0m"
              << "  Pop: \033[1;32m"     << std::setw(6) << pop << "\033[0m"
              << "  Processos: \033[1;35m" << procs        << "\033[0m"
              << "  Ghost cells: \033[1;35m" << ghost      << "\033[0m"
              << "  Média: \033[1;34m"   << std::fixed << std::setprecision(2)
              << avg_ms << " ms\033[0m"
              << "\033[36m" << std::string(3, ' ') << "║\033[0m\n";

    std::cout << "\033[1;36m╚══════════════════════════════════════════════════════════════════╝\033[0m\n";

    // Limita o que exibe para não estourar o terminal
    int disp_cols = std::min(C, 130);
    int disp_rows = std::min(R, 40);

    std::cout << "┌" << std::string(disp_cols, '-') << "┐\n";
    for(int r=0;r<disp_rows;++r){
        std::cout << "│";
        for(int c=0;c<disp_cols;++c)
            // Célula viva = bloco verde '█', célula morta = espaço em branco
            std::cout << (g[r*C+c] ? "\033[1;32m█\033[0m" : " ");
        std::cout << "│\n";
    }
    std::cout << "└" << std::string(disp_cols, '-') << "┘\n";

    if(R > disp_rows)
        std::cout << "  (" << (R-disp_rows) << " linhas não exibidas)\n";

    std::cout.flush();
}

// ============================================================================
// Estado compartilhado entre todos os processos (threads)
// ============================================================================
// Esta struct guarda tudo que os processos precisam acessar em conjunto:
//   - O tabuleiro global (leitura e escrita)
//   - Os buffers de ghost cells (troca entre vizinhos)
//   - A divisão de trabalho (quais linhas cada processo gerencia)
//   - Estatísticas de desempenho
struct SharedState {
    int  ROWS, COLS, GHOST, PROCS;

    // Dois tabuleiros: o atual (que lemos) e o próximo (que escrevemos).
    // Alternamos entre eles a cada geração (double buffering).
    // Isso evita que um processo leia dados que outro já atualizou.
    Board global_cur;   // estado atual
    Board global_nxt;   // próxima geração sendo calculada

    // Buffers de ghost cells:
    //   ghost_top[p] = linhas que o processo p recebe do seu vizinho de cima (p-1)
    //   ghost_bot[p] = linhas que o processo p recebe do seu vizinho de baixo (p+1)
    // Cada buffer tem GHOST*COLS células.
    std::vector<Board> ghost_top;
    std::vector<Board> ghost_bot;

    // Divisão de trabalho: processo p gerencia linhas [row_start[p], row_start[p]+row_count[p])
    std::vector<int> row_start;   // índice da primeira linha de cada processo
    std::vector<int> row_count;   // quantidade de linhas de cada processo

    // Estatísticas
    std::vector<double> compute_time_ms;  // tempo gasto calculando por processo
    std::atomic<int>    total_pop{0};     // contador atômico da população total

    // Construtor: inicializa todos os buffers e divide o trabalho entre os processos
    SharedState(int R, int C, int G, int P)
        : ROWS(R), COLS(C), GHOST(G), PROCS(P),
          global_cur(R*C, 0), global_nxt(R*C, 0),
          ghost_top(P, Board(G*C, 0)),
          ghost_bot(P, Board(G*C, 0)),
          row_start(P), row_count(P),
          compute_time_ms(P, 0.0)
    {
        // Distribuição de carga balanceada:
        // Se ROWS não é divisível por PROCS, os primeiros 'rem' processos
        // recebem uma linha extra para equilibrar.
        int base = R / P, rem = R % P;
        int off = 0;
        for(int p=0;p<P;++p){
            row_start[p] = off;
            row_count[p] = base + (p < rem ? 1 : 0);
            off += row_count[p];
        }
    }
};

// ============================================================================
// Função principal de cada processo (thread)
// ============================================================================
// Cada processo executa esta função em paralelo com os outros.
// Os argumentos são:
//   pid         — identificador do processo (0 a PROCS-1)
//   S           — estado compartilhado (tabuleiro, ghost cells, etc.)
//   bar         — barreira de sincronização C++20 (todos esperam aqui)
//   gens        — total de gerações a simular
//   display     — se deve exibir animação
//   delay_ms    — pausa entre frames
//   gen_counter — contador atômico da geração atual (para o relatório)
static void process_func(int pid, SharedState& S, std::barrier<>& bar,
                         int gens, bool display, int delay_ms,
                         std::atomic<double>& /*total_avg_ms*/,
                         std::atomic<int>& gen_counter)
{
    const int C  = S.COLS;
    const int G  = S.GHOST;
    const int P  = S.PROCS;
    const int rs = S.row_start[pid];  // primeira linha real deste processo
    const int rc = S.row_count[pid];  // quantas linhas reais deste processo

    // Identifica vizinhos com wrap-around: o processo 0 é vizinho do último
    int prev = (pid - 1 + P) % P;  // processo acima
    int next = (pid + 1) % P;      // processo abaixo

    // Loop principal: executa uma iteração por geração
    for(int gen = 1; gen <= gens; ++gen) {

        // ── PASSO 1: TROCA DE GHOST CELLS ─────────────────────────────────
        //
        // Cada processo copia suas linhas de borda para os buffers ghost
        // dos vizinhos. Como estamos em memória compartilhada (threads do
        // mesmo processo), isso é apenas um memcpy — em MPI real seria
        // MPI_Sendrecv ou similar.
        //
        // Processo pid → envia suas primeiras G linhas → ghost_bot[prev]
        // (o vizinho de cima precisa dessas linhas como sua borda inferior)
        {
            const uint8_t* src = S.global_cur.data() + rs * C;
            uint8_t*       dst = S.ghost_bot[prev].data();
            std::memcpy(dst, src, (size_t)G * C);
        }
        // Processo pid → envia suas últimas G linhas → ghost_top[next]
        // (o vizinho de baixo precisa dessas linhas como sua borda superior)
        {
            const uint8_t* src = S.global_cur.data() + (rs + rc - G) * C;
            uint8_t*       dst = S.ghost_top[next].data();
            std::memcpy(dst, src, (size_t)G * C);
        }

        // ── PASSO 2: BARREIRA — aguarda todos terminarem a troca ───────────
        //
        // Antes de calcular, todos os processos devem ter terminado de
        // copiar suas ghost cells. A barreira garante isso: nenhum processo
        // passa daqui até que todos tenham chegado neste ponto.
        bar.arrive_and_wait();

        // ── PASSO 3: CÁLCULO LOCAL ─────────────────────────────────────────
        //
        // Cada processo monta sua visão local: ghost_top + região_real + ghost_bot
        // e aplica as regras de Conway apenas nas suas linhas reais.
        auto t0 = std::chrono::high_resolution_clock::now();

        // trl = total de linhas no buffer local (incluindo ghosts)
        int trl = G + rc + G;
        Board local(trl * C, 0);

        // Monta o buffer local copiando ghost cells e região real
        std::memcpy(local.data(),
                    S.ghost_top[pid].data(), (size_t)G * C);
        std::memcpy(local.data() + G * C,
                    S.global_cur.data() + rs * C, (size_t)rc * C);
        std::memcpy(local.data() + (G + rc) * C,
                    S.ghost_bot[pid].data(), (size_t)G * C);

        // Aplica as regras de Conway para cada célula da região real
        // A região real está nas linhas [G, G+rc) do buffer local
        int local_pop = 0;
        for(int lr = G; lr < G + rc; ++lr) {
            for(int c = 0; c < C; ++c) {
                // Conta os vizinhos vivos nos 8 vizinhos ao redor de (lr, c)
                int live = 0;
                for(int dr = -1; dr <= 1; ++dr) {
                    int nr = lr + dr;
                    if(nr < 0 || nr >= trl) continue;  // não existe (borda do buffer)
                    for(int dc = -1; dc <= 1; ++dc) {
                        if(dr==0 && dc==0) continue;  // ignora a própria célula
                        int nc = c + dc;
                        // Wrap horizontal: o tabuleiro é "cilíndrico" nas colunas
                        if(nc < 0)  nc += C;
                        if(nc >= C) nc -= C;
                        live += local[nr * C + nc];
                    }
                }
                uint8_t cell = local[lr * C + c];
                // Aplica as regras B3/S23:
                uint8_t nxt  = (cell==1)
                    ? ((live==2||live==3) ? 1 : 0)  // viva: sobrevive com 2 ou 3 vizinhos
                    : ((live==3) ? 1 : 0);           // morta: nasce com exatamente 3 vizinhos
                // Escreve o resultado no tabuleiro global_nxt
                // lr - G converte a linha local para a linha global
                S.global_nxt[(rs + lr - G) * C + c] = nxt;
                local_pop += nxt;
            }
        }

        // Mede o tempo de cálculo deste processo nesta geração
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        S.compute_time_ms[pid] += ms;

        // Incrementa o contador atômico de população (thread-safe)
        S.total_pop.fetch_add(local_pop, std::memory_order_relaxed);

        // ── PASSO 4: BARREIRA — aguarda todos terminarem o cálculo ─────────
        //
        // Nenhum processo pode avançar (fazer o swap dos tabuleiros) antes
        // que todos tenham terminado de escrever em global_nxt.
        bar.arrive_and_wait();

        // ── PASSO 5: Processo 0 gerencia a transição entre gerações ────────
        //
        // Apenas o processo 0 executa esta seção:
        //   - Troca global_cur e global_nxt (swap de ponteiros, O(1))
        //   - Exibe o tabuleiro no terminal
        //   - Reseta o contador de população para a próxima geração
        if(pid == 0) {
            std::swap(S.global_cur, S.global_nxt);

            // Calcula o tempo médio de compute por geração (em todos os processos)
            double avg_ms = 0;
            for(int p=0;p<P;++p) avg_ms += S.compute_time_ms[p];
            avg_ms /= (gen * P);

            if(display) {
                render(S.global_cur, S.ROWS, C, gen,
                       S.total_pop.load(), avg_ms, P, G);
                if(delay_ms > 0)
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(delay_ms));
            }
            // Reseta a população para a próxima geração
            S.total_pop.store(0, std::memory_order_relaxed);
            gen_counter.store(gen, std::memory_order_relaxed);
        }

        // ── PASSO 6: BARREIRA FINAL — aguarda o swap e exibição ────────────
        //
        // Todos os processos esperam antes de começar a próxima geração.
        // Sem isso, alguns poderiam começar a ler global_cur da próxima
        // geração antes que o processo 0 termine o swap.
        bar.arrive_and_wait();
    }
}

// ============================================================================
// MAIN — ponto de entrada do programa
// ============================================================================
int main(int argc, char** argv)
{
    Args a = parse_args(argc, argv);

    const int ROWS  = a.rows;
    const int COLS  = a.cols;
    const int GHOST = a.ghost;
    const int PROCS = a.procs;
    const int GENS  = a.gens;

    // Validação: o número de ghost cells deve ser menor que o número de linhas
    // por processo, caso contrário as ghost cells se sobreporiam à região real.
    if(GHOST < 1 || GHOST > ROWS/PROCS) {
        std::cerr << "[ERRO] GHOST deve ser >= 1 e <= ROWS/PROCS ("
                  << ROWS/PROCS << ").\n";
        return 1;
    }

    // ── Inicializa o estado compartilhado ────────────────────────────────────
    // Aloca o tabuleiro, os buffers de ghost cells e faz a divisão de linhas
    SharedState S(ROWS, COLS, GHOST, PROCS);

    // ── Inicializa o tabuleiro com o padrão escolhido ────────────────────────
    if(!a.file_path.empty()) {
        std::ifstream f(a.file_path);
        if(!f){ std::cerr << "Erro ao abrir: " << a.file_path << "\n"; return 1; }
        std::string content((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());
        bool is_rle = (a.file_path.size()>=4 &&
                       a.file_path.substr(a.file_path.size()-4)==".rle")
                      || content.find("x =")!=std::string::npos;
        if(is_rle) apply_rle(S.global_cur, ROWS, COLS, content);
        else       apply_cells(S.global_cur, ROWS, COLS, content);
    } else if(a.pattern=="rle" && !a.rle_str.empty()) {
        apply_rle(S.global_cur, ROWS, COLS, a.rle_str);
    } else if(a.pattern=="blinker") { pat_blinker(S.global_cur, ROWS, COLS); }
    else if(a.pattern=="pulsar")    { pat_pulsar (S.global_cur, ROWS, COLS); }
    else if(a.pattern=="gosper")    { pat_gosper (S.global_cur, ROWS, COLS); }
    else if(a.pattern=="random")    { pat_random (S.global_cur, ROWS, COLS, a.seed); }
    else                            { pat_glider (S.global_cur, ROWS, COLS); }

    // ── Exibe a geração 0 (estado inicial, antes de qualquer cálculo) ────────
    if(a.display) {
        int pop = 0;
        for(auto v : S.global_cur) pop += v;
        render(S.global_cur, ROWS, COLS, 0, pop, 0.0, PROCS, GHOST);
        if(a.delay > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(a.delay));
    }

    // ── Exibe a divisão de linhas entre os processos ─────────────────────────
    if(a.display) {
        std::cout << "\n\033[1;33m  Divisão de linhas entre os " << PROCS << " processos:\033[0m\n";
        for(int p=0;p<PROCS;++p)
            std::cout << "    Processo " << std::setw(2) << p
                      << ": linhas [" << std::setw(3) << S.row_start[p]
                      << ", " << std::setw(3) << S.row_start[p]+S.row_count[p]-1
                      << "]  (+" << GHOST << " ghost acima e abaixo)\n";
        std::cout << "\n  Iniciando em 2s...\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }

    // ── Cria a barreira e lança as threads ───────────────────────────────────
    //
    // std::barrier (C++20): sincroniza PROCS threads.
    // Cada thread chama arrive_and_wait() e bloqueia até que todas tenham chegado.
    std::barrier bar(PROCS);
    std::atomic<double> total_avg_ms{0.0};
    std::atomic<int>    gen_counter{0};

    // Marca o início para medir o tempo total (wall clock)
    auto t_wall_start = std::chrono::high_resolution_clock::now();

    // Cria e lança PROCS threads, cada uma executando process_func com seu pid
    std::vector<std::thread> threads;
    threads.reserve(PROCS);
    for(int p = 0; p < PROCS; ++p) {
        threads.emplace_back(process_func, p,
                             std::ref(S), std::ref(bar),
                             GENS, a.display, a.delay,
                             std::ref(total_avg_ms),
                             std::ref(gen_counter));
    }
    // Aguarda todas as threads terminarem (join)
    for(auto& t : threads) t.join();

    auto t_wall_end = std::chrono::high_resolution_clock::now();
    double wall_ms = std::chrono::duration<double, std::milli>(
                         t_wall_end - t_wall_start).count();

    // Reativa o cursor do terminal (havia sido escondido durante a animação)
    if(a.display) std::cout << "\033[?25h";

    // ── Relatório final de desempenho ────────────────────────────────────────
    double total_compute = 0;
    for(int p=0;p<PROCS;++p) total_compute += S.compute_time_ms[p];
    double avg_compute   = total_compute / (PROCS * GENS);
    // Eficiência paralela: quanto do tempo total foi gasto efetivamente computando
    // (vs. sincronização, overhead de threads, etc.)
    double parallel_eff  = (total_compute / PROCS) / wall_ms * 100.0;

    int final_pop = 0;
    for(auto v : S.global_cur) final_pop += v;

    std::cout << "\n\033[1;36m";
    std::cout << "╔══════════════════════════════════════════════════════════════════╗\n";
    std::cout << "║                   RELATÓRIO DE DESEMPENHO                       ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";
    std::cout << "\033[0m";
    auto row = [](const std::string& k, const std::string& v){
        std::cout << "\033[36m║\033[0m  \033[33m" << std::left << std::setw(28) << k
                  << "\033[0m" << std::left << std::setw(34) << v
                  << "\033[36m║\033[0m\n";
    };
    row("Tabuleiro",        std::to_string(DEF_ROWS>0? DEF_ROWS:0) +
                            "→ " + std::to_string(ROWS) + " × " + std::to_string(COLS));
    row("Gerações simuladas", std::to_string(GENS));
    row("Processos paralelos", std::to_string(PROCS));
    row("Ghost cells por lado", std::to_string(GHOST));
    row("Tempo wall total",    std::to_string((int)wall_ms) + " ms");
    row("Média compute/gen",   std::to_string(avg_compute).substr(0,6) + " ms");
    row("Eficiência paralela", std::to_string((int)parallel_eff) + " %");
    row("População final",     std::to_string(final_pop));

    std::cout << "\033[1;36m";
    std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";
    std::cout << "║  Tempos por processo (ms acumulados em " << std::setw(3) << GENS << " gerações):           ║\n";
    std::cout << "╠══════════════════════════════════════════════════════════════════╣\n";
    std::cout << "\033[0m";
    for(int p=0;p<PROCS;++p){
        std::ostringstream oss;
        oss << "Processo " << std::setw(2) << p
            << " (" << std::setw(3) << S.row_count[p] << " linhas)";
        std::ostringstream v;
        v << std::fixed << std::setprecision(3) << S.compute_time_ms[p] << " ms";
        row(oss.str(), v.str());
    }
    std::cout << "\033[1;36m╚══════════════════════════════════════════════════════════════════╝\033[0m\n";

    return 0;
}
