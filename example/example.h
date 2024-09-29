#ifndef __INFINIELF_EXAMPLE_H__
#define __INFINIELF_EXAMPLE_H__

#define INFELF_TEST_MSG_FMT           "infelf_test: "
#define INFELF_TEST_MIN_BUFSIZE       sizeof(stringify(INT_MAX)) + sizeof(INFELF_TEST_MSG_FMT)

#include <uv.h>

/*
 * These states are used to signal events between the completion handler
 * and the main client or server thread.
 *
 * Once CONNECTED, they cycle through RDMA_READ_ADV, RDMA_WRITE_ADV,
 * and RDMA_WRITE_COMPLETE for each ping.
 */
enum test_state {
    IDLE = 1,
    CONNECTED,
    RDMA_READ_ADV,
    RDMA_READ_COMPLETE,
    RDMA_WRITE_ADV,
    RDMA_WRITE_COMPLETE,
    DISCONNECTED,
    ERROR
};

/*
 * Test Application Context truct.
 */
struct app_ctx_t {
    int is_server;                   /* 0 iff client */
    int socket;
    uv_thread_t cqthread;
    struct infelf_conn_t conn;

    struct sockaddr_storage sin;
    int port;                        /* dst port in NBO */

    char *gpu_bdf;                   /* allocate buffer on GPU if not NULL*/
    enum test_state state;           /* used for cond/signalling */
    int verbose;                     /* verbose logging */
    int size;                        /* ping data size */
};

int run_server(struct app_ctx_t *app);
int run_client(struct app_ctx_t *app);
void cq_thread(void *arg);

#endif
