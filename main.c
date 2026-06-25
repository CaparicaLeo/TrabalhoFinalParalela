/**
 * ============================================================================
 *  Jogo da Vida de Conway — Paralelismo com Múltiplos Processos e Ghost Cells
 * ============================================================================
 *
 *  COMPILAÇÃO
 *  ──────────
 *    gcc -O2 -std=c11 -pthread -o game_of_life game_of_life.c
 *
 *  USO  (idêntico à versão C++)
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

#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <stdatomic.h>   /* C11 — _Atomic */

/* ============================================================================
 * Constantes padrão
 * ============================================================================ */
#define DEF_ROWS    64
#define DEF_COLS    128
#define DEF_GENS    200
#define DEF_DELAY   80
#define DEF_GHOST   5
#define DEF_PROCS   16

/* ============================================================================
 * Utilitários
 * ============================================================================ */
static int min_int(int a, int b) { return a < b ? a : b; }

/* Pausa em milissegundos — Windows (MinGW) ou POSIX */
#ifdef _WIN32
#  include <windows.h>
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
static void sleep_ms(int ms) {
    struct timespec ts;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

/* Tempo atual em milissegundos (wall-clock) — Windows ou POSIX */
#ifdef _WIN32
static double now_ms(void) {
    LARGE_INTEGER freq, cnt;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&cnt);
    return (double)cnt.QuadPart / freq.QuadPart * 1000.0;
}
#else
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif

/* ============================================================================
 * Gerador de números pseudo-aleatórios simples (substituição do mt19937)
 * Usa xorshift32 com semente configurável — suficiente para o padrão random.
 * ============================================================================ */
typedef struct { uint32_t state; } RNG;

static void     rng_seed(RNG* r, uint32_t s) { r->state = s ? s : 1; }
static uint32_t rng_next(RNG* r) {
    r->state ^= r->state << 13;
    r->state ^= r->state >> 17;
    r->state ^= r->state << 5;
    return r->state;
}

/* ============================================================================
 * Argumentos de linha de comando
 * ============================================================================ */
typedef struct {
    char    pattern[64];
    char    rle_str[8192];
    char    file_path[1024];
    int     rows, cols, gens, delay, ghost, procs;
    int     display;     /* 1 = exibir animação, 0 = benchmark */
    unsigned seed;
} Args;

static void args_init(Args* a) {
    strncpy(a->pattern,   "glider", sizeof(a->pattern));
    a->rle_str[0]    = '\0';
    a->file_path[0]  = '\0';
    a->rows    = DEF_ROWS;
    a->cols    = DEF_COLS;
    a->gens    = DEF_GENS;
    a->delay   = DEF_DELAY;
    a->ghost   = DEF_GHOST;
    a->procs   = DEF_PROCS;
    a->display = 1;
    a->seed    = 42;
}

static void parse_args(int argc, char** argv, Args* a) {
    args_init(a);
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--pattern") == 0 && i+1 < argc)
            strncpy(a->pattern, argv[++i], sizeof(a->pattern)-1);
        else if (strcmp(argv[i], "--rle") == 0 && i+1 < argc)
            strncpy(a->rle_str, argv[++i], sizeof(a->rle_str)-1);
        else if (strcmp(argv[i], "--file") == 0 && i+1 < argc)
            strncpy(a->file_path, argv[++i], sizeof(a->file_path)-1);
        else if (strcmp(argv[i], "--rows")  == 0 && i+1 < argc) a->rows  = atoi(argv[++i]);
        else if (strcmp(argv[i], "--cols")  == 0 && i+1 < argc) a->cols  = atoi(argv[++i]);
        else if (strcmp(argv[i], "--gens")  == 0 && i+1 < argc) a->gens  = atoi(argv[++i]);
        else if (strcmp(argv[i], "--delay") == 0 && i+1 < argc) a->delay = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ghost") == 0 && i+1 < argc) a->ghost = atoi(argv[++i]);
        else if (strcmp(argv[i], "--procs") == 0 && i+1 < argc) a->procs = atoi(argv[++i]);
        else if (strcmp(argv[i], "--seed")  == 0 && i+1 < argc) a->seed  = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--no-display") == 0)           a->display = 0;
    }
}

/* ============================================================================
 * Inicialização de padrões
 * ============================================================================ */
static void pat_glider(uint8_t* g, int R, int C) {
    (void)R;
    g[1*C+2]=1; g[2*C+3]=1;
    g[3*C+1]=1; g[3*C+2]=1; g[3*C+3]=1;
}

static void pat_blinker(uint8_t* g, int R, int C) {
    int r = R/2, c = C/2;
    g[r*C+(c-1)]=1; g[r*C+c]=1; g[r*C+(c+1)]=1;
}

static void pat_pulsar(uint8_t* g, int R, int C) {
    int r0 = R/2-6, c0 = C/2-6;
    int pts[][2] = {
        {0,2},{0,3},{0,4},{0,8},{0,9},{0,10},
        {2,0},{3,0},{4,0},{2,5},{3,5},{4,5},
        {2,7},{3,7},{4,7},{2,12},{3,12},{4,12},
        {5,2},{5,3},{5,4},{5,8},{5,9},{5,10},
        {7,2},{7,3},{7,4},{7,8},{7,9},{7,10},
        {8,0},{9,0},{10,0},{8,5},{9,5},{10,5},
        {8,7},{9,7},{10,7},{8,12},{9,12},{10,12},
        {12,2},{12,3},{12,4},{12,8},{12,9},{12,10}
    };
    int n = (int)(sizeof(pts)/sizeof(pts[0]));
    for (int i = 0; i < n; ++i) {
        int r = r0+pts[i][0], c = c0+pts[i][1];
        if (r>=0&&r<R&&c>=0&&c<C) g[r*C+c]=1;
    }
}

static void pat_gosper(uint8_t* g, int R, int C) {
    int r0=2, c0=2;
    int pts[][2] = {
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
    int n = (int)(sizeof(pts)/sizeof(pts[0]));
    for (int i = 0; i < n; ++i) {
        int r = r0+pts[i][0], c = c0+pts[i][1];
        if (r>=0&&r<R&&c>=0&&c<C) g[r*C+c]=1;
    }
}

static void pat_random(uint8_t* g, int R, int C, unsigned seed) {
    RNG rng;
    rng_seed(&rng, seed);
    for (int i = 0; i < R*C; ++i)
        g[i] = (rng_next(&rng) % 3 == 0) ? 1 : 0;
}

/* ============================================================================
 * Parser RLE
 * ============================================================================ */
static void apply_rle(uint8_t* g, int R, int C, const char* src) {
    /* Extrai cabeçalho para obter dimensões do padrão */
    int pw = 0, ph = 0;
    const char* p = src;

    /* Lê linha a linha buscando "x = N, y = N" */
    char line[512];
    const char* data_start = src;
    while (*p) {
        const char* end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line)-1;
        memcpy(line, p, len);
        line[len] = '\0';

        if (line[0] == '#') { p = end ? end+1 : p+len; continue; }
        if (line[0] == 'x' || line[0] == 'X') {
            sscanf(line, "x = %d, y = %d", &pw, &ph);
            p = end ? end+1 : p+len;
            data_start = p;
            continue;
        }
        /* Primeira linha que não é comentário nem cabeçalho = início dos dados */
        data_start = p;
        break;
    }

    int or0 = (ph > 0) ? (R - ph) / 2 : R / 4;
    int oc0 = (pw > 0) ? (C - pw) / 2 : C / 4;

    int row = or0, col = oc0, cnt = 0;
    for (const char* ch = data_start; *ch; ++ch) {
        if (*ch == '!') break;
        if (*ch == '\n' || *ch == '\r') continue;
        if (isdigit((unsigned char)*ch)) {
            cnt = cnt * 10 + (*ch - '0');
            continue;
        }
        int rep = (cnt == 0) ? 1 : cnt;
        cnt = 0;
        if (*ch == '$') {
            row += rep;
            col  = oc0;
        } else if (*ch == 'b') {
            col += rep;
        } else if (*ch == 'o') {
            for (int i = 0; i < rep; ++i) {
                if (row >= 0 && row < R && col >= 0 && col < C)
                    g[row * C + col] = 1;
                ++col;
            }
        }
    }
}

/* Parser .cells (plaintext) */
static void apply_cells(uint8_t* g, int R, int C, const char* src) {
    /* Coleta linhas não-comentário */
    #define MAX_LINES 2048
    const char* lines[MAX_LINES];
    int         lens[MAX_LINES];
    int nlines = 0, maxw = 0;

    const char* p = src;
    while (*p && nlines < MAX_LINES) {
        const char* end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > 0 && p[0] != '!') {
            lines[nlines]  = p;
            lens[nlines]   = (int)len;
            if ((int)len > maxw) maxw = (int)len;
            ++nlines;
        }
        p = end ? end + 1 : p + len;
        if (!end) break;
    }

    int or0 = (R - nlines) / 2;
    int oc0 = (C - maxw)   / 2;

    for (int r = 0; r < nlines; ++r) {
        for (int c = 0; c < lens[r]; ++c) {
            int rr = or0 + r, cc = oc0 + c;
            if (rr >= 0 && rr < R && cc >= 0 && cc < C)
                g[rr * C + cc] = (lines[r][c] == 'O' || lines[r][c] == '*') ? 1 : 0;
        }
    }
    #undef MAX_LINES
}

/* ============================================================================
 * Renderização no terminal (ANSI)
 * ============================================================================ */
static void render(const uint8_t* g, int R, int C, int gen, int pop,
                   double avg_ms, int procs, int ghost)
{
    printf("\033[H\033[2J\033[?25l");

    printf("\033[1;36m");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║      🌱 Jogo da Vida de Conway — Paralelismo com Processos       ║\n");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("\033[0m");

    printf("\033[36m║\033[0m"
           "  Geração: \033[1;33m%5d\033[0m"
           "  Pop: \033[1;32m%6d\033[0m"
           "  Processos: \033[1;35m%d\033[0m"
           "  Ghost cells: \033[1;35m%d\033[0m"
           "  Média: \033[1;34m%.2f ms\033[0m"
           "\033[36m   ║\033[0m\n",
           gen, pop, procs, ghost, avg_ms);

    printf("\033[1;36m╚══════════════════════════════════════════════════════════════════╝\033[0m\n");

    int disp_cols = min_int(C, 130);
    int disp_rows = min_int(R, 40);

    printf("┌");
    for (int i = 0; i < disp_cols; ++i) printf("-");
    printf("┐\n");

    for (int r = 0; r < disp_rows; ++r) {
        printf("│");
        for (int c = 0; c < disp_cols; ++c)
            printf("%s", g[r*C+c] ? "\033[1;32m█\033[0m" : " ");
        printf("│\n");
    }

    printf("└");
    for (int i = 0; i < disp_cols; ++i) printf("-");
    printf("┘\n");

    if (R > disp_rows)
        printf("  (%d linhas não exibidas)\n", R - disp_rows);

    fflush(stdout);
}

/* ============================================================================
 * Estado compartilhado entre todos os processos (threads)
 * ============================================================================ */
typedef struct {
    int ROWS, COLS, GHOST, PROCS;

    /* Double buffering — alternamos a cada geração */
    uint8_t* global_cur;
    uint8_t* global_nxt;

    /* ghost_top[p] e ghost_bot[p]: buffers de borda para o processo p */
    uint8_t** ghost_top;
    uint8_t** ghost_bot;

    /* Divisão de linhas entre os processos */
    int* row_start;
    int* row_count;

    /* Estatísticas */
    double*        compute_time_ms;
    _Atomic int    total_pop;

    /* Barreira de sincronização (POSIX) */
    pthread_barrier_t barrier;

    /* Configurações de exibição */
    int gens;
    int display;
    int delay_ms;

    /* Contador de geração visível externamente */
    _Atomic int gen_counter;
} SharedState;

/* Aloca e inicializa o estado compartilhado */
static SharedState* state_create(int R, int C, int G, int P,
                                 int gens, int display, int delay_ms)
{
    SharedState* S = (SharedState*)calloc(1, sizeof(SharedState));
    S->ROWS    = R;
    S->COLS    = C;
    S->GHOST   = G;
    S->PROCS   = P;
    S->gens    = gens;
    S->display = display;
    S->delay_ms= delay_ms;

    S->global_cur = (uint8_t*)calloc((size_t)R * C, 1);
    S->global_nxt = (uint8_t*)calloc((size_t)R * C, 1);

    S->ghost_top = (uint8_t**)malloc(P * sizeof(uint8_t*));
    S->ghost_bot = (uint8_t**)malloc(P * sizeof(uint8_t*));
    for (int p = 0; p < P; ++p) {
        S->ghost_top[p] = (uint8_t*)calloc((size_t)G * C, 1);
        S->ghost_bot[p] = (uint8_t*)calloc((size_t)G * C, 1);
    }

    S->row_start        = (int*)malloc(P * sizeof(int));
    S->row_count        = (int*)malloc(P * sizeof(int));
    S->compute_time_ms  = (double*)calloc(P, sizeof(double));

    /* Distribuição balanceada de linhas */
    int base = R / P, rem = R % P, off = 0;
    for (int p = 0; p < P; ++p) {
        S->row_start[p] = off;
        S->row_count[p] = base + (p < rem ? 1 : 0);
        off += S->row_count[p];
    }

    atomic_init(&S->total_pop,    0);
    atomic_init(&S->gen_counter,  0);
    pthread_barrier_init(&S->barrier, NULL, (unsigned)P);

    return S;
}

/* Libera toda a memória alocada */
static void state_destroy(SharedState* S) {
    free(S->global_cur);
    free(S->global_nxt);
    for (int p = 0; p < S->PROCS; ++p) {
        free(S->ghost_top[p]);
        free(S->ghost_bot[p]);
    }
    free(S->ghost_top);
    free(S->ghost_bot);
    free(S->row_start);
    free(S->row_count);
    free(S->compute_time_ms);
    pthread_barrier_destroy(&S->barrier);
    free(S);
}

/* ============================================================================
 * Argumento passado para cada thread
 * ============================================================================ */
typedef struct {
    int          pid;
    SharedState* S;
} ThreadArg;

/* ============================================================================
 * Função de cada thread (equivalente a process_func em C++)
 * ============================================================================ */
static void* process_func(void* arg) {
    ThreadArg*    ta  = (ThreadArg*)arg;
    int           pid = ta->pid;
    SharedState*  S   = ta->S;

    const int C  = S->COLS;
    const int G  = S->GHOST;
    const int P  = S->PROCS;
    const int rs = S->row_start[pid];
    const int rc = S->row_count[pid];
    const int gens    = S->gens;
    const int display = S->display;
    const int delay_ms= S->delay_ms;

    int prev = (pid - 1 + P) % P;
    int next = (pid + 1) % P;

    /* Buffer local: ghost_top + região_real + ghost_bot */
    int trl = G + rc + G;
    uint8_t* local = (uint8_t*)malloc((size_t)trl * C);

    for (int gen = 1; gen <= gens; ++gen) {

        /* ── PASSO 1: TROCA DE GHOST CELLS ──────────────────────────────── */
        memcpy(S->ghost_bot[prev], S->global_cur + rs * C,            (size_t)G * C);
        memcpy(S->ghost_top[next], S->global_cur + (rs + rc - G) * C, (size_t)G * C);

        /* ── PASSO 2: BARREIRA ───────────────────────────────────────────── */
        pthread_barrier_wait(&S->barrier);

        /* ── PASSO 3: CÁLCULO LOCAL ──────────────────────────────────────── */
        double t0 = now_ms();

        memcpy(local,                  S->ghost_top[pid], (size_t)G  * C);
        memcpy(local + G * C,          S->global_cur + rs * C, (size_t)rc * C);
        memcpy(local + (G + rc) * C,   S->ghost_bot[pid], (size_t)G  * C);

        int local_pop = 0;
        for (int lr = G; lr < G + rc; ++lr) {
            for (int c = 0; c < C; ++c) {
                int live = 0;
                for (int dr = -1; dr <= 1; ++dr) {
                    int nr = lr + dr;
                    if (nr < 0 || nr >= trl) continue;
                    for (int dc = -1; dc <= 1; ++dc) {
                        if (dr == 0 && dc == 0) continue;
                        int nc = c + dc;
                        if (nc < 0)  nc += C;
                        if (nc >= C) nc -= C;
                        live += local[nr * C + nc];
                    }
                }
                uint8_t cell = local[lr * C + c];
                uint8_t nxt  = (cell == 1)
                    ? ((live == 2 || live == 3) ? 1 : 0)
                    : ((live == 3) ? 1 : 0);
                S->global_nxt[(rs + lr - G) * C + c] = nxt;
                local_pop += nxt;
            }
        }

        double t1 = now_ms();
        S->compute_time_ms[pid] += (t1 - t0);

        /* Incremento atômico da população */
        atomic_fetch_add_explicit(&S->total_pop, local_pop, memory_order_relaxed);

        /* ── PASSO 4: BARREIRA ───────────────────────────────────────────── */
        pthread_barrier_wait(&S->barrier);

        /* ── PASSO 5: Processo 0 gerencia a transição ────────────────────── */
        if (pid == 0) {
            /* Swap dos dois tabuleiros (troca de ponteiros) */
            uint8_t* tmp   = S->global_cur;
            S->global_cur  = S->global_nxt;
            S->global_nxt  = tmp;

            double avg_ms = 0.0;
            for (int p = 0; p < P; ++p) avg_ms += S->compute_time_ms[p];
            avg_ms /= (gen * P);

            if (display) {
                render(S->global_cur, S->ROWS, C, gen,
                       atomic_load(&S->total_pop), avg_ms, P, G);
                if (delay_ms > 0) sleep_ms(delay_ms);
            }

            atomic_store_explicit(&S->total_pop,   0,   memory_order_relaxed);
            atomic_store_explicit(&S->gen_counter, gen, memory_order_relaxed);
        }

        /* ── PASSO 6: BARREIRA FINAL ─────────────────────────────────────── */
        pthread_barrier_wait(&S->barrier);
    }

    free(local);
    return NULL;
}

/* ============================================================================
 * MAIN
 * ============================================================================ */
int main(int argc, char** argv) {
    Args a;
    parse_args(argc, argv, &a);

    const int ROWS  = a.rows;
    const int COLS  = a.cols;
    const int GHOST = a.ghost;
    const int PROCS = a.procs;
    const int GENS  = a.gens;

    if (GHOST < 1 || GHOST > ROWS / PROCS) {
        fprintf(stderr, "[ERRO] GHOST deve ser >= 1 e <= ROWS/PROCS (%d).\n",
                ROWS / PROCS);
        return 1;
    }

    /* ── Inicializa estado compartilhado ──────────────────────────────────── */
    SharedState* S = state_create(ROWS, COLS, GHOST, PROCS,
                                  GENS, a.display, a.delay);

    /* ── Inicializa o tabuleiro com o padrão escolhido ──────────────────── */
    if (a.file_path[0] != '\0') {
        FILE* f = fopen(a.file_path, "r");
        if (!f) { fprintf(stderr, "Erro ao abrir: %s\n", a.file_path); return 1; }
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);
        char* content = (char*)malloc(sz + 1);
        size_t nread = fread(content, 1, sz, f);
        content[nread] = '\0';
        fclose(f);

        /* Detecta RLE pela extensão ou conteúdo */
        int is_rle = (sz >= 4 && strcmp(a.file_path + strlen(a.file_path) - 4, ".rle") == 0)
                     || (strstr(content, "x =") != NULL);
        if (is_rle) apply_rle  (S->global_cur, ROWS, COLS, content);
        else        apply_cells(S->global_cur, ROWS, COLS, content);
        free(content);
    } else if (strcmp(a.pattern, "rle") == 0 && a.rle_str[0] != '\0') {
        apply_rle(S->global_cur, ROWS, COLS, a.rle_str);
    } else if (strcmp(a.pattern, "blinker") == 0) { pat_blinker(S->global_cur, ROWS, COLS); }
    else if   (strcmp(a.pattern, "pulsar")  == 0) { pat_pulsar (S->global_cur, ROWS, COLS); }
    else if   (strcmp(a.pattern, "gosper")  == 0) { pat_gosper (S->global_cur, ROWS, COLS); }
    else if   (strcmp(a.pattern, "random")  == 0) { pat_random (S->global_cur, ROWS, COLS, a.seed); }
    else                                           { pat_glider (S->global_cur, ROWS, COLS); }

    /* ── Exibe a geração 0 ───────────────────────────────────────────────── */
    if (a.display) {
        int pop = 0;
        for (int i = 0; i < ROWS * COLS; ++i) pop += S->global_cur[i];
        render(S->global_cur, ROWS, COLS, 0, pop, 0.0, PROCS, GHOST);
        if (a.delay > 0) sleep_ms(a.delay);
    }

    /* ── Exibe divisão de linhas ─────────────────────────────────────────── */
    if (a.display) {
        printf("\n\033[1;33m  Divisão de linhas entre os %d processos:\033[0m\n", PROCS);
        for (int p = 0; p < PROCS; ++p)
            printf("    Processo %2d: linhas [%3d, %3d]  (+%d ghost acima e abaixo)\n",
                   p, S->row_start[p],
                   S->row_start[p] + S->row_count[p] - 1, GHOST);
        printf("\n  Iniciando em 2s...\n");
        sleep_ms(2000);
    }

    /* ── Lança as threads ────────────────────────────────────────────────── */
    pthread_t*  threads = (pthread_t*)  malloc(PROCS * sizeof(pthread_t));
    ThreadArg*  targs   = (ThreadArg*)  malloc(PROCS * sizeof(ThreadArg));

    double t_wall_start = now_ms();

    for (int p = 0; p < PROCS; ++p) {
        targs[p].pid = p;
        targs[p].S   = S;
        pthread_create(&threads[p], NULL, process_func, &targs[p]);
    }
    for (int p = 0; p < PROCS; ++p)
        pthread_join(threads[p], NULL);

    double wall_ms = now_ms() - t_wall_start;

    if (a.display) printf("\033[?25h");   /* reativa cursor */

    /* ── Relatório final ─────────────────────────────────────────────────── */
    double total_compute = 0.0;
    for (int p = 0; p < PROCS; ++p) total_compute += S->compute_time_ms[p];
    double avg_compute  = total_compute / (PROCS * GENS);
    double parallel_eff = (total_compute / PROCS) / wall_ms * 100.0;

    int final_pop = 0;
    for (int i = 0; i < ROWS * COLS; ++i) final_pop += S->global_cur[i];

    printf("\n\033[1;36m");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                   RELATÓRIO DE DESEMPENHO                       ║\n");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("\033[0m");

    #define SIMPLE_ROW(label, fmt, ...) do { \
        char _v[64]; snprintf(_v, sizeof(_v), fmt, ##__VA_ARGS__); \
        printf("\033[36m║\033[0m  \033[33m%-28s\033[0m %-34s \033[36m║\033[0m\n", label, _v); \
    } while(0)

    SIMPLE_ROW("Tabuleiro",             "%d x %d", ROWS, COLS);
    SIMPLE_ROW("Gerações simuladas",    "%d",   GENS);
    SIMPLE_ROW("Processos paralelos",   "%d",   PROCS);
    SIMPLE_ROW("Ghost cells por lado",  "%d",   GHOST);
    SIMPLE_ROW("Tempo wall total",      "%d ms",(int)wall_ms);
    SIMPLE_ROW("Média compute/gen",     "%.4f ms", avg_compute);
    SIMPLE_ROW("Eficiência paralela",   "%d %%", (int)parallel_eff);
    SIMPLE_ROW("População final",       "%d",   final_pop);

    printf("\033[1;36m");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║  Tempos por processo (ms acumulados em %3d gerações):           ║\n", GENS);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("\033[0m");

    for (int p = 0; p < PROCS; ++p) {
        char label[48], val[32];
        snprintf(label, sizeof(label), "Processo %2d (%3d linhas)", p, S->row_count[p]);
        snprintf(val,   sizeof(val),   "%.3f ms", S->compute_time_ms[p]);
        printf("\033[36m║\033[0m  \033[33m%-28s\033[0m %-34s \033[36m║\033[0m\n", label, val);
    }
    printf("\033[1;36m╚══════════════════════════════════════════════════════════════════╝\033[0m\n");

    #undef SIMPLE_ROW

    /* ── Libera recursos ─────────────────────────────────────────────────── */
    free(threads);
    free(targs);
    state_destroy(S);

    return 0;
}
