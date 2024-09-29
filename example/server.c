#include <stdio.h>
#include <stdlib.h>
#include <uv.h>
#include <sys/socket.h>
#include <unistd.h>
#include "infelf_rdma.h"
#include "infelf_gpu_mem.h"
#include "example.h"

static int example_test_server(struct app_ctx_t *app)
{
    struct ibv_send_wr *bad_wr;
    struct infelf_conn_t *conn = &app->conn;
    int ret;

    while (1) {
        /* Wait for client's Start STAG/TO/Len */
        while (app->state != RDMA_READ_ADV) {
            sleep_us(1000000);
        }
        DEBUG("server received sink adv\n");

        INFO("server: TEST CASE 1: Issue rdma_read() to client\n");
        ret = infelf_rdma_read(conn);
        if (ret) {
            ERROR("server: post send error %d\n", ret);
            break;
        }
        DEBUG("server: posted rdma read req \n");

        /* Wait to confirm the read completion */
        DEBUG("server: waiting for RDMA_READ_COMPLETE state %d\n", app->state);
        while (app->state != RDMA_READ_COMPLETE) {
            sleep_us(1000000);
        }
        DEBUG("server: received read complete\n");

        /* Display data in recv buf */
        if (app->verbose)
            printf("rdma_read() to client: %s\n", (char *)conn->rdma_buf);

        INFO("server: TEST CASE 2: Issue rdma_write() to client\n");
        /* Tell client to continue */
        ret = ibv_post_send(conn->qp, &conn->ctrl.sq_wr, &bad_wr);
        if (ret) {
            ERROR("server: post send error %d\n", ret);
            break;
        }
        DEBUG("server: posted go ahead\n");

        /* Wait for client's RDMA STAG/TO/Len */
        DEBUG("server: waiting for RDMA_WRITE_ADV state %d\n", app->state);
        while (app->state != RDMA_WRITE_ADV) {
            sleep_us(1000000);
        }
        DEBUG("server: server received sink adv\n");

        /* RDMA Write echo data */
        ret = infelf_rdma_write_with_imm(conn, 0);
        if (ret) {
            ERROR("server: post send error %d\n", ret);
            break;
        }

        /* Wait to confirm the write completion */
        DEBUG("server: waiting for RDMA_WRITE_COMPLETE state %d\n", app->state);
        while (app->state != RDMA_WRITE_COMPLETE) {
            sleep_us(1000000);
        }
        DEBUG("server: rdma write complete \n");

        /* Tell client to begin again */
        ret = ibv_post_send(conn->qp, &conn->rdma_sq_wr, &bad_wr);
        if (ret) {
            ERROR("server: post send error %d\n", ret);
            break;
        }
        DEBUG("server: posted go ahead\n");
    }

    return (app->state == DISCONNECTED) ? 0 : ret;
}

int server_exchange_rdma_info_over_tcp(int port, struct infelf_conn_t *conn) {
    int sockfd, connfd;
    struct sockaddr_in servaddr;
    union infelf_sock_ctrl_t *msg;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    bzero(&servaddr, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(port);
    bind(sockfd, (struct sockaddr *)&servaddr, sizeof(servaddr));
    listen(sockfd, 1);
    connfd = accept(sockfd, NULL, NULL);
    printf("Server is listening for exchanging RDMA node info...\n");

    msg = calloc(1, sizeof(*msg));
    infelf_rinfo_socket_format(INFELF_SOCK_CTRL_RDMA_NODE, conn, msg);
    send(connfd, msg, sizeof(*msg), 0);
    recv(connfd, msg, sizeof(*msg), 0);
    infelf_rinfo_socket_parse(msg, sizeof(*msg), conn);
    printf("Done\n");

    free(msg);
    close(connfd);
    close(sockfd);

    return 0;
}

int run_server(struct app_ctx_t *app)
{
    int ret = 0;

    struct infelf_cfg_t cfg = {
        .ib_devname = "mlx5_0",
        .ib_port = 1,
        .use_socket_for_rinfo = true,
        .use_event_channel = true
    };

    ret = infelf_init(&cfg, &app->conn);
    if (ret) {
        ERROR("server: infelf_rdma_init() failed: %d\n", ret);
        return ret;
    }
    ret = server_exchange_rdma_info_over_tcp(app->port, &app->conn);
    if (ret) {
        ERROR("server: cannot exchange rdmo node info: %d\n", ret);
        goto err0;
    }
    ret = infelf_connect(&app->conn);
    if (ret) {
        ERROR("server: infelf_rdma_connect() failed: %d\n", ret);
        goto err0;
    }
    app->state = CONNECTED;

    /* Always allocate CPU memory on server side */
    ret = infelf_alloc_buffers(&app->conn, app->size, -1);
    if (ret) {
        ERROR("server: infelf_setup_buffers failed: %d\n", ret);
        goto err0;
    }
    ret = uv_thread_create(&app->cqthread, cq_thread, app);
    if (ret) {
        ERROR("server: uv_thread_create() failed\n");
        goto err1;
    }
    ret = example_test_server(app);
    if (ret) {
        ERROR("server: failed with ret %d\n", ret);
        goto err2;
    }

err2:
    uv_thread_join(&app->cqthread);
err1:
    infelf_free_buffers(&app->conn);
err0:
    infelf_close(&app->conn);

    return ret;
}
