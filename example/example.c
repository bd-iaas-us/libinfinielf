#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <netdb.h>
#include <uv.h>
#include "infelf_rdma.h"
#include "example.h"

/*
 * example:
 *     client sends source rkey/addr/len
 *     server receives source rkey/add/len
 *     server rdma reads "ping" data from source
 *     server sends "go ahead" on rdma read completion
 *     client sends sink rkey/addr/len
 *     server receives sink rkey/addr/len
 *     server rdma writes "pong" data to sink
 *     server sends "go ahead" on rdma write completion
 */

static int get_addr(char *dst, struct sockaddr *addr)
{
    struct addrinfo *res;
    int ret;

    ret = getaddrinfo(dst, NULL, NULL, &res);
    if (ret) {
        ERROR("getaddrinfo failed (%s) - invalid hostname or IP address\n",
              gai_strerror(ret));
        return ret;
    }

    if (res->ai_family == PF_INET)
        memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
    else if (res->ai_family == PF_INET6)
        memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in6));
    else
        ret = -1;

    freeaddrinfo(res);
    return ret;
}


static void usage(const char *name)
{
    printf("%s -s [-vd] [-S size] [-a addr] [-p port]\n", name);
    printf("%s -c [-vd] [-S size] [-I addr] -a addr [-p port]\n", name);
    printf("\t-c\t\tclient side\n");
    printf("\t-s\t\tserver side. To bind to any address with IPv6 use -a ::0\n");
    printf("\t-v\t\tdisplay data to stdout\n");
    printf("\t-d\t\tdebug printfs\n");
    printf("\t-S size \tping data size\n");
    printf("\t-a addr\t\taddress\n");
    printf("\t-p port\t\tport\n");
}

static int cq_event_handler(struct app_ctx_t *app)
{
    struct ibv_wc wc;
    struct infelf_conn_t *conn = &app->conn;
    int ret;
    int flushed = 0;

    while ((ret = ibv_poll_cq(conn->cq, 1, &wc)) == 1) {
        ret = 0;

        if (wc.status) {
            if (wc.status == IBV_WC_WR_FLUSH_ERR) {
                flushed = 1;
                continue;

            }
            ERROR("cq completion failed status %d\n", wc.status);
            ret = -1;
            goto error;
        }

        switch (wc.opcode) {
        case IBV_WC_SEND:
            DEBUG("%s: [CQ] send completion\n",
                  app->is_server ? "server" : "client");
            break;
        case IBV_WC_RDMA_WRITE:
            DEBUG("%s: [CQ] rdma write completion\n",
                  app->is_server ? "server" : "client");
            app->state = RDMA_WRITE_COMPLETE;
            break;
        case IBV_WC_RDMA_READ:
            DEBUG("%s: [CQ] rdma read completion\n",
                  app->is_server ? "server" : "client");
            app->state = RDMA_READ_COMPLETE;
            break;
        case IBV_WC_RECV:
            DEBUG("%s: [CQ] recv completion\n",
                  app->is_server ? "server" : "client");
            if (app->is_server) {
                if (app->state <= CONNECTED || app->state == RDMA_WRITE_COMPLETE)
                    app->state = RDMA_READ_ADV;
                else
                    app->state = RDMA_WRITE_ADV;
            } else {
                if (app->state == RDMA_READ_ADV)
                    app->state = RDMA_WRITE_ADV;
                else
                    app->state = RDMA_WRITE_COMPLETE;
            }
            if (ret) {
                ERROR("[CQ] recv wc error: %d\n", ret);
                goto error;
            }
            break;

        default:
            DEBUG("[CQ] unknown!!!!! completion\n");
            ret = -1;
            goto error;
        }
    }
    if (ret) {
        ERROR("[CQ] poll error %d\n", ret);
        goto error;
    }
    return flushed;

error:
    app->state = ERROR;
    return ret;
}

void cq_thread(void *arg)
{
    struct app_ctx_t *app = arg;
    struct infelf_conn_t *conn = &app->conn;
    struct ibv_cq *ev_cq;
    void *ev_ctx;
    int ret;

    DEBUG("cq_thread started.\n");
    while (1) {
        ret = ibv_get_cq_event(conn->channel, &ev_cq, &ev_ctx);
        if (ret) {
            ERROR("Failed to get cq event!\n");
            break;
        }
        if (ev_cq != conn->cq) {
            ERROR("Unknown CQ!\n");
            break;
        }
        ret = ibv_req_notify_cq(conn->cq, 0);
        if (ret) {
            ERROR("Failed to set notify!\n");
            break;
        }
        ret = cq_event_handler(app);
        ibv_ack_cq_events(conn->cq, 1);
        if (ret)
            break;
    }
}

int main(int argc, char *argv[])
{
    struct app_ctx_t *app;
    int op;
    int ret = 0;

    app = calloc(1, sizeof(*app));
    if (!app)
        return -ENOMEM;

    app->is_server = -1;
    app->sin.ss_family = PF_INET;
    app->port = 7174;
    app->state = IDLE;
    app->size = 64;

    opterr = 0;
    while ((op = getopt(argc, argv, "a:p:S:G:scvd")) != -1) {
        switch (op) {
        case 'a':
            ret = get_addr(optarg, (struct sockaddr *) &app->sin);
            break;
        case 'p':
            app->port = atoi(optarg);
            DEBUG("port %d\n", (int) atoi(optarg));
            break;
        case 's':
            app->is_server = 1;
            break;
        case 'c':
            app->is_server = 0;
            break;
        case 'S':
            app->size = atoi(optarg);
            if ((app->size < INFELF_TEST_MIN_BUFSIZE) ||
                (app->size > (INFELF_BUFSIZE - 1))) {
                ERROR("Invalid size %d "
                       "(valid range is %zd to %d)\n",
                       app->size, INFELF_TEST_MIN_BUFSIZE, INFELF_BUFSIZE);
                ret = EINVAL;
            } else
                DEBUG("size %d\n", (int) atoi(optarg));
            break;
        case 'v':
            app->verbose++;
            break;
        case 'G':
            app->gpu_bdf = strdup(optarg);
            break;
        case 'd':
            debug++;
            break;
        default:
            usage("example");
            ret = EINVAL;
            goto out;
        }
    }
    if (ret)
        goto out;

    if (app->is_server == -1) {
        usage("example");
        ret = EINVAL;
        goto out;
    }

    if (app->is_server) {
        ret = run_server(app);
    } else {
        ret = run_client(app);
    }

out:
    free(app);
    return ret;
}
