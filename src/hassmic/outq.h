/* Outgoing queue of one client connection, written by a thread of its own.  The protocols send under core_lock; a write
 * to a client whose link stalls blocks for up to its send timeout (5 s), and with it every thread that wants core_lock:
 * the wake word, the buttons, the other clients.  So code under core_lock only queues, and the writer thread waits. */
#ifndef OUTQ_H
#define OUTQ_H
#include <stddef.h>
#include <sys/uio.h>

struct outq;

/* Starts the writer for fd.  limit: bytes queued before the client counts as gone (shut down: its reader cleans up);
 * behind: where outq_behind() starts saying so.  NULL: no thread */
struct outq *outq_open(int fd, size_t limit, size_t behind);
/* One message, the parts in one piece and in the order of the calls.  0: queued; -1: not, the connection is shut down
 * (a write failed, or the queue passed its limit) */
int  outq_put(struct outq *q, const struct iovec *iov, int n);
/* 1: n more bytes would put the queue past `behind`: a message that may be lost (mic audio) is better left out.  Asked
 * before such a message is made (and encrypted: a Noise frame left out after that would break every one behind it).
 * Counts it, and says on stderr how much was left out once the queue has caught up. */
int  outq_behind(struct outq *q, size_t n);
/* Not under core_lock: what is queued still goes out (or fails within the send timeout), then the writer ends.  Frees q;
 * the caller closes the socket after this, so the writer can never write to a descriptor that was reused. */
void outq_close(struct outq *q);
#endif
