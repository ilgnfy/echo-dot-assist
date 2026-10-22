/* Outgoing queue of one client connection (outq.h). */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "outq.h"

struct item { struct item *next; size_t len; unsigned char data[]; };

struct outq {
    int fd;
    size_t limit, behind;
    pthread_mutex_t lock;               /* a leaf lock: taken under core_lock, never the other way round */
    pthread_cond_t cond;
    struct item *head, *tail;           /* under lock */
    size_t bytes;                       /* under lock: queued, not yet handed to the writer */
    int dead, closing;                  /* under lock: no more writes / drain, then end */
    unsigned left_out;                  /* under lock: messages outq_behind() kept out since the queue was last behind */
    pthread_t thread;
};

static void drop_all(struct outq *q)    /* lock held */
{
    while (q->head) { struct item *it = q->head; q->head = it->next; free(it); }
    q->tail = NULL; q->bytes = 0;
}

/* A message is one piece here, so one write: with TCP_NODELAY, header and body written apart went out as two segments */
static int write_all_fd(int fd, const unsigned char *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static void *writer(void *arg)
{
    struct outq *q = arg;
    pthread_mutex_lock(&q->lock);
    for (;;) {
        while (!q->head && !q->closing && !q->dead) pthread_cond_wait(&q->cond, &q->lock);
        if (q->dead || !q->head) break;                 /* failed, or closing with nothing left */
        struct item *it = q->head;
        q->head = it->next; if (!q->head) q->tail = NULL;
        q->bytes -= it->len;
        pthread_mutex_unlock(&q->lock);
        int rc = write_all_fd(q->fd, it->data, it->len);   /* the send timeout bounds it */
        free(it);
        pthread_mutex_lock(&q->lock);
        if (rc < 0 && !q->dead) { q->dead = 1; shutdown(q->fd, SHUT_RDWR); }    /* its reader cleans up */
    }
    drop_all(q);
    pthread_mutex_unlock(&q->lock);
    return NULL;
}

struct outq *outq_open(int fd, size_t limit, size_t behind)
{
    struct outq *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->fd = fd; q->limit = limit; q->behind = behind;
    pthread_mutex_init(&q->lock, NULL); pthread_cond_init(&q->cond, NULL);
    if (pthread_create(&q->thread, NULL, writer, q)) {
        pthread_mutex_destroy(&q->lock); pthread_cond_destroy(&q->cond); free(q);
        return NULL;
    }
    return q;
}

int outq_put(struct outq *q, const struct iovec *iov, int n)
{
    size_t len = 0;
    for (int i = 0; i < n; i++) len += iov[i].iov_len;
    pthread_mutex_lock(&q->lock);
    if (q->dead) { pthread_mutex_unlock(&q->lock); return -1; }
    struct item *it = q->bytes + len <= q->limit ? malloc(sizeof *it + len) : NULL;
    if (!it) {
        /* Not taking it would lose a state or an answer without the client knowing: better it reconnects and asks */
        fprintf(stderr, "client: %zu bytes waiting, it does not take them: disconnected\n", q->bytes);
        q->dead = 1; drop_all(q); shutdown(q->fd, SHUT_RDWR);
        pthread_cond_signal(&q->cond);
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    it->next = NULL; it->len = len;
    for (size_t i = 0, at = 0; i < (size_t)n; at += iov[i].iov_len, i++) if (iov[i].iov_len) memcpy(it->data + at, iov[i].iov_base, iov[i].iov_len);
    if (q->tail) q->tail->next = it; else q->head = it;
    q->tail = it; q->bytes += len;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

int outq_behind(struct outq *q, size_t n)
{
    pthread_mutex_lock(&q->lock);
    int behind = q->bytes + n > q->behind;
    if (behind) q->left_out++;
    else if (q->left_out) { fprintf(stderr, "client: was behind, %u messages left out\n", q->left_out); q->left_out = 0; }
    pthread_mutex_unlock(&q->lock);
    return behind;
}

void outq_close(struct outq *q)
{
    pthread_mutex_lock(&q->lock);
    q->closing = 1;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->lock);
    pthread_join(q->thread, NULL);
    pthread_mutex_destroy(&q->lock); pthread_cond_destroy(&q->cond);
    free(q);
}
