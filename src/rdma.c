#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "infelf_rdma.h"
#include "infelf_gpu_mem.h"

int infelf_rinfo_socket_format(int type,
                               struct infelf_conn_t *conn,
                               union infelf_sock_ctrl_t *msg)
{
    msg->type = type;
    switch (type) {
        case INFELF_SOCK_CTRL_RDMA_NODE:
            msg->rdma_node.lid = htobe32(conn->local_info.lid);
            msg->rdma_node.qpn = htobe32(conn->local_info.qpn);
            msg->rdma_node.psn = htobe32(conn->local_info.psn);
            memcpy(&msg->rdma_node.gid,
                   &conn->local_info.gid,
                   sizeof(union ibv_gid));
            memcpy(msg->rdma_node.ib_devname,
                   conn->local_info.ib_devname,
                   sizeof(IBV_SYSFS_NAME_MAX));
            msg->rdma_node.ib_port = htobe32(conn->local_info.ib_port);
            msg->rdma_node.ib_gidx = htobe32(conn->local_info.ib_gidx);
            break;
        case INFELF_SOCK_CTRL_RDMA_INFO:
            msg->rdma_info.buf = htobe64(conn->tx.buf);
            msg->rdma_info.rkey = htobe32(conn->tx.rkey);
            msg->rdma_info.size = htobe32(conn->tx.size);
            break;
        default:
            return -1;
    }

    return 0;
}

int infelf_rinfo_socket_parse(void *buf,
                              int size,
                              struct infelf_conn_t *conn)
{
    union infelf_sock_ctrl_t *msg = (union infelf_sock_ctrl_t *)buf;

    if (size != sizeof(*msg)) {
        ERROR("Buffer length doesn't match sizeof(infelf_rdma_conn_t).\n");
        return -1;
    }

    switch (msg->type) {
        case INFELF_SOCK_CTRL_RDMA_NODE:
            conn->remote_info.lid = be32toh(msg->rdma_node.lid);
            conn->remote_info.qpn = be32toh(msg->rdma_node.qpn);
            conn->remote_info.psn = be32toh(msg->rdma_node.psn);
            memcpy(&conn->remote_info.gid,
                   &msg->rdma_node.gid,
                   sizeof(union ibv_gid));
            memcpy(&conn->remote_info.ib_devname,
                   msg->rdma_node.ib_devname,
                   sizeof(IBV_SYSFS_NAME_MAX));
            conn->remote_info.ib_port = be32toh(msg->rdma_node.ib_port);
            conn->remote_info.ib_gidx = be32toh(msg->rdma_node.ib_gidx);
            break;
        case INFELF_SOCK_CTRL_RDMA_INFO:
            conn->tx.buf = be64toh(msg->rdma_info.buf);
            conn->tx.rkey = be32toh(msg->rdma_info.rkey);
            conn->tx.size = be32toh(msg->rdma_info.size);
            DEBUG("rkey %x addr %" PRIx64 " len %d from peer\n",
                  conn->tx.rkey, conn->tx.buf, conn->tx.size);
            break;
        default:
            ERROR("bogus data, size %d\n", size);
            return -1;
    }

    return 0;
}

int infelf_rinfo_rdma_send(struct infelf_conn_t *conn)
{
    struct ibv_send_wr *bad_wr;
    struct infelf_rdma_info_t *info = &conn->ctrl.send_buf;

    info->buf = htobe64((uint64_t) (unsigned long) conn->rdma_buf);
    info->rkey = htobe32(conn->rdma_mr->rkey);
    info->size = htobe32(conn->rdma_buf_size);

    DEBUG("RDMA addr %" PRIx64" rkey %x len %d\n",
          be64toh(info->buf), be32toh(info->rkey), be32toh(info->size));

    if (ibv_post_send(conn->qp, &conn->ctrl.sq_wr, &bad_wr)) {
        ERROR("ibv_post_send() failed: %d\n", errno);
        return errno;
    }

    return 0;
}

int infelf_rinfo_rdma_recv(struct infelf_conn_t *conn, struct ibv_wc *wc)
{
    struct infelf_rdma_info_t *tx = &conn->tx;
    if (wc->byte_len != sizeof(conn->ctrl.recv_buf)) {
        ERROR("received bogus data, size %d\n", wc->byte_len);
        return -1;
    }

    tx->buf = be64toh(conn->ctrl.recv_buf.buf);
    tx->rkey = be32toh(conn->ctrl.recv_buf.rkey);
    tx->size = be32toh(conn->ctrl.recv_buf.size);
    DEBUG("received rkey %x addr %" PRIx64 " len %d from peer\n",
          tx->rkey, tx->buf, tx->size);

    return 0;
}

int infelf_pop_node_info(struct infelf_conn_t *conn,
                         const char* ib_devname,
                         int ib_port)
{
    struct infelf_rdma_node_t *node = &conn->local_info;

    // Truncate the interface name for memory safety
    int size = strlen(ib_devname) + 1;
    if (size > sizeof(node->ib_devname)) {
        size = sizeof(node->ib_devname);
    }
    memcpy(node->ib_devname, ib_devname, size);
    node->ib_port = ib_port;
    // TODO: gid support, hard-coded for now
    node->ib_gidx = 3;

    struct ibv_port_attr port_attr;
    if (ibv_query_port(conn->verbs, ib_port, &port_attr)) {
        ERROR("ibv_query_port() failed: %d\n", errno);
        return errno;
    }
    if (port_attr.link_layer != IBV_LINK_LAYER_ETHERNET && !port_attr.lid) {
        ERROR("invalid lid of %s port %d\n", ib_devname, ib_port);
        return -1;
    }
    node->lid = port_attr.lid;
    node->qpn = conn->qp->qp_num;
    node->psn = lrand48() & 0xffffff;
    if (ibv_query_gid(conn->verbs, node->ib_port, node->ib_gidx, &node->gid)) {
        ERROR("ibv_query_gid() failed: %d. index=%d\n", errno, node->ib_gidx);
        return errno;
    }

    return 0;
}

struct ibv_context *infelf_open_ib_device(const char *ib_devname) {
    struct ibv_device **dev_list;
    struct ibv_device *ib_dev;
    struct ibv_context *verbs = NULL;
    int i = 0;

    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        ERROR("failed to get IB devices list");
        return NULL;
    }

    if (!ib_devname) {
        ib_dev = *dev_list;
        if (!ib_dev) {
            ERROR("No IB devices found\n");
            goto err0;
        }
        INFO("Connect using first device: %s.", ibv_get_device_name(ib_dev));
    } else {
        for (i = 0; dev_list[i]; ++i)
            if (!strcmp(ibv_get_device_name(dev_list[i]), ib_devname))
                break;
        ib_dev = dev_list[i];
        if (!ib_dev) {
            ERROR("IB device %s not found\n", ib_devname);
            goto err0;
        }
        verbs = ibv_open_device(ib_dev);
    }

err0:
    ibv_free_device_list(dev_list);
    return verbs;
}

int infelf_init(struct infelf_cfg_t *cfg, struct infelf_conn_t *conn)
{
    int ret = 0;
    char gid[64], *p;

    memset(conn, 0, sizeof(*conn));
    conn->use_event_channel = cfg->use_event_channel;
    conn->use_socket_for_rinfo = cfg->use_socket_for_rinfo;

    // Create RDMA resources (pd, channel, cq)
    conn->verbs = infelf_open_ib_device(cfg->ib_devname);
    if (!conn->verbs) {
        ERROR("ibv_open_device() failed: %d\n", errno);
        return -1;
    }
    conn->pd = ibv_alloc_pd(conn->verbs);
    if (!conn->pd) {
        ERROR("ibv_alloc_pd() failed: %d\n", errno);
        goto err0;
        return errno;
    }
    if (conn->use_event_channel) {
        conn->channel = ibv_create_comp_channel(conn->verbs);
        if (!conn->channel) {
            ERROR("ibv_create_comp_channel() failed: %d\n", errno);
            ret = errno;
            goto err1;
        }
    }
    conn->cq = ibv_create_cq(conn->verbs,
        INFELF_WQ_DEPTH + 1, NULL, conn->channel, 0);
    if (!conn->cq) {
        ERROR("ibv_create_cq() failed: %d\n", errno);
        ret = errno;
        goto err2;
    }
    if (conn->use_event_channel) {
        ret = ibv_req_notify_cq(conn->cq, 0);
        if (ret) {
            ERROR("ibv_req_notify_cq() failed: %d\n", errno);
            ret = errno;
            goto err3;
        }
    }

    // Create QP
    struct ibv_qp_init_attr qp_init_attr = {};
    qp_init_attr.send_cq = conn->cq;
    qp_init_attr.recv_cq = conn->cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr = INFELF_SQ_DEPTH;
    qp_init_attr.cap.max_recv_wr = INFELF_WQ_DEPTH;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    conn->qp = ibv_create_qp(conn->pd, &qp_init_attr);
    if (!conn->qp) {
        ERROR("ibv_create_qp() failed: %d\n", errno);
        ret = errno;
        goto err3;
    }
    ret = infelf_pop_node_info(conn, cfg->ib_devname, cfg->ib_port);
    if (ret) {
        return ret;
    }

    p = (char *)&conn->local_info.gid;
    sprintf(gid, "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
            "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
			p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
            p[8],p[9], p[10], p[11], p[12], p[13], p[14], p[15]);
    INFO("local address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
         conn->local_info.lid, conn->local_info.qpn, conn->local_info.psn, gid);

    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_INIT,
        .pkey_index = 0,
        .port_num = conn->local_info.ib_port,
        .qp_access_flags = 0,
    };
    ret = ibv_modify_qp(conn->qp, &attr,
                        IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                        IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) {
        ERROR("ibv_modify_qp() failed: %d\n", errno);
        ret = errno;
        goto err4;
    }

    if (!conn->use_socket_for_rinfo) {
        // Setup SQ/RQ SGL
        struct infelf_rdma_ctrl_t *ctrl = &conn->ctrl;

        ctrl->send_mr =
            ibv_reg_mr(conn->pd, &ctrl->send_buf, sizeof(ctrl->send_buf), 0);
        if (!ctrl->send_mr) {
            ERROR("ibv_reg_mr() failed on send_mr: %d\n", errno);
            ret = errno;
            goto err5;
        }
        ctrl->recv_mr = ibv_reg_mr(conn->pd,
                                   &ctrl->recv_buf,
                                   sizeof(ctrl->recv_buf),
                                   IBV_ACCESS_LOCAL_WRITE);
        if (!ctrl->recv_mr) {
            ERROR("ibv_reg_mr() failed on recv_mr: %d\n", errno);
            ret = errno;
            goto err6;
        }

        ctrl->send_sgl.addr = (uint64_t) (unsigned long) &ctrl->send_buf;
        ctrl->send_sgl.length = sizeof(ctrl->send_buf);
        ctrl->send_sgl.lkey = ctrl->send_mr->lkey;
        ctrl->sq_wr.opcode = IBV_WR_SEND;
        ctrl->sq_wr.send_flags = IBV_SEND_SIGNALED;
        ctrl->sq_wr.sg_list = &ctrl->send_sgl;
        ctrl->sq_wr.num_sge = 1;

        ctrl->recv_sgl.addr = (uint64_t) (unsigned long) &ctrl->recv_buf;
        ctrl->recv_sgl.length = sizeof(ctrl->recv_buf);
        ctrl->recv_sgl.lkey = ctrl->recv_mr->lkey;
        ctrl->rq_wr.sg_list = &ctrl->recv_sgl;
        ctrl->rq_wr.num_sge = 1;
    }

    return 0;

err6:
    ibv_dereg_mr(conn->ctrl.recv_mr);
err5:
    ibv_dereg_mr(conn->ctrl.send_mr);
err4:
    ibv_destroy_qp(conn->qp);
err3:
    ibv_destroy_cq(conn->cq);
err2:
    if (conn->use_event_channel) {
        ibv_destroy_comp_channel(conn->channel);
    }
err1:
    ibv_dealloc_pd(conn->pd);
err0:
    ibv_close_device(conn->verbs);
    return ret;
}

int infelf_connect(struct infelf_conn_t *conn)
{
    int flags;

    // Move QP to RTR
    struct ibv_qp_attr attr = {
        .qp_state = IBV_QPS_RTR,
        .path_mtu = IBV_MTU_1024, // TODO (fix MTU discovery)
        .dest_qp_num = conn->remote_info.qpn,
        .rq_psn = conn->remote_info.psn,
        .max_dest_rd_atomic = 1,
        .min_rnr_timer = 12,
        .ah_attr = {
            .is_global = 0,
            .dlid = conn->remote_info.lid,
            .sl = 0,
            .src_path_bits = 0,
            .port_num = conn->local_info.ib_port
        }
    };
    if (conn->remote_info.gid.global.interface_id) {
        attr.ah_attr.is_global = 1;
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.dgid = conn->remote_info.gid;
        attr.ah_attr.grh.sgid_index = conn->local_info.ib_gidx;
    }
    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
            IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(conn->qp, &attr, flags)) {
        ERROR("ibv_modify_qp() failed: %d. Move QP to RTR.\n", errno);
        return -1;
    }

    // Move QP to RTS
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = conn->local_info.psn;
    attr.max_rd_atomic = 1;
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(conn->qp, &attr, flags)) {
        ERROR("ibv_modify_qp() failed: %d. Move QP to RTS.\n", errno);
        return -1;
    }

    return 0;
}

int infelf_close(struct infelf_conn_t *conn)
{
    if (conn->use_socket_for_rinfo) {
        struct infelf_rdma_ctrl_t *ctrl = &conn->ctrl;
        if (ibv_dereg_mr(ctrl->send_mr)) {
            ERROR("ibv_dereg_mr() failed on ctrl.send_mr: %d\n", errno);
            return -1;
        }
        if (ibv_dereg_mr(ctrl->recv_mr)) {
            ERROR("ibv_dereg_mr() failed on ctrl.recv_mr: %d\n", errno);
            return -1;
        }
    }
    if (ibv_destroy_qp(conn->qp)) {
        ERROR("ibv_destroy_qp() failed: %d\n", errno);
        return -1;
    }
    if (ibv_destroy_cq(conn->cq)) {
        ERROR("ibv_destroy_qp() failed: %d\n", errno);
        return -1;
    }
    if (ibv_dereg_mr(conn->rdma_mr)) {
        ERROR("ibv_dereg_mr() failed on rdma_mr: %d\n", errno);
        return -1;
    }
    if (ibv_dealloc_pd(conn->pd)) {
        ERROR("ibv_dealloc_pd() failed: %d\n", errno);
        return -1;
    }
    if (conn->channel) {
        if (ibv_destroy_comp_channel(conn->channel)) {
            ERROR("ibv_destroy_comp_channel() failed: %d\n", errno);
            return -1;
        }
    }
    if (ibv_close_device(conn->verbs)) {
        ERROR("ibv_close_device() failed: %d\n", errno);
        return -1;
    }

    return 0;
}

int infelf_alloc_buffers(struct infelf_conn_t *conn,
                         int size,
                         int gpu_dev_id)
{
    int ret;

    /* TODO: Use memalign also for buffers on CPU
     *  int page_size = sysconf(_SC_PAGESIZE);
     *  buff = memalign(page_size, length);
     */
    /* Allocate buffer from GPU if gpu_dev_id >= 0 */
    if (gpu_dev_id >= 0) {
        conn->is_gpu_buffer = 1;
    }
    conn->rdma_buf_size = size;
    conn->rdma_buf = (conn->is_gpu_buffer) ?
        infelf_gpu_buffer_alloc(size, gpu_dev_id) : malloc(size);
    if (!conn->rdma_buf) {
        ERROR("buf malloc failed on %s\n",
              (gpu_dev_id >= 0) ? "GPU" : "CPU");
        ret = -ENOMEM;
    }
    conn->rdma_mr = ibv_reg_mr(conn->pd, conn->rdma_buf, size,
                               IBV_ACCESS_LOCAL_WRITE |
                               IBV_ACCESS_REMOTE_READ |
                               IBV_ACCESS_REMOTE_WRITE);
    if (!conn->rdma_mr) {
        ERROR("ibv_reg_mr() failed: %d\n", errno);
        ret = errno;
        goto err0;
    }

    conn->rdma_sgl.addr = (uint64_t) (unsigned long) conn->rdma_buf;
    conn->rdma_sgl.lkey = conn->rdma_mr->lkey;
    conn->rdma_sq_wr.send_flags = IBV_SEND_SIGNALED;
    conn->rdma_sq_wr.sg_list = &conn->rdma_sgl;
    conn->rdma_sq_wr.num_sge = 1;

    return 0;

err0:
    if (conn->is_gpu_buffer) {
        infelf_gpu_buffer_free(conn->rdma_buf);
    } else {
        free(conn->rdma_buf);
    }
    return ret;
}

void infelf_free_buffers(struct infelf_conn_t *conn)
{
    if (!conn->use_socket_for_rinfo) {
        ibv_dereg_mr(conn->ctrl.recv_mr);
        ibv_dereg_mr(conn->ctrl.send_mr);
    }
    ibv_dereg_mr(conn->rdma_mr);
    if (conn->is_gpu_buffer) {
        infelf_gpu_buffer_free(conn->rdma_buf);
    } else {
        free(conn->rdma_buf);
    }
}

int infelf_rdma_read(struct infelf_conn_t *conn)
{
    struct ibv_send_wr *bad_wr;
    int ret;

    conn->rdma_sq_wr.opcode = IBV_WR_RDMA_READ;
    conn->rdma_sq_wr.wr.rdma.rkey = conn->tx.rkey;
    conn->rdma_sq_wr.wr.rdma.remote_addr = conn->tx.buf;
    conn->rdma_sq_wr.sg_list->length = conn->tx.size;
    ret = ibv_post_send(conn->qp, &conn->rdma_sq_wr, &bad_wr);
    if (ret) {
        ERROR("ibv_post_send() failed on IBV_WR_RDMA_READ: %d\n", errno);
        return -1;
    }

    return 0;
}

int infelf_rdma_write_with_imm(struct infelf_conn_t *conn, int imm)
{
    struct ibv_send_wr *bad_wr;
    int ret;

    conn->rdma_sq_wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
    conn->rdma_sq_wr.imm_data = htonl(imm);
    conn->rdma_sq_wr.wr.rdma.rkey = conn->tx.rkey;
    conn->rdma_sq_wr.wr.rdma.remote_addr = conn->tx.buf;
    conn->rdma_sq_wr.sg_list->length = strlen(conn->rdma_buf) + 1;
    ret = ibv_post_send(conn->qp, &conn->rdma_sq_wr, &bad_wr);
    if (ret) {
        ERROR("ibv_post_send() failed on IBV_WR_RDMA_WRITE_WITH_IMM: %d\n",
              errno);
        return -1;
    }

    return 0;
}
