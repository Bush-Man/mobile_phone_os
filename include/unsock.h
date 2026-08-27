#ifndef UNSOCK_H
#define UNSOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "task.h"

struct file;

/*
 * Unix-domain sockets (phase 8, plan item 45) -- the local transport
 * later daemons and the UI compositor protocol will ride on.
 *
 * SOCK_STREAM flavour only, two topologies:
 *
 *   - socketpair(): anonymous connected pair, back-to-back buffers;
 *   - named transport: usock_serve(path) publishes a listener fd,
 *     clients usock_connect(path) (blocking, refuses on dead/full
 *     listeners through the normal wait machinery) and servers
 *     usock_accept() them one at a time (backlog of 4).
 *
 * Endpoints integrate with the VFS as anonymous V_SOCK vnodes so
 * read/write/lseek(-ESPIPE)/poll behave exactly like pipes do; the
 * poll wakeup contract is shared (every state change calls
 * ipc_wake_pollers()).
 */

#define USOCK_NAME_MAX  24
#define USOCK_BUF_CAP  2048u
#define USOCK_EP_MAX   16u              /* data endpoints total        */
#define USOCK_SERVE_MAX 6u
#define USOCK_BACKLOG   4u

/* anonymous connected pair: O_RDONLY / O_WRONLY descriptions out */
int usocket_pair(struct file **a_out, struct file **b_out);

/* publish a listener; -EADDRINUSE when the path is already served */
int usock_serve(const char *path, struct file **listen_out);

/* accept one queued client; blocks when the backlog is empty     */
long usock_accept(struct file *listener, struct file **conn_out);

/* connect to a listener; blocks; -ECONNREFUSED/-ENOENT outcomes  */
long usock_connect(const char *path, struct file **conn_out);

/* readiness of a data/listener descriptor (for SYS_poll)         */
unsigned usock_file_ready(const struct file *f);

void usock_subsys_init(void);

#endif /* UNSOCK_H */