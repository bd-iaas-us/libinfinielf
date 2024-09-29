#ifndef __INFELF_RDMA_H__
#define __INFELF_RDMA_H__

#include <pthread.h>
#include <stdbool.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>
#include "infelf.h"

#define INFELF_BUFSIZE 64*1024
#define INFELF_SQ_DEPTH 16
#define INFELF_WQ_DEPTH 16

enum {
    INFELF_SOCK_CTRL_RDMA_NODE = 0,
    INFELF_SOCK_CTRL_RDMA_INFO = 1
};

struct infelf_rdma_info_t {
    uint64_t buf;
    uint32_t rkey;
    uint32_t size;
};

// RDMA endpoint
struct infelf_rdma_node_t {
    int lid;
    int qpn;
    int psn;
    union ibv_gid gid;

    char ib_devname[IBV_SYSFS_NAME_MAX];
    int ib_port;
    int ib_gidx;
};

// Used for socket-based connection control
union infelf_sock_ctrl_t {
    int type;
    struct infelf_rdma_info_t rdma_info;
    struct infelf_rdma_node_t rdma_node;
};

// Used for rdma-based connection control
struct infelf_rdma_ctrl_t {
    struct ibv_recv_wr rq_wr;         /* recv work request record */
    struct ibv_sge recv_sgl;          /* recv single SGE */
    struct infelf_rdma_info_t recv_buf;
    struct ibv_mr *recv_mr;           /* MR associated with this buffer */

    struct ibv_send_wr sq_wr;         /* send work request record */
    struct ibv_sge send_sgl;
    struct infelf_rdma_info_t send_buf; /* send single buf SGE */
    struct ibv_mr *send_mr;
};

// RDMA Connection
struct infelf_cfg_t {
    const char* ib_devname;
    int ib_port;
    // If true, infelf_rdma_info_t will be exchanged using TCP socket
    bool use_socket_for_rinfo;
    // Enable CQ event channel for notifications
    bool use_event_channel;
};

// RDMA Connection
struct infelf_conn_t {
    bool use_socket_for_rinfo;
    bool use_event_channel;
    struct infelf_rdma_ctrl_t ctrl;
    // buf/rkey/size of remote RDMA peer
    struct infelf_rdma_info_t tx;

    struct ibv_context *verbs;
    // Used when use_event_channel is sets to True
    struct ibv_comp_channel *channel;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct infelf_rdma_node_t local_info, remote_info;

    //RDMA resources/buffers for payload
    int is_gpu_buffer;
    struct ibv_send_wr rdma_sq_wr;
    struct ibv_sge rdma_sgl;
    struct ibv_mr *rdma_mr;
    void *rdma_buf;
    int rdma_buf_size;
};

void size_str(char *str, size_t ssize, long long size);
void cnt_str(char *str, size_t ssize, long long cnt);
int size_to_count(int size);
uint64_t gettime_ns(void);
uint64_t gettime_us(void);
int sleep_us(unsigned int time_us);

int infelf_init(struct infelf_cfg_t *cfg, struct infelf_conn_t *conn);
int infelf_connect(struct infelf_conn_t *conn);
int infelf_close(struct infelf_conn_t *conn);
int infelf_rdma_read(struct infelf_conn_t *conn);
int infelf_rdma_write_with_imm(struct infelf_conn_t *conn, int imm);
int infelf_rinfo_socket_format(int type,
                               struct infelf_conn_t *conn,
                               union infelf_sock_ctrl_t *msg);
int infelf_rinfo_socket_parse(void *buf,
                              int size,
                              struct infelf_conn_t *conn);
int infelf_rinfo_rdma_send(struct infelf_conn_t *conn);
int infelf_rinfo_rdma_recv(struct infelf_conn_t *conn, struct ibv_wc *wc);
int infelf_alloc_buffers(struct infelf_conn_t *conn,
                         int size,
                         int gpu_dev_id);
void infelf_free_buffers(struct infelf_conn_t *conn);

#endif
