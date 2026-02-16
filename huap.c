#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <utime.h>
#include <unistd.h>

#include "vendor/md4c/md4c-html.h"
#include "vendor/mongoose/mongoose.h"

#define BODY_PH "{{Body}}"
#define SIDENOTE_S "[sidenote]"
#define SIDENOTE_E "[/sidenote]"
#define SIDENOTE_R_S "<div class=\"sidenote\">"
#define SIDENOTE_R_E "</div>"
#define CODE_CMD "$code "
#define SNIPPET_S "//snippet "
#define SNIPPET_E "//endsnippet"

/* ---------------- Small arena (per-job) ---------------- */

#define ALIGN_UP(n,a) (((n)+((a)-1)) & ~((a)-1))
#define ARENA_ALIGN(n) ALIGN_UP((n), sizeof(void*))

typedef struct {
	uint8_t *base;
	size_t cap;
	size_t perm_p;
	size_t temp_p;
} Arena;

static void arena_init(Arena *a, size_t cap){
	a->base = malloc(cap);
	if(!a->base){ perror("malloc"); exit(1); }
	a->cap = cap; a->perm_p = 0; a->temp_p = cap;
}
static void *arena_alloc_perm(Arena *a, size_t n){
	n = ARENA_ALIGN(n);
	if(n > a->temp_p - a->perm_p) return NULL;
	void *p = a->base + a->perm_p; a->perm_p += n; return p;
}
static void *arena_alloc_temp(Arena *a, size_t n){
	n = ARENA_ALIGN(n);
	if(n > a->temp_p - a->perm_p) return NULL;
	a->temp_p -= n; return a->base + a->temp_p;
}
static void arena_reset_temp(Arena *a){ a->temp_p = a->cap; }
static void arena_destroy(Arena *a){ free(a->base); }

/* ---------------- Growable buffer ---------------- */

typedef struct { 
	char *p; 
	size_t len;
	size_t cap; 
} Buf;

static int buf_grow(Buf *b, size_t add){
	size_t need = b->len + add + 1;
	if(need <= b->cap) return 0;
	size_t ncap = b->cap ? b->cap : 4096;
	while(ncap < need) ncap *= 2;
	char *np = realloc(b->p, ncap);
	if(!np) return -1;
	b->p = np; b->cap = ncap;
	return 0;
}
static int buf_putn(Buf *b, const void *s, size_t n){
	if(buf_grow(b, n) != 0) return -1;
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = '\0';
	return 0;
}
static int buf_puts(Buf *b, const char *s){ return buf_putn(b, s, strlen(s)); }

/* ---------------- Path helpers ---------------- */

static int has_ext(const char *name, const char *ext){
	size_t n = strlen(name), e = strlen(ext);
	return n >= e && strcmp(name + (n - e), ext) == 0;
}
static int path_has_extension(const char *path){
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	return strchr(base, '.') != NULL;
}
static char *xjoin2(const char *a, const char *b){
	size_t na = strlen(a), nb = strlen(b);
	size_t need = na + 1 + nb + 1;
	char *p = malloc(need);
	if(!p) return NULL;
	snprintf(p, need, "%s/%s", a, b);
	return p;
}
static char *md_to_html_ext(const char *p){
	size_t n = strlen(p);
	if(n >= 3 && strcmp(p + n - 3, ".md") == 0){
		size_t m = n - 3;
		char *r = malloc(m + sizeof(".html"));
		if(!r) return NULL;
		memcpy(r, p, m);
		memcpy(r + m, ".html", sizeof(".html"));
		return r;
	}
	return strdup(p);
}

/* mkdir -p for parent dirs */
static int mkdir_p(const char *path, mode_t mode){
	char *s = strdup(path);
	if(!s) return -1;
	int rv = 0;
	for(char *p = s + 1; *p; p++){
		if(*p != '/') continue;
		*p = '\0';
		if(mkdir(s, mode) == -1 && errno != EEXIST){ rv = -1; break; }
		*p = '/';
	}
	if(rv == 0 && mkdir(s, mode) == -1 && errno != EEXIST) rv = -1;
	free(s);
	return rv;
}

/* ---------------- File I/O ---------------- */

static char *read_file(Arena *a, const char *path, int temp){
	struct stat st;
	int fd = open(path, O_RDONLY);
	if(fd == -1) return NULL;
	if(fstat(fd, &st) == -1 || st.st_size < 0){ close(fd); return NULL; }
	size_t n = (size_t)st.st_size;
	char *buf = temp ? arena_alloc_temp(a, n + 1) : arena_alloc_perm(a, n + 1);
	if(!buf){ close(fd); return NULL; }
	size_t off = 0;
	while(off < n){
		ssize_t r = read(fd, buf + off, n - off);
		if(r <= 0){ close(fd); return NULL; }
		off += (size_t)r;
	}
	buf[n] = '\0';
	close(fd);
	return buf;
}

static int write_all(int fd, const void *buf, size_t n){
	const uint8_t *p = buf;
	while(n){
		ssize_t w = write(fd, p, n);
		if(w < 0){
			if(errno == EINTR) continue;
			return -1;
		}
		p += (size_t)w;
		n -= (size_t)w;
	}
	return 0;
}

static int copy_times(const char *dst, const struct stat *st){
	struct utimbuf tb;
	tb.actime = st->st_atime;
	tb.modtime = st->st_mtime;
	return utime(dst, &tb);
}

static int preserve_mode_mtime(const char *dst, const struct stat *st){
	if(chmod(dst, st->st_mode & 0777) == -1) return -1;
	if(copy_times(dst, st) == -1) return -1;
	return 0;
}

static int needs_rebuild_from_mtime(const char *src, const char *dst){
	struct stat src_st, dst_st;
	if(stat(src, &src_st) != 0) return 1;
	if(stat(dst, &dst_st) != 0) return 1;
	if(!S_ISREG(dst_st.st_mode)) return 1;
	return dst_st.st_mtime < src_st.st_mtime;
}

static int copy_file(const char *src, const char *dst){
	struct stat st;
	int in = open(src, O_RDONLY);
	if(in == -1) return -1;
	if(fstat(in, &st) == -1){ close(in); return -1; }

	int out = open(dst, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if(out == -1){ close(in); return -1; }

	uint8_t buf[8192];
	for(;;){
		ssize_t r = read(in, buf, sizeof(buf));
		if(r == 0) break;
		if(r < 0){
			if(errno == EINTR) continue;
			break;
		}
		if(write_all(out, buf, (size_t)r) != 0){ close(in); close(out); return -1; }
	}
	close(in);
	close(out);
	if(preserve_mode_mtime(dst, &st) == -1) return -1;
	return 0;
}

/* ---------------- Markdown + preprocessing ---------------- */

static void md_cb(const MD_CHAR *text, MD_SIZE size, void *userdata){
	Buf *b = userdata;
	(void)buf_putn(b, text, (size_t)size);
}

/* strip local href ending with .md (".md\"" -> "\"") */
static void postprocess_links_strip_md(char *html){
	const char *needle = ".md\"";
	char *p = html;
	while((p = strstr(p, needle))){
		memmove(p, p + 3, strlen(p + 3) + 1);
	}
}

/* locate snippet content inside file; returns pointer into file with temporary NUL insertion */
static char *extract_snippet(char *file, const char *name){
	if(!name || !*name) return NULL;

	char pat[512];
	snprintf(pat, sizeof(pat), "%s%s", SNIPPET_S, name);

	char *p = file;
	for(;;){
		char *s = strstr(p, SNIPPET_S);
		if(!s) return NULL;
		/* ensure it matches this snippet name at line start-ish */
		if((s == file || s[-1] == '\n') && strncmp(s, pat, strlen(pat)) == 0){
			char *start = strchr(s, '\n');
			if(!start) return NULL;
			start++;
			char *end = strstr(start, SNIPPET_E);
			if(!end) return NULL;
			/* end marker should be at line start */
			char *ls = end;
			while(ls > start && ls[-1] != '\n') ls--;
			if(ls != end) { p = end + 1; continue; }
			*end = '\0';
			return start;
		}
		p = s + 1;
	}
}

/* append entire file but skip snippet marker lines */
static void append_file_stripping_markers(Buf *out, const char *file){
	const char *p = file;
	while(*p){
		const char *line = p;
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - line) : strlen(line);

		/* check line prefix */
		if(!(n >= strlen(SNIPPET_S) && memcmp(line, SNIPPET_S, strlen(SNIPPET_S)) == 0) &&
		   !(n >= strlen(SNIPPET_E) && memcmp(line, SNIPPET_E, strlen(SNIPPET_E)) == 0)){
			buf_putn(out, line, n);
			if(nl) buf_putn(out, "\n", 1);
		} else {
			if(nl) buf_putn(out, "\n", 1);
		}
		if(!nl) break;
		p = nl + 1;
	}
}

/* trim spaces/tabs and optional \r at end, for line token checks */
static void line_trim(const char *s, size_t n, const char **out_s, size_t *out_n){
	while(n && (*s == ' ' || *s == '\t')){ s++; n--; }
	while(n && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) n--;
	*out_s = s; *out_n = n;
}

/* parse $code line: "$code <path> [snippet]" (snippet optional; supports [name] or bare token) */
static void handle_code_line(Arena *a, const char *s, size_t n, Buf *out){
	/* s/n already trimmed to line content (no leading ws) */
	const char *p = s + (sizeof(CODE_CMD) - 1);
	while((size_t)(p - s) < n && (*p == ' ' || *p == '\t')) p++;

	/* path token */
	const char *path_s = p;
	while((size_t)(p - s) < n && *p != ' ' && *p != '\t') p++;
	size_t path_n = (size_t)(p - path_s);
	if(path_n == 0) return;

	char path[2048];
	if(path_n >= sizeof(path)) path_n = sizeof(path) - 1;
	memcpy(path, path_s, path_n);
	path[path_n] = '\0';

	/* optional snippet token */
	while((size_t)(p - s) < n && (*p == ' ' || *p == '\t')) p++;
	char snip[256]; snip[0] = '\0';
	if((size_t)(p - s) < n){
		const char *sn_s = p;
		size_t sn_n = n - (size_t)(p - s);
		/* allow [name] */
		if(sn_n >= 2 && sn_s[0] == '['){
			const char *rb = memchr(sn_s, ']', sn_n);
			if(rb){
				sn_s++;
				sn_n = (size_t)(rb - sn_s);
			}
		} else {
			/* bare token: stop at whitespace */
			const char *q = sn_s;
			while((size_t)(q - s) < n && *q != ' ' && *q != '\t') q++;
			sn_n = (size_t)(q - sn_s);
		}
		if(sn_n >= sizeof(snip)) sn_n = sizeof(snip) - 1;
		memcpy(snip, sn_s, sn_n);
		snip[sn_n] = '\0';
	}

	char *file = read_file(a, path, 1);
	if(!file){
		buf_puts(out, "`[Code file not found: ");
		buf_puts(out, path);
		buf_puts(out, "]`");
		return;
	}

	buf_puts(out, "\n```\n");
	if(snip[0]){
		char *snippet = extract_snippet(file, snip);
		if(snippet) buf_puts(out, snippet);
		else buf_puts(out, "SNIPPET NOT FOUND\n");
	} else {
		append_file_stripping_markers(out, file);
	}
	buf_puts(out, "\n```\n");
	arena_reset_temp(a);
}

static char *preprocess(Arena *a, const char *src){
	Buf out = {0};
	const char *p = src;

	while(*p){
		const char *line = p;
		const char *nl = strchr(p, '\n');
		size_t ln = nl ? (size_t)(nl - line) : strlen(line);

		const char *ts; size_t tn;
		line_trim(line, ln, &ts, &tn);

		/* sidenote markers must occupy their own paragraph/line */
		if(tn == strlen(SIDENOTE_S) && memcmp(ts, SIDENOTE_S, tn) == 0){
			buf_puts(&out, SIDENOTE_R_S);
			buf_putn(&out, "\n", 1);
		} else if(tn == strlen(SIDENOTE_E) && memcmp(ts, SIDENOTE_E, tn) == 0){
			buf_puts(&out, SIDENOTE_R_E);
			buf_putn(&out, "\n", 1);
		} else if(tn >= strlen(CODE_CMD) && memcmp(ts, CODE_CMD, strlen(CODE_CMD)) == 0){
			handle_code_line(a, ts, tn, &out);
			buf_putn(&out, "\n", 1);
		} else {
			buf_putn(&out, line, ln);
			if(nl) buf_putn(&out, "\n", 1);
		}

		if(!nl) break;
		p = nl + 1;
	}
	/* Empty input still needs a valid C string for md_html/strlen. */
	if(!out.p){
		out.p = strdup("");
	}
	return out.p; /* caller frees */
}

static int write_output_wrapped(const char *dst, const char *html, const char *layout){
	int fd = open(dst, O_WRONLY|O_CREAT|O_TRUNC, 0644);
	if(fd == -1) return -1;

	if(layout){
		const char *ip = strstr(layout, BODY_PH);
		if(ip){
			if(write_all(fd, layout, (size_t)(ip - layout)) != 0) goto fail;
			if(write_all(fd, html, strlen(html)) != 0) goto fail;
			const char *tail = ip + (sizeof(BODY_PH) - 1);
			if(write_all(fd, tail, strlen(tail)) != 0) goto fail;
			close(fd);
			return 0;
		}
		/* If layout exists but no placeholder, just emit layout then html */
		if(write_all(fd, layout, strlen(layout)) != 0) goto fail;
		if(write_all(fd, html, strlen(html)) != 0) goto fail;
		close(fd);
		return 0;
	}

	if(write_all(fd, html, strlen(html)) != 0) goto fail;
	close(fd);
	return 0;

fail:
	close(fd);
	return -1;
}

static int md_to_html_file(const char *md_path, const char *out_path, const char *layout_path)
{
	struct stat src_st;
	int have_src_st = (stat(md_path, &src_st) == 0);

	Arena a;
	arena_init(&a, 8 * 1024 * 1024);

	char *layout = layout_path ? read_file(&a, layout_path, 0) : NULL; /* perm */
	char *mdsrc  = read_file(&a, md_path, 0); /* perm: preprocess walks this while temp arena is reused */
	if(!mdsrc){ arena_destroy(&a); return -1; }

	char *prep = preprocess(&a, mdsrc);
	if(!prep){ arena_destroy(&a); return -1; }
	arena_reset_temp(&a);

	Buf html = {0};
	if(md_html(prep, strlen(prep), md_cb, &html, MD_DIALECT_GITHUB, 0) != 0){
		free(prep);
		free(html.p);
		arena_destroy(&a);
		return -1;
	}
	free(prep);

	if(html.p) postprocess_links_strip_md(html.p);
	int rc = write_output_wrapped(out_path, html.p ? html.p : "", layout);
	if(rc == 0 && have_src_st){
		if(preserve_mode_mtime(out_path, &src_st) == -1) rc = -1;
	}

	free(html.p);
	arena_destroy(&a);
	return rc;
}

/* ---------------- Parallel build (thread pool) ---------------- */

typedef enum { JOB_COPY, JOB_MD } JobType;

typedef struct Job {
	JobType t;
	char *src;
	char *dst;
	struct Job *next;
} Job;

typedef struct {
	Job *head, *tail;
	int closed;
	pthread_mutex_t mu;
	pthread_cond_t cv;
} JobQ;

static void jq_init(JobQ *q){
	memset(q, 0, sizeof(*q));
	pthread_mutex_init(&q->mu, NULL);
	pthread_cond_init(&q->cv, NULL);
}
static void jq_close(JobQ *q){
	pthread_mutex_lock(&q->mu);
	q->closed = 1;
	pthread_cond_broadcast(&q->cv);
	pthread_mutex_unlock(&q->mu);
}
static void jq_push(JobQ *q, Job *j){
	j->next = NULL;
	pthread_mutex_lock(&q->mu);
	if(q->tail) q->tail->next = j;
	else q->head = j;
	q->tail = j;
	pthread_cond_signal(&q->cv);
	pthread_mutex_unlock(&q->mu);
}
static Job *jq_pop(JobQ *q){
	pthread_mutex_lock(&q->mu);
	while(!q->head && !q->closed) pthread_cond_wait(&q->cv, &q->mu);
	Job *j = q->head;
	if(j){
		q->head = j->next;
		if(!q->head) q->tail = NULL;
	}
	pthread_mutex_unlock(&q->mu);
	return j;
}

typedef struct {
	JobQ *q;
	const char *layout_path;
} WorkerCtx;

static void *worker_main(void *arg){
	WorkerCtx *ctx = arg;
	for(;;){
		Job *j = jq_pop(ctx->q);
		if(!j){
			/* closed + empty */
			break;
		}
		if(j->t == JOB_COPY){
			if(copy_file(j->src, j->dst) != 0){
				fprintf(stderr, "copy failed: %s -> %s (%s)\n",
				        j->src, j->dst, strerror(errno));
			}
		} else {
			if(md_to_html_file(j->src, j->dst, ctx->layout_path) != 0){
				fprintf(stderr, "render failed: %s -> %s (%s)\n",
				        j->src, j->dst, strerror(errno));
			}
		}
		free(j->src);
		free(j->dst);
		free(j);
	}
	return NULL;
}

/* ---------------- Build traversal (fts) ---------------- */

static void build_tree_parallel(const char *srcroot, const char *dstroot, int nthreads)
{
	/* layout is discovered in srcroot/layout.html (optional) */
	char *layout_path = xjoin2(srcroot, "layout.html");
	const char *layout_use = NULL;
	struct stat st;
	if(layout_path && stat(layout_path, &st) == 0 && S_ISREG(st.st_mode)) layout_use = layout_path;

	if(mkdir(dstroot, 0755) == -1 && errno != EEXIST){
		perror("mkdir dest");
		free(layout_path);
		exit(1);
	}

	JobQ q;
	jq_init(&q);

	WorkerCtx wctx = { .q = &q, .layout_path = layout_use };

	if(nthreads < 1) nthreads = 1;
	pthread_t *ths = calloc((size_t)nthreads, sizeof(*ths));
	if(!ths){ perror("calloc"); free(layout_path); exit(1); }

	for(int i = 0; i < nthreads; i++){
		if(pthread_create(&ths[i], NULL, worker_main, &wctx) != 0){
			fprintf(stderr, "pthread_create failed\n");
			exit(1);
		}
	}

	char *paths[] = { (char*)srcroot, NULL };
	FTS *fts = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
	if(!fts){ perror("fts_open"); exit(1); }

	size_t base = strlen(srcroot);

	while(1){
		FTSENT *ent = fts_read(fts);
		if(!ent) break;

		if(ent->fts_level == 0) continue;

		/* skip dotfiles/dirs like original */
		if(ent->fts_name[0] == '.'){
			if(ent->fts_info == FTS_D) fts_set(fts, ent, FTS_SKIP);
			continue;
		}

		const char *src = ent->fts_path;
		const char *rel = src + base;
		if(*rel == '/') rel++;

		char *dst = xjoin2(dstroot, rel);
		if(!dst){ perror("malloc"); continue; }

		if(ent->fts_info == FTS_D){
			if(mkdir(dst, 0755) == -1 && errno != EEXIST) perror("mkdir");
			free(dst);
			continue;
		}

		if(ent->fts_info != FTS_F){
			free(dst);
			continue;
		}

		/* ensure parent exists (cheap safety net) */
		{
			char *slash = strrchr(dst, '/');
			if(slash){
				*slash = '\0';
				if(mkdir_p(dst, 0755) == -1 && errno != EEXIST) perror("mkdir_p");
				*slash = '/';
			}
		}

		if(has_ext(ent->fts_name, ".md")){
			char *dst2 = md_to_html_ext(dst);
			free(dst);
			if(!dst2) continue;
			if(!needs_rebuild_from_mtime(src, dst2)){
				free(dst2);
				continue;
			}
			dst = dst2;
		} else {
			if(!needs_rebuild_from_mtime(src, dst)){
				free(dst);
				continue;
			}
		}

		Job *j = calloc(1, sizeof(*j));
		if(!j){ perror("calloc"); free(dst); continue; }

		j->src = strdup(src);
		if(!j->src){ free(j); free(dst); continue; }
		j->dst = dst;
		j->t = has_ext(ent->fts_name, ".md") ? JOB_MD : JOB_COPY;

		jq_push(&q, j);
	}

	(void)fts_close(fts);
	jq_close(&q);

	for(int i = 0; i < nthreads; i++) pthread_join(ths[i], NULL);

	free(ths);
	free(layout_path);
}

/* ---------------- Serve mode (mongoose) ---------------- */

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int sig){ (void)sig; g_stop = 1; }

typedef struct {
	const char *root;
	char *layout_path; /* root/layout.html (optional) */
} ServeCtx;

static char *req_to_md_path(const char *root, struct mg_str uri)
{
	/* uri is not NUL-terminated; copy into a buffer */
	char *u = malloc(uri.len + 1);
	if(!u) return NULL;
	memcpy(u, uri.buf, uri.len);
	u[uri.len] = '\0';

	/* strip query etc already handled by mongoose in hm->uri; still normalize */
	char *path = u;
	if(path[0] == '\0') { free(u); return NULL; }

	/* map "/" to "/index" */
	if(strcmp(path, "/") == 0){
		free(u);
		u = strdup("/index");
		if(!u) return NULL;
		path = u;
	}

	/* prevent simple .. traversal (keep it basic) */
	if(strstr(path, "..")){ free(u); return NULL; }

	/* root + path(without leading /) + ".md" */
	const char *rel = (path[0] == '/') ? path + 1 : path;
	size_t need = strlen(root) + 1 + strlen(rel) + 3 + 1;
	char *mdp = malloc(need);
	if(!mdp){ free(u); return NULL; }
	snprintf(mdp, need, "%s/%s.md", root, rel);

	free(u);
	return mdp;
}

static void serve_markdown(struct mg_connection *c, struct mg_http_message *hm, ServeCtx *ctx)
{
	char *mdp = req_to_md_path(ctx->root, hm->uri);
	if(!mdp){
		mg_http_reply(c, 400, "", "Bad request\n");
		return;
	}

	struct stat st;
	if(stat(mdp, &st) != 0 || !S_ISREG(st.st_mode)){
		free(mdp);
		mg_http_reply(c, 404, "", "Not found\n");
		return;
	}

	/* render into memory, then reply */
	Arena a; arena_init(&a, 8 * 1024 * 1024);
	char *layout = NULL;
	if(ctx->layout_path && stat(ctx->layout_path, &st) == 0 && S_ISREG(st.st_mode))
		layout = read_file(&a, ctx->layout_path, 0);

	char *mdsrc = read_file(&a, mdp, 0);
	free(mdp);
	if(!mdsrc){ arena_destroy(&a); mg_http_reply(c, 500, "", "Read failed\n"); return; }

	char *prep = preprocess(&a, mdsrc);
	if(!prep){ arena_destroy(&a); mg_http_reply(c, 500, "", "Render failed\n"); return; }
	arena_reset_temp(&a);

	Buf html = {0};
	if(md_html(prep, strlen(prep), md_cb, &html, MD_DIALECT_GITHUB, 0) != 0){
		free(prep);
		free(html.p);
		arena_destroy(&a);
		mg_http_reply(c, 500, "", "Render failed\n");
		return;
	}
	free(prep);

	if(html.p) postprocess_links_strip_md(html.p);

	/* wrap into response body */
	Buf body = {0};
	if(layout){
		const char *ip = strstr(layout, BODY_PH);
		if(ip){
			buf_putn(&body, layout, (size_t)(ip - layout));
			buf_puts(&body, html.p ? html.p : "");
			buf_puts(&body, ip + (sizeof(BODY_PH) - 1));
		} else {
			buf_puts(&body, layout);
			buf_puts(&body, html.p ? html.p : "");
		}
	} else {
		buf_puts(&body, html.p ? html.p : "");
	}

	mg_http_reply(c, 200, "Content-Type: text/html; charset=utf-8\r\n", "%.*s",
	              (int)body.len, body.p ? body.p : "");

	free(body.p);
	free(html.p);
	arena_destroy(&a);
}

static void http_fn(struct mg_connection *c, int ev, void *ev_data)
{
	if(ev != MG_EV_HTTP_MSG) return;
	struct mg_http_message *hm = ev_data;
	ServeCtx *ctx = (ServeCtx *)c->fn_data;

	/* If request path has an extension, serve raw file unprocessed */
	{
		char *u = malloc(hm->uri.len + 1);
		if(!u){ mg_http_reply(c, 500, "", "oom\n"); return; }
		memcpy(u, hm->uri.buf, hm->uri.len);
		u[hm->uri.len] = '\0';
		int ext = path_has_extension(u);
		free(u);

		if(ext){
			struct mg_http_serve_opts opts;
			memset(&opts, 0, sizeof(opts));
			opts.root_dir = ctx->root;
			mg_http_serve_dir(c, hm, &opts);
			return;
		}
	}

	/* No extension => render Markdown */
	serve_markdown(c, hm, ctx);
}

static void serve_http(const char *root, const char *port)
{
	char url[128];
	snprintf(url, sizeof(url), "http://0.0.0.0:%s", port);

	ServeCtx ctx;
	ctx.root = root;
	ctx.layout_path = xjoin2(root, "layout.html");

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	struct mg_mgr mgr;
	mg_mgr_init(&mgr);

	if(mg_http_listen(&mgr, url, http_fn, &ctx) == NULL){
		fprintf(stderr, "Failed to listen on %s\n", url);
		mg_mgr_free(&mgr);
		free(ctx.layout_path);
		exit(1);
	}

	printf("Serving %s on %s (Ctrl-C to stop)\n", root, url);
	while(!g_stop) mg_mgr_poll(&mgr, 200);

	mg_mgr_free(&mgr);
	free(ctx.layout_path);
}

static int is_port_spec(const char *s){
	/* md2html uses ":8080" style */
	return s && s[0] == ':' && s[1] != '\0';
}

static int cpu_count(void){
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	if(n < 1) return 1;
	if(n > 128) return 128;
	return (int)n;
}

static void usage(const char *argv0){
	fprintf(stderr,
		"Usage:\n"
		"  %s              # serve current dir on :8080\n"
		"  %s :PORT        # serve current dir on :PORT\n"
		"  %s DESTDIR      # build into DESTDIR\n"
		"Options:\n"
		"  -j N            # parallel build workers (default: CPU count)\n",
		argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
	int j = cpu_count();
	int opt;

	while((opt = getopt(argc, argv, "j:")) != -1){
		switch(opt){
		case 'j': j = atoi(optarg); if(j < 1) j = 1; break;
		default: usage(argv[0]); return 2;
		}
	}

	const char *dest = NULL;
	if(optind < argc) dest = argv[optind];

	/* No args => server on :8080, serving current directory */
	if(!dest){
		serve_http(".", "8080");
		return 0;
	}

	/* dest is :PORT => server mode */
	if(is_port_spec(dest)){
		serve_http(".", dest + 1);
		return 0;
	}

	/* else dest is directory => build mode */
	build_tree_parallel(".", dest, j);
	return 0;
}
