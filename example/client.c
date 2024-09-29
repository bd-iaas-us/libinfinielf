#include <stdio.h>
#include <uv.h>
#include <sys/socket.h>
#include <unistd.h>
#include "infelf_rdma.h"
#include "infelf_gpu_mem.h"
#include "example.h"

#ifdef HAVE_CUDA
#include "cuda_runtime.h"
#endif

static void fill_buffer(char *buf, int size) {
    int start, cc, i;
    unsigned char c;

    start = 65;
    cc = snprintf(buf, size, INFELF_TEST_MSG_FMT);
    for (i = cc, c = start; i < size; i++) {
        buf[i] = c;
        c++;
        if (c > 122)
            c = 65;
    }
    start++;
    if (start > 122)
        start = 65;
    buf[size - 1] = 0;
}

static int example_test_client(struct app_ctx_t *app)
{
    int ret = 0;
    struct infelf_conn_t *conn = &app->conn;

    INFO("client: TEST CASE 1: Prepare for server to rdma_read()\n");
    app->state = RDMA_READ_ADV;
    /* Put some ascii text in the buffer. */

    if (app->gpu_bdf) {
#ifdef HAVE_CUDA
        char *tmp_buf = malloc(app->size);
        if (!tmp_buf) {
            return -1;
        }
        fill_buffer(tmp_buf, app->size);
        cudaMemcpy(conn->rdma_buf, tmp_buf, app->size, cudaMemcpyHostToDevice);
        free(tmp_buf);
#else
        ERROR("Feature is not supported. Define HAVE_CUDA to enable...\n");
        return -1;
#endif
    } else {
        fill_buffer(conn->rdma_buf, app->size);
    }

    /* Send the start_buf/start_mr for server to do rdma_read() */
    conn->rdma_buf_size = app->size;
    ret = infelf_rinfo_rdma_send(conn);
    if (ret) {
        ERROR("client: post send error %d\n", ret);
        return -1;
    }
    INFO("client: ready for rdma_read()\n");

    INFO("client: TEST CASE 2: Print contents of rdma_write() from server\n");
    /* Wait to confirm the server is done with 1st test case */
    DEBUG("client: waiting for RDMA_WRITE_ADV state %d\n", app->state);
    while (app->state != RDMA_WRITE_ADV) {
        sleep_us(1000000);
    }

    /* Prepare the memory to be used for rdma_write() */
    ret = infelf_rinfo_rdma_send(conn);
    if (ret) {
        ERROR("post send error %d\n", ret);
        return -1;
    }

    /* Wait to confirm the server is done with rdma_write() */
    DEBUG("client: waiting for RDMA_WRITE_COMPLETE state %d\n", app->state);
    while (app->state != RDMA_WRITE_COMPLETE) {
        sleep_us(1000000);
    }

    if (app->verbose) {
        if (app->gpu_bdf) {
#ifdef HAVE_CUDA
            char *tmp_buf = malloc(app->size);
            if (!tmp_buf) {
                return -1;
            }
            cudaMemcpy(tmp_buf, conn->rdma_buf, app->size, cudaMemcpyDeviceToHost);
            printf("rdma_write() from server (GPU): %s\n", tmp_buf);
            free(tmp_buf);
#else
            ERROR("Feature is not supported. Define HAVE_CUDA to enable...\n");
            return -1;
#endif
        } else {
            printf("rdma_write() from server (CPU): %s\n", (char *)conn->rdma_buf);
        }
    }

    return (app->state == DISCONNECTED) ? 0 : ret;
}

int client_exchange_rdma_info_over_tcp(struct sockaddr_in *sin,
                                       int port,
                                       struct infelf_conn_t *conn) {
    int sockfd;
    union infelf_sock_ctrl_t *msg;

    sin->sin_port = htons(port);
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    printf("Client is connecting to server for exchanging RDMA node info...\n");
    connect(sockfd, (struct sockaddr *)sin, sizeof(*sin));

    msg = calloc(1, sizeof(*msg));
    infelf_rinfo_socket_format(INFELF_SOCK_CTRL_RDMA_NODE, conn, msg);
    send(sockfd, msg, sizeof(*msg), 0);
    recv(sockfd, msg, sizeof(*msg), 0);
    infelf_rinfo_socket_parse(msg, sizeof(*msg), conn);
    printf("Done\n");

    free(msg);
    close(sockfd);

    return 0;
}

int run_client(struct app_ctx_t *app)
{
    int dev_id = -1, ret;

    struct infelf_cfg_t cfg = {
        .ib_devname = "mlx5_0",
        .ib_port = 1,
        .use_socket_for_rinfo = true,
        .use_event_channel = true
    };

    ret = infelf_init(&cfg, &app->conn);
    if (ret) {
        ERROR("client: infelf_rdma_init() failed: %d\n", ret);
        return ret;
    }

    ret = client_exchange_rdma_info_over_tcp((struct sockaddr_in *)&app->sin,
                                             app->port,
                                             &app->conn);
    if (ret) {
        ERROR("server: cannot exchange rdmo node info: %d\n", ret);
        goto err0;
    }

    ret = infelf_connect(&app->conn);
    if (ret) {
        ERROR("client: infelf_rdma_connect() failed: %d\n", ret);
        goto err0;
    }
    app->state = CONNECTED;

    /* Allocate GPU memory if GPU BDF is supplied */
    if (app->gpu_bdf) {
        dev_id = infelf_gpu_init(app->gpu_bdf);
    }
    ret = infelf_alloc_buffers(&app->conn, app->size, dev_id);
    if (ret) {
        ERROR("client: infelf_setup_buffers failed: %d\n", ret);
        goto err1;
    }

    ret = uv_thread_create(&app->cqthread, cq_thread, app);
    if (ret) {
        ERROR("client: uv_thread_create() failed\n");
        goto err2;
    }

    ret = example_test_client(app);
    if (ret) {
        ERROR("client: failed with ret: %d\n", ret);
        goto err3;
    }

    ret = 0;

err3:
    uv_thread_join(&app->cqthread);
err2:
    infelf_free_buffers(&app->conn);
err1:
    if (app->gpu_bdf) {
       infelf_gpu_close();
    }
err0:
    infelf_close(&app->conn);

    return ret;
}
