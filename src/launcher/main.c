// nxnative-launcher: the Asahi Linux front end. It lists the game profiles it
// can find, shows what each needs, edits the player settings, manages local
// saves, and starts the runtime with the right environment.
//
// SDL3 is loaded at run time (the runtime and this launcher must not require
// the SDL development package). The UI is deliberately dependency-free: a
// renderer window with SDL's built-in debug font, keyboard and gamepad.
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef NXNATIVE_DEFAULT_GAMES_DIR
#define NXNATIVE_DEFAULT_GAMES_DIR ""
#endif
#ifndef NXNATIVE_VERSION
#define NXNATIVE_VERSION "0.1.0"
#endif

// ---- SDL3 ABI subset ----
typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Event { uint8_t data[64]; } SDL_Event;
typedef struct SDL_FRect { float x, y, w, h; } SDL_FRect;
typedef int SDL_Scancode;
enum { SDL_INIT_VIDEO = 0x20u };
enum {
    SDL_SCANCODE_A = 4, SDL_SCANCODE_B = 5, SDL_SCANCODE_C = 6, SDL_SCANCODE_D = 7,
    SDL_SCANCODE_E = 8, SDL_SCANCODE_F = 9, SDL_SCANCODE_G = 10, SDL_SCANCODE_H = 11,
    SDL_SCANCODE_I = 12, SDL_SCANCODE_J = 13, SDL_SCANCODE_K = 14, SDL_SCANCODE_L = 15,
    SDL_SCANCODE_M = 16, SDL_SCANCODE_N = 17, SDL_SCANCODE_O = 18, SDL_SCANCODE_P = 19,
    SDL_SCANCODE_Q = 20, SDL_SCANCODE_R = 21, SDL_SCANCODE_S = 22, SDL_SCANCODE_T = 23,
    SDL_SCANCODE_U = 24, SDL_SCANCODE_V = 25, SDL_SCANCODE_W = 26, SDL_SCANCODE_X = 27,
    SDL_SCANCODE_Y = 28, SDL_SCANCODE_Z = 29,
    SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE = 41, SDL_SCANCODE_BACKSPACE = 42,
    SDL_SCANCODE_TAB = 43, SDL_SCANCODE_SPACE = 44,
    SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT = 80, SDL_SCANCODE_DOWN = 81, SDL_SCANCODE_UP = 82,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2 = 59, SDL_SCANCODE_F5 = 62,
};
static SDL_Window *(*p_SDL_CreateWindow)(const char *, int, int, uint64_t);
static void (*p_SDL_DestroyWindow)(SDL_Window *);
static SDL_Renderer *(*p_SDL_CreateRenderer)(SDL_Window *, const char *);
static void (*p_SDL_DestroyRenderer)(SDL_Renderer *);
static int (*p_SDL_SetRenderDrawColor)(SDL_Renderer *, uint8_t, uint8_t, uint8_t, uint8_t);
static int (*p_SDL_RenderClear)(SDL_Renderer *);
static int (*p_SDL_RenderFillRect)(SDL_Renderer *, const SDL_FRect *);
static int (*p_SDL_RenderRect)(SDL_Renderer *, const SDL_FRect *);
static int (*p_SDL_RenderDebugText)(SDL_Renderer *, float, float, const char *);
static int (*p_SDL_RenderPresent)(SDL_Renderer *);
static int (*p_SDL_SetRenderScale)(SDL_Renderer *, float, float);
static int (*p_SDL_PollEvent)(SDL_Event *);
static const uint8_t *(*p_SDL_GetKeyboardState)(int *);
static int (*p_SDL_InitSubSystem)(uint32_t);
static void (*p_SDL_Quit)(void);
static const char *(*p_SDL_GetError)(void);
static void (*p_SDL_Delay)(uint32_t);
static int (*p_SDL_ShowSimpleMessageBox)(uint32_t, const char *, const char *, SDL_Window *);
static int (*p_SDL_GetWindowSizeInPixels)(SDL_Window *, int *, int *);

static void *sdl;
#define LOAD(n) do { *(void **)(&p_##n) = dlsym(sdl, #n); } while (0)

#include <dlfcn.h>

typedef struct {
    char id[32], name[160], dir[1600], runtime[1800], runtime_note[200];
    int has_files, has_runtime;
    int fps, scale_index, mute;
} game_t;

static game_t *games;
static int ngames, cgames;
static int selected, view;   // 0 list, 1 settings, 2 saves
static int settings_row;
static char status[512];
static char root_default[1024];

static void set_status(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(status, sizeof status, fmt, ap); va_end(ap);
}
static int has_file(const char *dir, const char *name) {
    char p[2048]; snprintf(p, sizeof p, "%s/%s", dir, name);
    return access(p, R_OK) == 0;
}
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static void mkdirs(const char *p) {
    char b[2048]; snprintf(b, sizeof b, "%s", p);
    for (char *q = b + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(b, 0755); *q = '/'; }
    mkdir(b, 0755);
}
static const char *cfg_root(void) {
    static char p[1200];
    const char *x = getenv("XDG_CONFIG_HOME");
    if (x && *x) snprintf(p, sizeof p, "%s/nxnative/launcher", x);
    else snprintf(p, sizeof p, "%s/.config/nxnative/launcher", getenv("HOME") ? getenv("HOME") : ".");
    mkdirs(p);
    return p;
}
static const char *data_root(void) {
    static char p[1200];
    const char *x = getenv("XDG_DATA_HOME");
    if (x && *x) snprintf(p, sizeof p, "%s/nxnative", x);
    else snprintf(p, sizeof p, "%s/.local/share/nxnative", getenv("HOME") ? getenv("HOME") : ".");
    return p;
}

static void trim(char *s) {
    char *a = s; while (*a && isspace((unsigned char)*a)) a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s); while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}
static int tsv_value(const char *path, const char *key, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        char k[128]; snprintf(k, sizeof k, "%s", line); trim(k);
        if (strcmp(k, key)) continue;
        char *v = tab + 1; trim(v);
        snprintf(out, n, "%.*s", (int)(n ? n - 1 : 0), v);
        fclose(f);
        return 0;
    }
    fclose(f);
    return -1;
}

static void settings_path(const game_t *g, char *out, size_t n) {
    snprintf(out, n, "%s/%s.conf", cfg_root(), g->id);
}
static void settings_load(game_t *g) {
    g->fps = 0; g->scale_index = 0; g->mute = 0;   // default: game's own frame rate, 1.0, sound on
    char p[1300], line[256];
    settings_path(g, p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char k[64], v[64];
        if (sscanf(line, "%63[^=]=%63s", k, v) == 2) {
            if (!strcmp(k, "fps")) g->fps = atoi(v);
            else if (!strcmp(k, "scale")) g->scale_index = atoi(v);
            else if (!strcmp(k, "mute")) g->mute = atoi(v);
        }
    }
    fclose(f);
}
static void settings_save(const game_t *g) {
    char p[1300];
    settings_path(g, p, sizeof p);
    FILE *f = fopen(p, "w");
    if (!f) { set_status("Cannot write %s", p); return; }
    fprintf(f, "fps=%d\nscale=%d\nmute=%d\n", g->fps, g->scale_index, g->mute);
    fclose(f);
    set_status("Settings saved");
}
static const char *fps_label(int v) {
    return v == 0 ? "Game's own" : v == 30 ? "30 fps" : v == 60 ? "60 fps" : "120 fps";
}
static const int fps_values[4] = {0, 30, 60, 120};
static const char *scale_label(int i) {
    static const char *l[5] = {"1.00 (native)", "0.90", "0.75", "0.67", "0.50"};
    return l[i < 0 ? 4 : i > 4 ? 4 : i];
}
static const float scale_values[5] = {1.0f, 0.9f, 0.75f, 0.67f, 0.5f};

static void add_game(const char *dir, const char *profile_dir) {
    char gp[1400]; snprintf(gp, sizeof gp, "%s/game.tsv", profile_dir);
    char id[64] = "", name[256] = "";
    if (tsv_value(gp, "title_id", id, sizeof id)) return;
    if (tsv_value(gp, "name", name, sizeof name)) snprintf(name, sizeof name, "%s", id);
    for (int i = 0; i < ngames; i++) if (!strcmp(games[i].id, id)) return;
    if (ngames == cgames) { cgames = cgames ? cgames * 2 : 64; games = realloc(games, (size_t)cgames * sizeof *games); }
    game_t *g = &games[ngames++]; memset(g, 0, sizeof *g);
    snprintf(g->id, sizeof g->id, "%s", id);
    snprintf(g->name, sizeof g->name, "%s", name);
    snprintf(g->dir, sizeof g->dir, "%s", dir);
    g->has_files = has_file(dir, "romfs") || has_file(dir, "main.patched.bin") || has_file(dir, "main.nso") || has_file(dir, "main");
    // The runtime for this profile: an explicitly provided one, a per-game
    // binary beside the launcher, or the generic runtime.
    const char *rt = getenv("NXNATIVE_RUNTIME");
    if (rt && *rt) snprintf(g->runtime, sizeof g->runtime, "%s", rt);
    else {
        char exe[1024]; ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) { exe[n] = 0; char *slash = strrchr(exe, '/'); if (slash) *slash = 0; }
        else snprintf(exe, sizeof exe, ".");
        const char *rd = getenv("NXNATIVE_RUNTIME_DIR");
        const char *cand[4];
        static char a[1200], b[1200], c[1200];
        snprintf(a, sizeof a, "%s/nxnative-%s", rd && *rd ? rd : exe, id);
        snprintf(b, sizeof b, "%s/nxnative-asahi", rd && *rd ? rd : exe);
        snprintf(c, sizeof c, "%s/nxnative-%s", exe, id);
        cand[0] = a; cand[1] = c; cand[2] = b; cand[3] = NULL;
        for (int k = 0; cand[k]; k++) if (access(cand[k], X_OK) == 0) { snprintf(g->runtime, sizeof g->runtime, "%s", cand[k]); break; }
        if (!g->runtime[0]) {
            const char *path = getenv("PATH"); char *copy = path ? strdup(path) : NULL;
            for (char *t = copy ? strtok(copy, ":") : NULL; t; t = strtok(NULL, ":")) {
                char p[1200]; snprintf(p, sizeof p, "%s/nxnative-%s", t, id);
                if (access(p, X_OK) == 0) { snprintf(g->runtime, sizeof g->runtime, "%s", p); break; }
                snprintf(p, sizeof p, "%s/nxnative-asahi", t);
                if (access(p, X_OK) == 0) { snprintf(g->runtime, sizeof g->runtime, "%s", p); break; }
            }
            free(copy);
        }
    }
    g->has_runtime = g->runtime[0] != 0;
    // A generic runtime is compiled for one profile. Ask it which one; a
    // per-game binary is trusted by its name.
    if (g->has_runtime) {
        const char *base = strrchr(g->runtime, '/');
        base = base ? base + 1 : g->runtime;
        if (!strncmp(base, "nxnative-asahi", 14)) {
            char cmd[1900], line[256] = "";
            snprintf(cmd, sizeof cmd, "%s profile 2>/dev/null", g->runtime);
            FILE *pipe = popen(cmd, "r");
            if (pipe) {
                char got[256];
                while (fgets(got, sizeof got, pipe)) {
                    int hex = 0;
                    while (hex < 16 && isxdigit((unsigned char)got[hex])) hex++;
                    if (hex != 16 || got[16] != '\t') continue;
                    got[16] = 0; snprintf(line, sizeof line, "%s", got); break;
                }
                pclose(pipe);
            }
            if (line[0] && strcmp(line, g->id)) {
                g->has_runtime = 0;
                snprintf(g->runtime_note, sizeof g->runtime_note, "runtime is for %s", line);
            }
        }
    }
    settings_load(g);
}

static int cmp_game(const void *a, const void *b) {
    const game_t *x = a, *y = b;
    return strcasecmp(x->name, y->name);
}

static void scan_dir(const char *root, int depth) {
    DIR *d = opendir(root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        if (!is_dir(p)) continue;
        if (has_file(p, "game.tsv")) {
            // A profile folder: the game files may be here or in <id>_build.
            char id[64] = "";
            char gp[1500]; snprintf(gp, sizeof gp, "%s/game.tsv", p);
            if (!tsv_value(gp, "title_id", id, sizeof id)) {
                char built[1600];
                snprintf(built, sizeof built, "%s/%s_build", root, id);
                add_game(is_dir(built) ? built : p, p);
            }
            continue;
        }
        if (depth < 2) scan_dir(p, depth + 1);
    }
    closedir(d);
}

static void scan_games(void) {
    ngames = 0;
    const char *roots = getenv("NXNATIVE_GAMES_DIR");
    if ((!roots || !*roots) && is_dir("dm/games")) roots = "dm/games";   // build tree, run from the repo root
    if (!roots || !*roots) roots = NXNATIVE_DEFAULT_GAMES_DIR;
    char *copy = roots && *roots ? strdup(roots) : NULL;
    for (char *t = copy ? strtok(copy, ":") : NULL; t; t = strtok(NULL, ":")) scan_dir(t, 0);
    free(copy);
    char data[1300]; snprintf(data, sizeof data, "%s/games", data_root());
    mkdirs(data);
    scan_dir(data, 0);
    qsort(games, (size_t)ngames, sizeof *games, cmp_game);
    if (selected >= ngames) selected = ngames ? ngames - 1 : 0;
}

static char **env_build(const game_t *g, char **buf, int *n) {
    *n = 0;
    char **v = calloc(256, sizeof *v);
    if (!v) return NULL;
    char romfs[1300], save[1300], temp[1300], shaders[1300];
    snprintf(romfs, sizeof romfs, "%s/romfs", g->dir);
    snprintf(save, sizeof save, "%s/save", g->dir);
    snprintf(temp, sizeof temp, "%s/temp", g->dir);
    snprintf(shaders, sizeof shaders, "%s/shaders", g->dir);
    mkdirs(save); mkdirs(temp);
    char tmp[1800]; snprintf(tmp, sizeof tmp, "DM_ROMFS=%s", romfs); v[(*n)++] = strdup(tmp);
    snprintf(tmp, sizeof tmp, "DM_SAVE=%s", save); v[(*n)++] = strdup(tmp);
    snprintf(tmp, sizeof tmp, "DM_TEMP=%s", temp); v[(*n)++] = strdup(tmp);
    if (is_dir(shaders)) {
        // Prefer a b-<chain> directory inside <game>/shaders; the runtime wants
        // the versioned directory itself.
        char use[1600]; snprintf(use, sizeof use, "%s", shaders);
        DIR *sd = opendir(shaders);
        if (sd) {
            struct dirent *de;
            while ((de = readdir(sd))) {
                if (de->d_name[0] != 'b' || de->d_name[1] != '-') continue;
                char cand[1800]; snprintf(cand, sizeof cand, "%s/%s", shaders, de->d_name);
                if (is_dir(cand)) { snprintf(use, sizeof use, "%s", cand); break; }
            }
            closedir(sd);
        }
        snprintf(tmp, sizeof tmp, "NX_NVN_SHADER_CACHE=%s", use); v[(*n)++] = strdup(tmp);
    }
    if (g->fps) { snprintf(tmp, sizeof tmp, "DM_FPS_TARGET=%d", g->fps); v[(*n)++] = strdup(tmp); }
    if (g->scale_index != 0) { snprintf(tmp, sizeof tmp, "DM_RENDER_SCALE=%.2f", scale_values[g->scale_index]); v[(*n)++] = strdup(tmp); }
    if (g->mute) v[(*n)++] = strdup("DM_AUDIO_MUTE=1");
    // Per-game overrides last: a packer or the player can set anything here.
    char envfile[1400]; snprintf(envfile, sizeof envfile, "%s/nxnative.env", g->dir);
    FILE *f = fopen(envfile, "r");
    if (f) {
        char line[2048];
        while (fgets(line, sizeof line, f) && *n < 250) {
            trim(line);
            if (!line[0] || line[0] == '#') continue;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            v[(*n)++] = strdup(line);
        }
        fclose(f);
    }
    if (buf) *buf = NULL;
    return v;
}
static void env_free(char **v) { if (!v) return; for (int i = 0; v[i]; i++) free(v[i]); free(v); }

static int launch(const game_t *g, int dry_run) {
    if (!g->has_runtime) { set_status("%s: build a runtime with GAME=%s", g->runtime_note[0] ? g->runtime_note : "No runtime", g->id); return -1; }
    char **env = NULL; int ne = 0;
    env = env_build(g, NULL, &ne);
    const char *argv[8]; int ac = 0;
    argv[ac++] = g->runtime;
    argv[ac++] = "run";
    argv[ac++] = g->dir;
    argv[ac++] = "main";
    argv[ac] = NULL;
    if (dry_run) {
        printf("game: %s (%s)\n", g->name, g->id);
        printf("dir: %s\nruntime: %s\ncommand:", g->dir, g->runtime);
        for (int i = 0; argv[i]; i++) printf(" %s", argv[i]);
        printf("\n");
        for (int i = 0; i < ne; i++) printf("env: %s\n", env[i]);
        env_free(env);
        return 0;
    }
    // Proxy environment entries as NAME=VALUE in the child.
    pid_t pid = fork();
    if (pid == 0) {
        for (int i = 0; i < ne; i++) putenv(env[i]);
        execv(g->runtime, (char *const *)argv);
        _exit(127);
    }
    if (pid < 0) { env_free(env); set_status("Cannot start the runtime: %s", strerror(errno)); return -1; }
    int st = 0;
    waitpid(pid, &st, 0);
    env_free(env);
    if (WIFEXITED(st) && WEXITSTATUS(st) == 127) { set_status("Cannot execute %s", g->runtime); return -1; }
    set_status("Game exited%s", WIFEXITED(st) ? "" : " (signal)");
    return 0;
}

// ---- save management ----
static int backup_save(const game_t *g) {
    const char *names[] = {"save", "save_device"};
    int made = 0;
    for (int i = 0; i < 2; i++) {
        char src[1400]; snprintf(src, sizeof src, "%s/%s", g->dir, names[i]);
        if (!is_dir(src)) continue;
        char dst[1500]; snprintf(dst, sizeof dst, "%s.backup-%ld", src, (long)time(NULL));
        char *argv[] = {"cp", "-a", src, dst, NULL};
        pid_t pid = fork();
        if (pid == 0) { execv("/usr/bin/cp", argv); execv("/bin/cp", argv); _exit(127); }
        int st = 0; waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) made++;
    }
    if (made) set_status("Save backed up (%d folder%s)", made, made == 1 ? "" : "s");
    else set_status("No save folder to back up");
    return made ? 0 : -1;
}
// Restore the newest .backup-* sibling of save/save_device. The current folder
// is removed only after a backup exists; both commands stay inside the game
// directory.
static int restore_save(const game_t *g) {
    const char *names[] = {"save", "save_device"};
    int restored = 0;
    for (int i = 0; i < 2; i++) {
        char dir[1400]; snprintf(dir, sizeof dir, "%s", g->dir);
        char prefix[64]; snprintf(prefix, sizeof prefix, "%s.backup-", names[i]);
        DIR *d = opendir(dir);
        if (!d) continue;
        char best[1600] = "";
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, prefix, strlen(prefix))) continue;
            if (strcmp(e->d_name, best) > 0) snprintf(best, sizeof best, "%s", e->d_name);
        }
        closedir(d);
        if (!best[0]) continue;
        char src[1800], dst[1600];
        snprintf(src, sizeof src, "%s/%s", dir, best);
        snprintf(dst, sizeof dst, "%s/%s", dir, names[i]);
        char *rmv[] = {"rm", "-rf", dst, NULL};
        pid_t pid = fork();
        if (pid == 0) { execv("/usr/bin/rm", rmv); execv("/bin/rm", rmv); _exit(127); }
        int st = 0; waitpid(pid, &st, 0);
        char *cpv[] = {"cp", "-a", src, dst, NULL};
        pid = fork();
        if (pid == 0) { execv("/usr/bin/cp", cpv); execv("/bin/cp", cpv); _exit(127); }
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) restored++;
    }
    set_status(restored ? "Newest save backup restored" : "No save backup to restore");
    return restored ? 0 : -1;
}

static int delete_save(const game_t *g) {
    const char *names[] = {"save", "save_device"};
    int made = 0;
    for (int i = 0; i < 2; i++) {
        char src[1400]; snprintf(src, sizeof src, "%s/%s", g->dir, names[i]);
        if (!is_dir(src)) continue;
        char *argv[] = {"rm", "-rf", src, NULL};
        pid_t pid = fork();
        if (pid == 0) { execv("/usr/bin/rm", argv); execv("/bin/rm", argv); _exit(127); }
        int st = 0; waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) made++;
    }
    set_status(made ? "Save deleted" : "No save folder to delete");
    return 0;
}

// ---- drawing ----
static void rect(SDL_Renderer *r, float x, float y, float w, float h, uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
    SDL_FRect q = {x, y, w, h};
    p_SDL_SetRenderDrawColor(r, cr, cg, cb, ca);
    p_SDL_RenderFillRect(r, &q);
}
static void text(SDL_Renderer *r, float x, float y, float scale, uint8_t cr, uint8_t cg, uint8_t cb, const char *s) {
    p_SDL_SetRenderScale(r, scale, scale);
    p_SDL_SetRenderDrawColor(r, cr, cg, cb, 255);
    p_SDL_RenderDebugText(r, x / scale, y / scale, s);
}

static const char *state_label(const game_t *g) {
    if (g->runtime_note[0]) return g->runtime_note;
    if (!g->has_runtime) return "runtime not built";
    if (!g->has_files) return "game files not found";
    return "ready";
}

static void draw_list(SDL_Renderer *r, int w, int h) {
    rect(r, 0, 0, (float)w, (float)h, 22, 24, 32, 255);
    rect(r, 0, 0, (float)w, 68, 30, 34, 46, 255);
    text(r, 24, 16, 2.0f, 235, 238, 250, "nxnative");
    text(r, 220, 24, 1.2f, 150, 160, 180, "native Switch games on Asahi Linux");
    text(r, (float)w - 190, 24, 1.0f, 140, 150, 170, "F1 settings   F2 saves");
    float top = 84, row = 44;
    int visible = (int)((h - top - 110) / row);
    if (visible < 1) visible = 1;
    int first = selected >= visible ? selected - visible + 1 : 0;
    for (int i = 0; i < visible && first + i < ngames; i++) {
        game_t *g = &games[first + i];
        float y = top + row * (float)i;
        if (first + i == selected) rect(r, 16, y, (float)w - 32, row - 4, 52, 82, 124, 255);
        else rect(r, 16, y, (float)w - 32, row - 4, 32, 36, 48, 255);
        text(r, 28, y + 10, 1.4f, 235, 238, 250, g->name);
        uint8_t sr = 230, sg = 170, sb = 120;
        if (g->has_runtime && g->has_files) { sr = 140; sg = 220; sb = 150; }
        text(r, (float)w - 250, y + 12, 1.0f, sr, sg, sb, state_label(g));
    }
    if (!ngames) {
        text(r, 28, top + 10, 1.4f, 230, 180, 120, "No game profiles found.");
        text(r, 28, top + 40, 1.0f, 160, 170, 190, "Set NXNATIVE_GAMES_DIR to a folder of profiles / prepared games.");
    }
    rect(r, 0, (float)h - 96, (float)w, 96, 30, 34, 46, 255);
    if (ngames) {
        game_t *g = &games[selected];
        text(r, 24, (float)h - 78, 1.0f, 170, 180, 200, g->id);
        text(r, 24, (float)h - 56, 1.0f, 170, 180, 200, g->dir);
        text(r, (float)w - 300, (float)h - 78, 1.0f, 170, 180, 200, g->runtime[0] ? g->runtime : "no runtime found");
    }
    text(r, 24, (float)h - 28, 1.0f, 130, 140, 160, "Up/Down select   Enter play   F1 settings   F2 saves   R rescan   Q quit");
    if (status[0]) text(r, (float)w - 340, (float)h - 28, 1.0f, 240, 200, 140, status);
}

static void draw_settings(SDL_Renderer *r, int w, int h) {
    rect(r, 0, 0, (float)w, (float)h, 22, 24, 32, 255);
    rect(r, 0, 0, (float)w, 68, 30, 34, 46, 255);
    text(r, 24, 16, 2.0f, 235, 238, 250, "Settings");
    if (!ngames) return;
    game_t *g = &games[selected];
    text(r, 24, 84, 1.2f, 150, 160, 180, g->name);
    const char *rows[3], *vals[3];
    rows[0] = "Frame rate"; vals[0] = fps_label(g->fps);
    rows[1] = "Render scale"; vals[1] = scale_label(g->scale_index);
    rows[2] = "Mute audio"; vals[2] = g->mute ? "on" : "off";
    for (int i = 0; i < 3; i++) {
        float y = 150 + 56 * (float)i;
        if (i == settings_row) rect(r, 20, y - 8, (float)w - 40, 48, 52, 82, 124, 255);
        text(r, 36, y, 1.4f, 235, 238, 250, rows[i]);
        text(r, (float)w - 300, y, 1.4f, 200, 220, 240, vals[i]);
    }
    text(r, 24, (float)h - 60, 1.0f, 130, 140, 160, "Up/Down select   Left/Right change   S save   Esc back");
    if (status[0]) text(r, 24, (float)h - 34, 1.0f, 240, 200, 140, status);
}

static void draw_saves(SDL_Renderer *r, int w, int h) {
    rect(r, 0, 0, (float)w, (float)h, 22, 24, 32, 255);
    rect(r, 0, 0, (float)w, 68, 30, 34, 46, 255);
    text(r, 24, 16, 2.0f, 235, 238, 250, "Saves");
    if (!ngames) return;
    game_t *g = &games[selected];
    text(r, 24, 84, 1.2f, 150, 160, 180, g->name);
    float y = 140;
    const char *names[] = {"save", "save_device"};
    for (int i = 0; i < 2; i++) {
        char p[1400]; snprintf(p, sizeof p, "%s/%s", g->dir, names[i]);
        text(r, 36, y, 1.2f, 220, 225, 240, names[i]);
        text(r, 260, y, 1.2f, is_dir(p) ? 150 : 120, is_dir(p) ? 220 : 140, is_dir(p) ? 160 : 160, is_dir(p) ? p : "not created yet");
        y += 40;
    }
    text(r, 24, (float)h - 90, 1.0f, 130, 140, 160, "Saves live in the prepared game folder, beside romfs and the modules.");
    text(r, 24, (float)h - 60, 1.0f, 130, 140, 160, "B backup   D delete (press twice)   Esc back");
    if (status[0]) text(r, 24, (float)h - 34, 1.0f, 240, 200, 140, status);
}

static int run_ui(void) {
    sdl = dlopen("libSDL3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!sdl) sdl = dlopen("libSDL3.so", RTLD_NOW | RTLD_LOCAL);
    if (!sdl) { fprintf(stderr, "SDL3 runtime not found: install the sdl3 package\n"); return 1; }
    LOAD(SDL_CreateWindow); LOAD(SDL_DestroyWindow); LOAD(SDL_CreateRenderer); LOAD(SDL_DestroyRenderer);
    LOAD(SDL_SetRenderDrawColor); LOAD(SDL_RenderClear); LOAD(SDL_RenderFillRect); LOAD(SDL_RenderRect);
    LOAD(SDL_RenderDebugText); LOAD(SDL_RenderPresent); LOAD(SDL_SetRenderScale); LOAD(SDL_PollEvent);
    LOAD(SDL_GetKeyboardState); LOAD(SDL_InitSubSystem); LOAD(SDL_Quit); LOAD(SDL_GetError);
    LOAD(SDL_Delay); LOAD(SDL_ShowSimpleMessageBox); LOAD(SDL_GetWindowSizeInPixels);
    if (!p_SDL_CreateWindow || !p_SDL_CreateRenderer || !p_SDL_RenderDebugText) {
        fprintf(stderr, "SDL3 is missing the renderer API\n"); return 1;
    }
    if (!p_SDL_InitSubSystem(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL video: %s\n", p_SDL_GetError()); return 1; }
    SDL_Window *win = p_SDL_CreateWindow("nxnative", 980, 640, 0x00000020ull /* SDL_WINDOW_RESIZABLE */);
    if (!win) { fprintf(stderr, "window: %s\n", p_SDL_GetError()); return 1; }
    SDL_Renderer *ren = p_SDL_CreateRenderer(win, NULL);
    if (!ren) { fprintf(stderr, "renderer: %s\n", p_SDL_GetError()); return 1; }
    static uint8_t prev[512];
    int running = 1, quit = 0, frames = 0;
    int self_test = getenv("NXNATIVE_LAUNCHER_SELFTEST") != NULL;
    int delete_armed = 0;
    while (running) {
        int nk = 0;
        const uint8_t *ks = p_SDL_GetKeyboardState(&nk);
        SDL_Event ev;
        while (p_SDL_PollEvent(&ev)) {}
        #define PRESS(k) (nk > (k) && ks[(k)] && !prev[(k)])
        int up = PRESS(SDL_SCANCODE_UP) || PRESS(SDL_SCANCODE_W);
        int down = PRESS(SDL_SCANCODE_DOWN) || PRESS(SDL_SCANCODE_S);
        int left = PRESS(SDL_SCANCODE_LEFT) || PRESS(SDL_SCANCODE_A);
        int right = PRESS(SDL_SCANCODE_RIGHT) || PRESS(SDL_SCANCODE_D);
        int enter = PRESS(SDL_SCANCODE_RETURN) || PRESS(SDL_SCANCODE_SPACE);
        int esc = PRESS(SDL_SCANCODE_ESCAPE);
        int f1 = PRESS(SDL_SCANCODE_F1), f2 = PRESS(SDL_SCANCODE_F2), f5 = PRESS(SDL_SCANCODE_F5);
        int q = PRESS(SDL_SCANCODE_Q);
        int b = PRESS(SDL_SCANCODE_B), del = PRESS(SDL_SCANCODE_D), rkey = PRESS(SDL_SCANCODE_R);
        if (nk > 0 && nk < (int)sizeof prev) memcpy(prev, ks, (size_t)nk);
        if (view == 0) {
            if (ngames) {
                if (up) selected = (selected + ngames - 1) % ngames;
                if (down) selected = (selected + 1) % ngames;
                if (enter) { launch(&games[selected], 0); }
            }
            if (f1) { view = 1; settings_row = 0; status[0] = 0; }
            if (f2) { view = 2; status[0] = 0; delete_armed = 0; }
            if (f5) { scan_games(); set_status("Rescanned: %d game%s", ngames, ngames == 1 ? "" : "s"); }
            if (q || esc) running = 0;
        } else if (view == 1) {
            if (ngames) {
                if (up) settings_row = (settings_row + 2) % 3;
                if (down) settings_row = (settings_row + 1) % 3;
                game_t *g = &games[selected];
                int dir = right ? 1 : left ? -1 : 0;
                if (dir) {
                    if (settings_row == 0) {
                        int k = 0; while (k < 3 && fps_values[k] != g->fps) k++;
                        k = (k + dir + 4) % 4; g->fps = fps_values[k];
                    } else if (settings_row == 1) {
                        g->scale_index = (g->scale_index + dir + 5) % 5;
                    } else g->mute = !g->mute;
                }
            }
            if (PRESS(SDL_SCANCODE_S)) settings_save(&games[selected]);
            if (esc || f1) view = 0;
        } else {
            if (b && ngames) backup_save(&games[selected]);
            if (rkey && ngames) restore_save(&games[selected]);
            if (del && ngames) {
                if (!delete_armed) { delete_armed = 1; set_status("Press D again to delete the save folders"); }
                else { delete_save(&games[selected]); delete_armed = 0; }
            }
            if (esc || f2) view = 0;
        }
        int w = 980, h = 640;
        if (p_SDL_GetWindowSizeInPixels) p_SDL_GetWindowSizeInPixels(win, &w, &h);
        if (w <= 0 || h <= 0) { p_SDL_Delay(16); continue; }
        p_SDL_SetRenderScale(ren, 1, 1);
        if (view == 0) draw_list(ren, w, h);
        else if (view == 1) draw_settings(ren, w, h);
        else draw_saves(ren, w, h);
        p_SDL_RenderPresent(ren);
        frames++;
        if (self_test && frames >= 2) running = 0;
        p_SDL_Delay(16);
    }
    p_SDL_DestroyRenderer(ren);
    p_SDL_DestroyWindow(win);
    return quit ? 1 : 0;
}

static void usage(const char *a0) {
    printf("nxnative-launcher %s\n"
           "usage: %s [--list] [--dry-run <title-id>]\n"
           "  --list               print the games found and exit\n"
           "  --dry-run <id>       print the launch command and environment\n"
           "  --self-test          draw two frames and exit (for CI; use SDL_VIDEODRIVER=dummy)\n"
           "  --backup <id>         back up the game's save folders and exit\n"
           "  --delete-save <id>    delete the game's save folders and exit\n"
           "  --restore-save <id>   restore the newest backup and exit\n"
           "environment: NXNATIVE_GAMES_DIR (colon separated), NXNATIVE_RUNTIME,\n"
           "             NXNATIVE_RUNTIME_DIR\n", NXNATIVE_VERSION, a0);
}

int main(int argc, char **argv) {
    const char *def = NXNATIVE_DEFAULT_GAMES_DIR;
    snprintf(root_default, sizeof root_default, "%s", def);
    if (!getenv("NXNATIVE_GAMES_DIR") && def[0]) setenv("NXNATIVE_GAMES_DIR", def, 0);
    scan_games();
    if (argc > 1 && !strcmp(argv[1], "--help")) { usage(argv[0]); return 0; }
    if (argc > 1 && !strcmp(argv[1], "--list")) {
        for (int i = 0; i < ngames; i++) printf("%s\t%s\t%s\t%s\n", games[i].id, games[i].has_files ? "files" : "-", games[i].has_runtime ? games[i].runtime : "-", games[i].name);
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "--dry-run")) {
        for (int i = 0; i < ngames; i++) if (!strcmp(games[i].id, argv[2])) {
            int rc = launch(&games[i], 1);
            if (rc) fprintf(stderr, "%s\n", status);
            return rc ? 1 : 0;
        }
        fprintf(stderr, "no game with title id %s\n", argv[2]); return 1;
    }
    if (argc > 1 && !strcmp(argv[1], "--self-test")) {
        setenv("NXNATIVE_LAUNCHER_SELFTEST", "1", 1);
        return run_ui();
    }
    if (argc > 2 && (!strcmp(argv[1], "--backup") || !strcmp(argv[1], "--delete-save") || !strcmp(argv[1], "--restore-save"))) {
        for (int i = 0; i < ngames; i++) if (!strcmp(games[i].id, argv[2])) {
            int rc = argv[1][2] == 'b' ? backup_save(&games[i]) : argv[1][2] == 'd' ? delete_save(&games[i]) : restore_save(&games[i]);
            printf("%s\n", status);
            return rc == 0 ? 0 : 1;
        }
        fprintf(stderr, "no game with title id %s\n", argv[2]); return 1;
    }
    if (!isatty(0) && !getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
        fprintf(stderr, "no graphical session (set DISPLAY or WAYLAND_DISPLAY), or use --list\n");
        return 1;
    }
    return run_ui();
}
