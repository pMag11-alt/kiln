// sh: the kiln shell.
//
//   cmd args...            run /bin/cmd (or a path)
//   a | b | c              pipelines, each stage in one process group
//   < in   > out   >> out  redirection; also 2> err and 2>&1
//   a && b   a || b        conditional execution on exit status
//   a ; b    a &           sequencing and background jobs
//   'x y'  "x $? y"  \c    quoting; $? is the last status, $$ the shell pid
//   *.c  file?             globbing in the last path component
//   # comment
//
// Builtins: cd, pwd, exit, wait, jobs, help. With a file argument (or -c
// "commands") the shell runs a script instead of prompting.
#include "user.h"

#define MAXARGS   64
#define MAXCMDS   16
#define MAXTOKENS 256
#define LINE_MAX  1024

enum tok { T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_BG, T_IN, T_OUT, T_APPEND, T_ERR, T_ERR2OUT, T_END };

struct token {
  enum tok type;
  char *word;
  bool glob;   // the word contained unquoted * or ?
};

struct cmd {
  char *argv[MAXARGS + 1];
  int argc;
  char *in, *out, *err;
  bool append, err2out;
};

static bool interactive;
static int last_status;
static pid_t jobs[32];
static int njobs;

// ---- tokenizer ----

// $? and $$ become these markers when a line is tokenized and are expanded
// just before each command runs, so "false; echo $?" sees the new status.
#define VAR_STATUS '\x01'
#define VAR_PID    '\x02'

static char store[LINE_MAX * 4];
static size_t store_len;

static char *store_word(const char *s, size_t n) {
  if (store_len + n + 1 > sizeof(store)) return NULL;
  char *w = store + store_len;
  memcpy(w, s, n);
  w[n] = '\0';
  store_len += n + 1;
  return w;
}

// Split line into tokens, processing quotes and $-expansions. Returns the
// number of tokens (a T_END is always appended) or -1 on a syntax error.
static int tokenize(const char *line, struct token *toks, int max) {
  int n = 0;
  store_len = 0;
  const char *p = line;
  char word[LINE_MAX];
  for (;;) {
    while (*p == ' ' || *p == '\t') p++;
    if (n >= max - 1) {
      dprintf(2, "sh: too many tokens\n");
      return -1;
    }
    if (*p == '\0' || *p == '#') break;
    struct token *t = &toks[n];
    t->word = NULL;
    t->glob = false;
    if (p[0] == '|' && p[1] == '|') { t->type = T_OR; p += 2; }
    else if (*p == '|') { t->type = T_PIPE; p++; }
    else if (p[0] == '&' && p[1] == '&') { t->type = T_AND; p += 2; }
    else if (*p == '&') { t->type = T_BG; p++; }
    else if (*p == ';') { t->type = T_SEMI; p++; }
    else if (*p == '<') { t->type = T_IN; p++; }
    else if (p[0] == '>' && p[1] == '>') { t->type = T_APPEND; p += 2; }
    else if (*p == '>') { t->type = T_OUT; p++; }
    else if (p[0] == '2' && p[1] == '>' && p[2] == '&' && p[3] == '1') { t->type = T_ERR2OUT; p += 4; }
    else if (p[0] == '2' && p[1] == '>') { t->type = T_ERR; p += 2; }
    else {
      size_t len = 0;
      t->type = T_WORD;
      while (*p && !strchr(" \t|&;<>", *p)) {
        if (len >= sizeof(word) - 32) {
          dprintf(2, "sh: word too long\n");
          return -1;
        }
        if (*p == '\'') {
          p++;
          while (*p && *p != '\'') word[len++] = *p++;
          if (*p != '\'') { dprintf(2, "sh: unterminated '\n"); return -1; }
          p++;
        } else if (*p == '"') {
          p++;
          while (*p && *p != '"') {
            if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) {
              word[len++] = p[1];
              p += 2;
            } else if (*p == '$' && (p[1] == '?' || p[1] == '$')) {
              word[len++] = p[1] == '?' ? VAR_STATUS : VAR_PID;
              p += 2;
            } else {
              word[len++] = *p++;
            }
          }
          if (*p != '"') { dprintf(2, "sh: unterminated \"\n"); return -1; }
          p++;
        } else if (*p == '\\' && p[1]) {
          word[len++] = p[1];
          p += 2;
        } else if (*p == '$' && (p[1] == '?' || p[1] == '$')) {
          word[len++] = p[1] == '?' ? VAR_STATUS : VAR_PID;
          p += 2;
        } else {
          if (*p == '*' || *p == '?') t->glob = true;
          word[len++] = *p++;
        }
      }
      t->word = store_word(word, len);
      if (!t->word) {
        dprintf(2, "sh: line too long\n");
        return -1;
      }
    }
    n++;
  }
  toks[n].type = T_END;
  toks[n].word = NULL;
  return n + 1;
}

// ---- globbing ----

static bool match(const char *pat, const char *s) {
  for (; *pat; pat++, s++) {
    if (*pat == '*') {
      while (pat[1] == '*') pat++;
      if (!pat[1]) return true;
      for (; *s; s++)
        if (match(pat + 1, s)) return true;
      return false;
    }
    if (!*s || (*pat != '?' && *pat != *s)) return false;
  }
  return !*s;
}

static int cmp_str(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

// Expand a pattern in its last path component. Returns how many words were
// added; 0 means no match (the caller keeps the word literally).
static int glob_expand(const char *word, char **argv, int room) {
  const char *slash = strrchr(word, '/');
  char dir[PATH_MAX];
  const char *pat = word;
  size_t dlen = 0;
  if (slash) {
    dlen = (size_t)(slash - word) + 1;
    if (dlen >= sizeof(dir)) return 0;
    memcpy(dir, word, dlen);
    dir[dlen] = '\0';
    pat = slash + 1;
  } else {
    strcpy(dir, "./");
  }
  if (strchr(dir, '*') || strchr(dir, '?')) return 0;   // only the last component
  DIR *d = opendir(dir);
  if (!d) return 0;
  int added = 0;
  struct dirent *e;
  while ((e = readdir(d)) && added < room) {
    if (e->d_name[0] == '.' && pat[0] != '.') continue;   // hidden files need an explicit dot
    if (!match(pat, e->d_name)) continue;
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%.*s%s", (int)dlen, word, e->d_name);
    char *w = store_word(full, strlen(full));
    if (!w) break;
    argv[added++] = w;
  }
  closedir(d);
  qsort(argv, (size_t)added, sizeof(char *), cmp_str);
  return added;
}

// Replace $? / $$ markers in a word with their current values.
static char *expand_vars(char *w) {
  if (!strchr(w, VAR_STATUS) && !strchr(w, VAR_PID)) return w;
  char out[LINE_MAX];
  size_t n = 0;
  for (char *p = w; *p && n < sizeof(out) - 16; p++) {
    if (*p == VAR_STATUS) n += (size_t)snprintf(out + n, 16, "%d", last_status);
    else if (*p == VAR_PID) n += (size_t)snprintf(out + n, 16, "%d", getpid());
    else out[n++] = *p;
  }
  char *e = store_word(out, n);
  return e ? e : w;
}

// ---- parser ----

// Parse one simple command starting at toks[*i].
static int parse_cmd(struct token *toks, int *i, struct cmd *c) {
  memset(c, 0, sizeof(*c));
  for (;;) {
    struct token *t = &toks[*i];
    if (t->type == T_WORD) {
      t->word = expand_vars(t->word);
      if (t->glob) {
        int k = glob_expand(t->word, c->argv + c->argc, MAXARGS - c->argc);
        if (k > 0) {
          c->argc += k;
          (*i)++;
          continue;
        }
      }
      if (c->argc >= MAXARGS) {
        dprintf(2, "sh: too many arguments\n");
        return -1;
      }
      c->argv[c->argc++] = t->word;
      (*i)++;
    } else if (t->type == T_IN || t->type == T_OUT || t->type == T_APPEND || t->type == T_ERR) {
      struct token *f = &toks[*i + 1];
      if (f->type != T_WORD) {
        dprintf(2, "sh: syntax error: redirection needs a file name\n");
        return -1;
      }
      f->word = expand_vars(f->word);
      if (t->type == T_IN) c->in = f->word;
      else if (t->type == T_ERR) c->err = f->word;
      else {
        c->out = f->word;
        c->append = t->type == T_APPEND;
      }
      *i += 2;
    } else if (t->type == T_ERR2OUT) {
      c->err2out = true;
      (*i)++;
    } else {
      break;
    }
  }
  c->argv[c->argc] = NULL;
  if (c->argc == 0) {
    dprintf(2, "sh: syntax error near '%s'\n",
            toks[*i].type == T_END ? "end of line" : toks[*i].type == T_PIPE ? "|" : "operator");
    return -1;
  }
  return 0;
}

// ---- builtins ----

static void reap_jobs(bool report) {
  for (int j = 0; j < njobs;) {
    int st;
    pid_t w = waitpid(jobs[j], &st, WNOHANG);
    if (w == jobs[j] || w < 0) {
      if (report && w > 0) printf("[%d] done\n", w);
      jobs[j] = jobs[--njobs];
    } else {
      j++;
    }
  }
}

static bool is_builtin(const char *name) {
  static const char *names[] = {"cd", "pwd", "exit", "wait", "jobs", "help"};
  for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    if (strcmp(name, names[i]) == 0) return true;
  return false;
}

static int builtin(struct cmd *c) {
  const char *name = c->argv[0];
  if (strcmp(name, "cd") == 0) {
    const char *dir = c->argc > 1 ? c->argv[1] : "/";
    if (chdir(dir) < 0) {
      dprintf(2, "cd: %s: %s\n", dir, strerror(errno));
      return 1;
    }
    return 0;
  }
  if (strcmp(name, "pwd") == 0) {
    char buf[PATH_MAX];
    if (!getcwd(buf, sizeof(buf))) {
      perror("pwd");
      return 1;
    }
    printf("%s\n", buf);
    return 0;
  }
  if (strcmp(name, "exit") == 0) exit(c->argc > 1 ? atoi(c->argv[1]) : last_status);
  if (strcmp(name, "wait") == 0) {
    for (int j = 0; j < njobs; j++) waitpid(jobs[j], NULL, 0);
    njobs = 0;
    return 0;
  }
  if (strcmp(name, "jobs") == 0) {
    reap_jobs(false);
    for (int j = 0; j < njobs; j++) printf("[%d] running\n", jobs[j]);
    return 0;
  }
  printf("builtins: cd [dir], pwd, exit [n], wait, jobs, help\n"
         "programs: ls /bin\n"
         "syntax:   a | b, < > >> 2> 2>&1, a && b, a || b, a ; b, a &, 'q' \"q\", *.c, $?\n");
  return 0;
}

// ---- execution ----

static const char *signame(int sig) {
  switch (sig) {
  case SIGINT: return "Interrupt";
  case SIGILL: return "Illegal instruction";
  case SIGABRT: return "Aborted";
  case SIGBUS: return "Bus error";
  case SIGKILL: return "Killed";
  case SIGSEGV: return "Segmentation fault";
  case SIGPIPE: return "Broken pipe";
  case SIGTERM: return "Terminated";
  }
  return "Killed by signal";
}

static int open_redirect(const char *path, int flags, int target) {
  int fd = open(path, flags, 0644);
  if (fd < 0) {
    dprintf(2, "sh: %s: %s\n", path, strerror(errno));
    return -1;
  }
  if (fd != target) {
    dup2(fd, target);
    close(fd);
  }
  return 0;
}

static void apply_redirects(struct cmd *c) {
  if (c->in && open_redirect(c->in, O_RDONLY, 0) < 0) exit(1);
  if (c->out && open_redirect(c->out, O_WRONLY | O_CREAT | (c->append ? O_APPEND : O_TRUNC), 1) < 0) exit(1);
  if (c->err && open_redirect(c->err, O_WRONLY | O_CREAT | O_TRUNC, 2) < 0) exit(1);
  if (c->err2out) dup2(1, 2);
}

static int status_of(int st) {
  return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

static int run_pipeline(struct cmd *cmds, int n, bool bg) {
  // A lone builtin runs in the shell itself so cd and exit take effect.
  if (n == 1 && !bg && is_builtin(cmds[0].argv[0]) && !cmds[0].in && !cmds[0].out &&
      !cmds[0].err) {
    return builtin(&cmds[0]);
  }

  pid_t pids[MAXCMDS];
  pid_t pgid = 0;
  int prev = -1;
  for (int i = 0; i < n; i++) {
    int fds[2] = {-1, -1};
    if (i + 1 < n && pipe(fds) < 0) {
      perror("sh: pipe");
      return 1;
    }
    pid_t pid = fork();
    if (pid < 0) {
      perror("sh: fork");
      return 1;
    }
    if (pid == 0) {
      setpgid(0, pgid);
      if (prev >= 0) {
        dup2(prev, 0);
        close(prev);
      }
      if (fds[1] >= 0) {
        dup2(fds[1], 1);
        close(fds[1]);
        close(fds[0]);
      }
      apply_redirects(&cmds[i]);
      if (is_builtin(cmds[i].argv[0])) exit(builtin(&cmds[i]));
      execvp(cmds[i].argv[0], cmds[i].argv);
      if (errno == ENOENT) dprintf(2, "sh: %s: command not found\n", cmds[i].argv[0]);
      else dprintf(2, "sh: %s: %s\n", cmds[i].argv[0], strerror(errno));
      exit(127);
    }
    if (!pgid) pgid = pid;
    setpgid(pid, pgid);
    if (prev >= 0) close(prev);
    if (fds[1] >= 0) close(fds[1]);
    prev = fds[0];
    pids[i] = pid;
  }

  if (bg) {
    if (njobs < (int)(sizeof(jobs) / sizeof(jobs[0]))) jobs[njobs++] = pids[n - 1];
    for (int i = 0; i + 1 < n; i++) {
      if (njobs < (int)(sizeof(jobs) / sizeof(jobs[0]))) jobs[njobs++] = pids[i];
    }
    printf("[%d]\n", pids[n - 1]);
    return 0;
  }

  if (interactive) tcsetpgrp(pgid);
  int status = 0;
  for (int i = 0; i < n; i++) {
    int st = 0;
    while (waitpid(pids[i], &st, 0) < 0 && errno == EINTR) {}
    if (i == n - 1) status = st;
  }
  if (interactive) tcsetpgrp(0);

  if (WIFSIGNALED(status) && WTERMSIG(status) != SIGPIPE) {
    if (WTERMSIG(status) == SIGINT) {
      // The console already echoed ^C.
    } else {
      dprintf(2, "%s\n", signame(WTERMSIG(status)));
    }
  }
  return status_of(status);
}

// Run one line: sequences of and-or lists of pipelines.
static void run_line(const char *line) {
  static struct token toks[MAXTOKENS];
  static struct cmd cmds[MAXCMDS];
  int ntok = tokenize(line, toks, MAXTOKENS);
  if (ntok < 0) {
    last_status = 2;
    return;
  }
  int i = 0;
  while (toks[i].type != T_END) {
    // One and-or list: pipeline (&& | ||) pipeline ...
    int status = last_status;
    enum tok op = T_SEMI;
    for (;;) {
      int n = 0;
      for (;;) {
        if (n >= MAXCMDS) {
          dprintf(2, "sh: pipeline too long\n");
          last_status = 2;
          return;
        }
        if (parse_cmd(toks, &i, &cmds[n]) < 0) {
          last_status = 2;
          return;
        }
        n++;
        if (toks[i].type != T_PIPE) break;
        i++;
      }
      // Background applies to the whole and-or list; peek for it.
      int j = i;
      while (toks[j].type == T_AND || toks[j].type == T_OR || toks[j].type == T_WORD ||
             toks[j].type == T_PIPE || (toks[j].type >= T_IN && toks[j].type <= T_ERR2OUT))
        j++;
      bool bg = toks[j].type == T_BG;
      bool run = op == T_SEMI || (op == T_AND && status == 0) || (op == T_OR && status != 0);
      if (run) status = run_pipeline(cmds, n, bg);
      if (toks[i].type == T_AND || toks[i].type == T_OR) {
        op = toks[i].type;
        i++;
        continue;
      }
      break;
    }
    last_status = status;
    if (toks[i].type == T_SEMI || toks[i].type == T_BG) i++;
    else if (toks[i].type != T_END) {
      dprintf(2, "sh: syntax error\n");
      last_status = 2;
      return;
    }
  }
}

static void prompt(void) {
  char cwd[PATH_MAX];
  if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
  printf("kiln:%s$ ", cwd);
  fflush(1);
}

int main(int argc, char *argv[]) {
  struct reader in;
  char line[LINE_MAX];

  if (argc >= 3 && strcmp(argv[1], "-c") == 0) {
    run_line(argv[2]);
    exit(last_status);
  }
  int fd = 0;
  if (argc >= 2) {
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) die("sh: %s: %s", argv[1], strerror(errno));
  }
  interactive = fd == 0 && isatty(0);
  if (interactive) {
    setpgid(0, 0);
    tcsetpgrp(0);   // ^C at the prompt only clears the line
  }

  reader_init(&in, fd);
  for (;;) {
    if (interactive) {
      reap_jobs(true);
      prompt();
    }
    if (reader_getline(&in, line, sizeof(line)) < 0) break;
    run_line(line);
  }
  if (interactive) printf("\n");
  exit(last_status);
}
