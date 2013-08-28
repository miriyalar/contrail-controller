/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#include <asm/types.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/genetlink.h>
#include <linux/sockios.h>

#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>

#include <base/logging.h>
#include <db/db.h>
#include <db/db_entry.h>
#include <db/db_table.h>
#include <db/db_table_partition.h>

#include <sandesh/sandesh_types.h>
#include <sandesh/sandesh.h>

#include "ksync_index.h"
#include "ksync_entry.h"
#include "ksync_object.h"
#include "ksync_sock.h"
#include "ksync_sock_user.h"

#include "oper/vrf.h"

#include "nl_util.h"
#include "vr_genetlink.h"
#include "vr_message.h"
#include "vr_types.h"

KSyncSockTypeMap *KSyncSockTypeMap::singleton_; 
vr_flow_entry *KSyncSockTypeMap::flow_table_;
using namespace boost::asio;

//store ops data
void vrouter_ops_test::Process(SandeshContext *context) {
}

//process sandesh messages that are being sent from the agent
//this is used to store a local copy of what is being send to kernel
void KSyncSockTypeMap::ProcessSandesh(const uint8_t *parse_buf, 
                                      KSyncUserSockContext *ctx) {
    struct nlmsghdr *nlh = (struct nlmsghdr *)(parse_buf);
    int total_len = nlh->nlmsg_len;
    int decode_len;
    uint8_t *decode_buf;

    struct genlmsghdr *genlh = (struct genlmsghdr *)(parse_buf + NLMSG_HDRLEN);
    if (genlh->cmd != SANDESH_REQUEST) {
        LOG(ERROR, "Unkown generic netlink cmd : " << genlh->cmd);
        assert(0);
    }

    struct nlattr *attr = (struct nlattr *)(parse_buf + NLMSG_HDRLEN
                                            + GENL_HDRLEN);
    if (attr->nla_type != NL_ATTR_VR_MESSAGE_PROTOCOL) {
        LOG(ERROR, "Unkown generic netlink TLV type : " << attr->nla_type);
        assert(0);
    }

    //parse sandesh
    int err = 0;
    int hdrlen = NLMSG_HDRLEN + GENL_HDRLEN + NLA_HDRLEN;
    int decode_buf_len = total_len - hdrlen;
    decode_buf = (uint8_t *)(parse_buf + hdrlen);
    while(decode_buf_len > (NLA_ALIGNTO - 1)) {
        decode_len = Sandesh::ReceiveBinaryMsgOne(decode_buf, decode_buf_len,
                                                  &err, ctx);
        if (decode_len < 0) {
            LOG(DEBUG, "Incorrect decode len " << decode_len);
            break;
        }
        decode_buf += decode_len;
        decode_buf_len -= decode_len;
    }
}

void KSyncSockTypeMap::FlowNatResponse(uint32_t seq_num, vr_flow_req *req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    struct nl_client cl;
    int error = 0, ret;
    uint8_t *buf = NULL;
    uint32_t buf_len = 0, encode_len;
    struct nlmsghdr *nlh;

    nl_init_generic_client_req(&cl, KSyncSock::GetNetlinkFamilyId());
    if ((ret = nl_build_header(&cl, &buf, &buf_len)) < 0) {
        LOG(DEBUG, "Error creating interface DUMP message : " << ret);
        nl_free(&cl);
        return;
    }

    nlh = (struct nlmsghdr *)cl.cl_buf;
    nlh->nlmsg_seq = seq_num;

    req->set_fr_op(flow_op::FLOW_SET);
    /* Send reverse-flow index as one more than fwd-flow index */
    int fwd_flow_idx = req->get_fr_index();
    req->set_fr_rindex(fwd_flow_idx + 1000);

    encode_len = req->WriteBinary(buf, buf_len, &error);
    if (error != 0) {
        SimulateResponse(seq_num, -ENOENT, 0); 
        nl_free(&cl);
        return;
    }

    nl_update_header(&cl, encode_len);
    LOG(DEBUG, "Sending mock Flow Set Response with seq num " << seq_num << 
        " for fwd-flow " << fwd_flow_idx);
    sock->sock_.send_to(buffer(cl.cl_buf, cl.cl_msg_len), sock->local_ep_);
    nl_free(&cl);

    //Activate the reverse flow-entry in flow mmap
    KSyncSockTypeMap::SetFlowEntry(req->get_fr_rindex(), true);
}

void KSyncSockTypeMap::SendNetlinkDoneMsg(int seq_num) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    struct nlmsghdr nlh;
    nlh.nlmsg_seq = seq_num;
    nlh.nlmsg_type = NLMSG_DONE;
    nlh.nlmsg_len = NLMSG_HDRLEN;
    nlh.nlmsg_flags = 0;
    sock->sock_.send_to(buffer(&nlh, NLMSG_HDRLEN), sock->local_ep_);
}

void KSyncSockTypeMap::SimulateResponse(uint32_t seq_num, int code, int flags) {
    struct nl_client cl;
    vr_response encoder;
    int encode_len, error, ret;
    uint8_t *buf;
    uint32_t buf_len;

    nl_init_generic_client_req(&cl, KSyncSock::GetNetlinkFamilyId());
    if ((ret = nl_build_header(&cl, &buf, &buf_len)) < 0) {
        LOG(DEBUG, "Error creating mpls message. Error : " << ret);
        nl_free(&cl);
        return;
    }

    struct nlmsghdr *nlh = (struct nlmsghdr *)cl.cl_buf;
    nlh->nlmsg_seq = seq_num;
    nlh->nlmsg_flags |= flags;
    encoder.set_h_op(sandesh_op::RESPONSE);
    encoder.set_resp_code(code);
    encode_len = encoder.WriteBinary(buf, buf_len, &error);
    nl_update_header(&cl, encode_len);
    LOG(DEBUG, "SimulateResponse " << " seq " << seq_num << " code " << std::hex << code);

    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    sock->sock_.send_to(buffer(cl.cl_buf, cl.cl_msg_len), sock->local_ep_);
    nl_free(&cl);
}

void KSyncSockTypeMap::IfStatsUpdate(int idx, int ibytes, int ipkts, int ierrors, 
                                     int obytes, int opkts, int oerrors) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    vr_interface_req req = sock->if_map[idx];
    req.set_vifr_ibytes(ibytes+req.get_vifr_ibytes());
    req.set_vifr_ipackets(ipkts+req.get_vifr_ipackets());
    req.set_vifr_ierrors(ierrors+req.get_vifr_ierrors());
    req.set_vifr_obytes(obytes+req.get_vifr_obytes());
    req.set_vifr_opackets(opkts+req.get_vifr_opackets());
    req.set_vifr_oerrors(oerrors+req.get_vifr_oerrors());
    sock->if_map[idx] = req;
}

void KSyncSockTypeMap::IfStatsSet(int idx, int ibytes, int ipkts, int ierrors, 
                                  int obytes, int opkts, int oerrors) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    vr_interface_req req = sock->if_map[idx];
    req.set_vifr_ibytes(ibytes);
    req.set_vifr_ipackets(ipkts);
    req.set_vifr_ierrors(ierrors);
    req.set_vifr_obytes(obytes);
    req.set_vifr_opackets(opkts);
    req.set_vifr_oerrors(oerrors);
    sock->if_map[idx] = req;
}

int KSyncSockTypeMap::IfCount() {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    return sock->if_map.size();
}

int KSyncSockTypeMap::NHCount() {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    return sock->nh_map.size();
}

int KSyncSockTypeMap::MplsCount() {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    return sock->mpls_map.size();
}

int KSyncSockTypeMap::RouteCount() {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    return sock->rt_tree.size();
}

//send or store in map
void KSyncSockTypeMap::AsyncSendTo(mutable_buffers_1 buf, HandlerCb cb) {
    const uint8_t *parse_buf = buffer_cast<const uint8_t *>(buf);
    struct nlmsghdr *nlh = (struct nlmsghdr *)parse_buf;
    KSyncUserSockContext ctx(true, nlh->nlmsg_seq);
    //parse and store info in map [done in Process() callbacks]
    ProcessSandesh(parse_buf, &ctx);

    if (ctx.IsResponseReqd()) {
        //simulate ok response with the same seq
        SimulateResponse(nlh->nlmsg_seq, 0, 0); 
    }
}

//send or store in map
void KSyncSockTypeMap::SendTo(const_buffers_1 buf) {
    const uint8_t *parse_buf = buffer_cast<const uint8_t *>(buf);
    struct nlmsghdr *nlh = (struct nlmsghdr *)parse_buf;
    KSyncUserSockContext ctx(true, nlh->nlmsg_seq);
    //parse and store info in map [done in Process() callbacks]
    KSyncSockTypeMap::ProcessSandesh(parse_buf, &ctx);

    if (ctx.IsResponseReqd()) {
        //simulate ok response with the same seq
        SimulateResponse(nlh->nlmsg_seq, 0, 0); 
    }
}

//receive msgs from datapath
void KSyncSockTypeMap::AsyncReceive(mutable_buffers_1 buf, HandlerCb cb) {
    sock_.async_receive_from(buf, local_ep_, cb);
}

//receive msgs from datapath
void KSyncSockTypeMap::Receive(mutable_buffers_1 buf) {
    sock_.receive(buf);
}

vr_flow_entry *KSyncSockTypeMap::FlowMmapAlloc(int size) {
    flow_table_ = (vr_flow_entry *)malloc(size);
    return flow_table_;
}

void KSyncSockTypeMap::FlowMmapFree() {
    if (flow_table_) {
        free(flow_table_);
        flow_table_ = NULL;
    }
}

vr_flow_entry *KSyncSockTypeMap::GetFlowEntry(int idx) {
    return &flow_table_[idx];
}

void KSyncSockTypeMap::SetFlowEntry(int idx, bool set) {
    vr_flow_entry *f = &flow_table_[idx];
    if (set)  {
        f->fe_flags |= VR_FLOW_FLAG_ACTIVE;
        f->fe_stats.flow_bytes = 30;
        f->fe_stats.flow_packets = 1;
    } else {
        f->fe_flags &= ~VR_FLOW_FLAG_ACTIVE;
        f->fe_stats.flow_bytes = 0;
        f->fe_stats.flow_packets = 0;
    }
}

void KSyncSockTypeMap::IncrFlowStats(int idx, int pkts, int bytes) {
    vr_flow_entry *f = &flow_table_[idx];
    if (f->fe_flags & VR_FLOW_FLAG_ACTIVE) {
        f->fe_stats.flow_bytes += bytes;
        f->fe_stats.flow_packets += pkts;
    }
}

//init ksync map
void KSyncSockTypeMap::Init(boost::asio::io_service &ios) {
    assert(singleton_ == NULL);
    singleton_ = new KSyncSockTypeMap(ios);

    singleton_->local_ep_.address(ip::address::from_string("127.0.0.1"));
    singleton_->local_ep_.port(0);
    singleton_->sock_.open(ip::udp::v4());
    singleton_->sock_.bind(singleton_->local_ep_);
    singleton_->local_ep_ = singleton_->sock_.local_endpoint();

    //update map for Sandesh callback of Process()
    SandeshBaseFactory::map_type update_map;
    update_map["vrouter_ops"] = &createT<vrouter_ops_test>;
    SandeshBaseFactory::Update(update_map);
}

void KSyncSockTypeMap::Shutdown() {
    delete singleton_;
    singleton_ = NULL;
}

void KSyncSockTypeMap::PurgeBlockedMsg() {
    while (!ctx_queue_.empty()) {
        ctx_queue_.front()->Process();
        delete ctx_queue_.front();
        ctx_queue_.pop();
    }
}

void KSyncSockTypeMap::SetBlockMsgProcessing(bool enable) {
    tbb::mutex::scoped_lock lock(ctx_queue_lock_);
    if (block_msg_processing_ != enable) {
        block_msg_processing_ = enable;
        if (!block_msg_processing_) {
            PurgeBlockedMsg();
        }
    }
}

void KSyncUserSockIfContext::Process() {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();

    //delete from map if command is delete
    if (req_->get_h_op() == sandesh_op::DELETE) {
        sock->if_map.erase(req_->get_vifr_idx());
    } else if (req_->get_h_op() == sandesh_op::DUMP) {
        IfDumpHandler dump;
        dump.SendDumpResponse(GetSeqNum(), req_);
        return;
    } else if (req_->get_h_op() == sandesh_op::GET) {
        IfDumpHandler dump;
        dump.SendGetResponse(GetSeqNum(), req_->get_vifr_idx());
        return;
    } else {
        //store in the map
        vr_interface_req if_info(*req_);
        sock->if_map[req_->get_vifr_idx()] = if_info;
    }
    KSyncSockTypeMap::SimulateResponse(GetSeqNum(), 0, 0); 
}

void KSyncUserSockContext::IfMsgHandler(vr_interface_req *req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncUserSockIfContext *ifctx = new KSyncUserSockIfContext(GetSeqNum(), req);

    if (sock->IsBlockMsgProcessing()) {
        tbb::mutex::scoped_lock lock(sock->ctx_queue_lock_);
        sock->ctx_queue_.push(ifctx);
    } else {
        ifctx->Process();
        delete ifctx;
    }
    SetResponseReqd(false);
}

void KSyncUserSockFlowContext::Process() {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    uint16_t flags = 0;

    flags = req_->get_fr_flags();
    //delete from map if command is delete
    if (!flags) {
        sock->flow_map.erase(req_->get_fr_index());
        //Deactivate the flow-entry in flow mmap
        KSyncSockTypeMap::SetFlowEntry(req_->get_fr_index(), false);
    } else {
        //store info from binary sandesh message
        vr_flow_req flow_info(*req_);
        sock->flow_map[req_->get_fr_index()] = flow_info;

        //Activate the flow-entry in flow mmap
        KSyncSockTypeMap::SetFlowEntry(req_->get_fr_index(), true);

        // For NAT flow, don't send vr_response, instead send
        // vr_flow_req with index of reverse_flow
        if (flags & VR_FLOW_FLAG_VRFT) {
            SetResponseReqd(false);
            KSyncSockTypeMap::FlowNatResponse(GetSeqNum(), req_);
            return;
        }
    }
    KSyncSockTypeMap::SimulateResponse(GetSeqNum(), 0, 0); 
}

void KSyncUserSockContext::FlowMsgHandler(vr_flow_req *req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncUserSockFlowContext *flowctx = new KSyncUserSockFlowContext(GetSeqNum(), req);

    if (sock->IsBlockMsgProcessing()) {
        tbb::mutex::scoped_lock lock(sock->ctx_queue_lock_);
        sock->ctx_queue_.push(flowctx);
    } else {
        flowctx->Process();
        delete flowctx;
    }
    SetResponseReqd(false);
}

void KSyncUserSockNHContext::Process() {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();

    //delete from map if command is delete
    if (req_->get_h_op() == sandesh_op::DELETE) {
        sock->nh_map.erase(req_->get_nhr_id());
    } else if (req_->get_h_op() == sandesh_op::DUMP) {
        NHDumpHandler dump;
        dump.SendDumpResponse(GetSeqNum(), req_);
        return;
    } else if (req_->get_h_op() == sandesh_op::GET) {
        NHDumpHandler dump;
        dump.SendGetResponse(GetSeqNum(), req_->get_nhr_id());
        return;
    } else {
        //store in the map
        vr_nexthop_req nh_info(*req_);
        sock->nh_map[req_->get_nhr_id()] = nh_info;
    }
    KSyncSockTypeMap::SimulateResponse(GetSeqNum(), 0, 0);
}

void KSyncUserSockContext::NHMsgHandler(vr_nexthop_req *req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncUserSockNHContext *nhctx = new KSyncUserSockNHContext(GetSeqNum(), req);

    if (sock->IsBlockMsgProcessing()) {
        tbb::mutex::scoped_lock lock(sock->ctx_queue_lock_);
        sock->ctx_queue_.push(nhctx);
    } else {
        nhctx->Process();
        delete nhctx;
    }
    SetResponseReqd(false);
}

void KSyncUserSockMplsContext::Process() {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();

    //delete from map mpls command is delete
    if (req_->get_h_op() == sandesh_op::DELETE) {
        sock->mpls_map.erase(req_->get_mr_label());
    } else if (req_->get_h_op() == sandesh_op::DUMP) {
        MplsDumpHandler dump;
        dump.SendDumpResponse(GetSeqNum(), req_);
        return;
    } else if (req_->get_h_op() == sandesh_op::GET) {
        MplsDumpHandler dump;
        dump.SendGetResponse(GetSeqNum(), req_->get_mr_label());
        return;
    } else {
        //store in the map
        vr_mpls_req mpls_info(*req_);
        sock->mpls_map[req_->get_mr_label()] = mpls_info;
    }
    KSyncSockTypeMap::SimulateResponse(GetSeqNum(), 0, 0); 
}

void KSyncUserSockContext::MplsMsgHandler(vr_mpls_req *req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncUserSockMplsContext *mplsctx = new KSyncUserSockMplsContext(GetSeqNum(), req);

    if (sock->IsBlockMsgProcessing()) {
        tbb::mutex::scoped_lock lock(sock->ctx_queue_lock_);
        sock->ctx_queue_.push(mplsctx);
    } else {
        mplsctx->Process();
        delete mplsctx;
    }
    SetResponseReqd(false);
}

void KSyncUserSockRouteContext::Process() {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();

    //delete from the route tree, if the command is delete
    if (req_->get_h_op() == sandesh_op::DELETE) {
        sock->rt_tree.erase(*req_);
    } else if (req_->get_h_op() == sandesh_op::DUMP) {
        RouteDumpHandler dump;
        dump.SendDumpResponse(GetSeqNum(), req_);
        return;
    } else {
        //store in the route tree
        std::pair<std::set<vr_route_req>::iterator, bool> ret;
        ret = sock->rt_tree.insert(*req_);

        /* If insertion fails, remove the existing entry and add the new one */
        if (ret.second == false) {
            int del_count = sock->rt_tree.erase(*req_);
            assert(del_count);
            ret = sock->rt_tree.insert(*req_);
            assert(ret.second == true);
        }
    }
    KSyncSockTypeMap::SimulateResponse(GetSeqNum(), 0, 0); 
}

void KSyncUserSockContext::RouteMsgHandler(vr_route_req *req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncUserSockRouteContext *rtctx = new KSyncUserSockRouteContext(GetSeqNum(), req);

    if (sock->IsBlockMsgProcessing()) {
        tbb::mutex::scoped_lock lock(sock->ctx_queue_lock_);
        sock->ctx_queue_.push(rtctx);
    } else {
        rtctx->Process();
        delete rtctx;
    }
    SetResponseReqd(false);
}

void KSyncUserSockContext::MirrorMsgHandler(vr_mirror_req *req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();

    //delete from map if command is delete
    if (req->get_h_op() == sandesh_op::DELETE) {
        sock->mirror_map.erase(req->get_mirr_index());
        return;
    }

    if (req->get_h_op() == sandesh_op::DUMP) {
        SetResponseReqd(false);
        MirrorDumpHandler dump;
        dump.SendDumpResponse(GetSeqNum(), req);
        return;
    }

    if (req->get_h_op() == sandesh_op::GET) {
        SetResponseReqd(false);
        MirrorDumpHandler dump;
        dump.SendGetResponse(GetSeqNum(), req->get_mirr_index());
        return;
    }

    //store in the map
    vr_mirror_req mirror_info(*req);
    sock->mirror_map[req->get_mirr_index()] = mirror_info;

}

void KSyncUserSockVrfAssignContext::Process() {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();

    //delete from the vrf assign tree, if the command is delete
    if (req_->get_h_op() == sandesh_op::DELETE) {
        sock->vrf_assign_tree.erase(*req_);
    } else if (req_->get_h_op() == sandesh_op::DUMP) {
        VrfAssignDumpHandler dump;
        dump.SendDumpResponse(GetSeqNum(), req_);
        return;
    } else {
        //store in the vrf assign tree
        std::pair<std::set<vr_vrf_assign_req>::iterator, bool> ret;
        ret = sock->vrf_assign_tree.insert(*req_);

        /* If insertion fails, remove the existing entry and add the new one */
        if (ret.second == false) {
            int del_count = sock->vrf_assign_tree.erase(*req_);
            assert(del_count);
            ret = sock->vrf_assign_tree.insert(*req_);
            assert(ret.second == true);
        }
    }
    KSyncSockTypeMap::SimulateResponse(GetSeqNum(), 0, 0); 
}

void KSyncUserSockContext::VrfAssignMsgHandler(vr_vrf_assign_req *req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncUserSockVrfAssignContext *ctx = 
        new KSyncUserSockVrfAssignContext(GetSeqNum(), req);

    if (sock->IsBlockMsgProcessing()) {
        tbb::mutex::scoped_lock lock(sock->ctx_queue_lock_);
        sock->ctx_queue_.push(ctx);
    } else {
        ctx->Process();
        delete ctx;
    }
    SetResponseReqd(false);
}

void MockDumpHandlerBase::SendDumpResponse(uint32_t seq_num, Sandesh *from_req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    struct nl_client cl;
    int error = 0, ret;
    uint8_t *buf = NULL;
    uint32_t buf_len = 0, encode_len = 0, tot_encode_len = 0;
    struct nlmsghdr *nlh;
    bool more = false;
    int count = 0;
    unsigned int resp_code = 0;

    Sandesh *req = GetFirst(from_req);
    if (req != NULL) {
        nl_init_generic_client_req(&cl, KSyncSock::GetNetlinkFamilyId());
        if ((ret = nl_build_header(&cl, &buf, &buf_len)) < 0) {
            LOG(DEBUG, "Error creating interface DUMP message : " << ret);
            nl_free(&cl);
            return;
        }

        nlh = (struct nlmsghdr *)cl.cl_buf;
        nlh->nlmsg_seq = seq_num;
    }

    while(req != NULL) {
        encode_len = req->WriteBinary(buf, buf_len, &error);
        if (error != 0) {
            break;
        }
        buf += encode_len;
        buf_len -= encode_len;
        tot_encode_len += encode_len;
        count++;

        req = GetNext(req);
        //If the remaining buffer length cannot accomodate any more encoded
        //messages, quit from the loop.
        if (req != NULL && buf_len < encode_len) {
            more = true;
            break;
        }
    }

    if (error) {
        KSyncSockTypeMap::SimulateResponse(seq_num, -ENOENT, 0); 
        nl_free(&cl);
        return;
    }

    resp_code = count;
    if (count > 0) {
        resp_code = count;
        if (more) {
            resp_code = resp_code | VR_MESSAGE_DUMP_INCOMPLETE;
        }
        //Send Vr-Response (with multi-flag set)
        KSyncSockTypeMap::SimulateResponse(seq_num, resp_code, NLM_F_MULTI);

        //Send dump-response containing objects (with multi-flag set)
        nlh->nlmsg_flags |= NLM_F_MULTI;
        nl_update_header(&cl, tot_encode_len);
        sock->sock_.send_to(buffer(cl.cl_buf, cl.cl_msg_len), sock->local_ep_);
        nl_free(&cl);
        //Send Netlink-Done message
        KSyncSockTypeMap::SendNetlinkDoneMsg(seq_num);
    } else {
        KSyncSockTypeMap::SimulateResponse(seq_num, resp_code, 0);
    }
}

void MockDumpHandlerBase::SendGetResponse(uint32_t seq_num, int idx) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    struct nl_client cl;
    int error = 0, ret;
    uint8_t *buf = NULL;
    uint32_t buf_len = 0, encode_len = 0;
    struct nlmsghdr *nlh;

    Sandesh *req = Get(idx);
    if (req == NULL) {
        KSyncSockTypeMap::SimulateResponse(seq_num, -ENOENT, 0); 
        return;
    }
    nl_init_generic_client_req(&cl, KSyncSock::GetNetlinkFamilyId());
    if ((ret = nl_build_header(&cl, &buf, &buf_len)) < 0) {
        LOG(DEBUG, "Error creating interface DUMP message : " << ret);
        nl_free(&cl);
        return;
    }

    nlh = (struct nlmsghdr *)cl.cl_buf;
    nlh->nlmsg_seq = seq_num;

    encode_len = req->WriteBinary(buf, buf_len, &error);
    if (error) {
        KSyncSockTypeMap::SimulateResponse(seq_num, -ENOENT, 0); 
        nl_free(&cl);
        return;
    }
    buf += encode_len;
    buf_len -= encode_len;

    nl_update_header(&cl, encode_len);
    sock->sock_.send_to(buffer(cl.cl_buf, cl.cl_msg_len), sock->local_ep_);
    nl_free(&cl);
}

Sandesh* IfDumpHandler::Get(int idx) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_if::const_iterator it;
    static vr_interface_req req;

    it = sock->if_map.find(idx);
    if (it != sock->if_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* IfDumpHandler::GetFirst(Sandesh *from_req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_if::const_iterator it;
    static vr_interface_req req;
    int idx;
    vr_interface_req *orig_req;
    orig_req = static_cast<vr_interface_req *>(from_req);

    idx = orig_req->get_vifr_marker();
    it = sock->if_map.upper_bound(idx);

    if (it != sock->if_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* IfDumpHandler::GetNext(Sandesh *input) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_if::const_iterator it;
    static vr_interface_req req, *r;

    r = static_cast<vr_interface_req *>(input);
    it = sock->if_map.upper_bound(r->get_vifr_idx());

    if (it != sock->if_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* NHDumpHandler::Get(int idx) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_nh::const_iterator it;
    static vr_nexthop_req req;

    it = sock->nh_map.find(idx);
    if (it != sock->nh_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* NHDumpHandler::GetFirst(Sandesh *from_req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_nh::const_iterator it;
    static vr_nexthop_req req;
    vr_nexthop_req *orig_req;
    orig_req = static_cast<vr_nexthop_req *>(from_req);
    int idx;

    idx = orig_req->get_nhr_marker();
    it = sock->nh_map.upper_bound(idx);
    if (it != sock->nh_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* NHDumpHandler::GetNext(Sandesh *input) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_nh::const_iterator it;
    static vr_nexthop_req req, *r;

    r = static_cast<vr_nexthop_req *>(input);
    it = sock->nh_map.upper_bound(r->get_nhr_id());

    if (it != sock->nh_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* MplsDumpHandler::Get(int idx) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_mpls::const_iterator it;
    static vr_mpls_req req;

    it = sock->mpls_map.find(idx);
    if (it != sock->mpls_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* MplsDumpHandler::GetFirst(Sandesh *from_req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_mpls::const_iterator it;
    static vr_mpls_req req;
    vr_mpls_req *orig_req;
    orig_req = static_cast<vr_mpls_req *>(from_req);
    int idx;

    idx = orig_req->get_mr_marker();
    it = sock->mpls_map.upper_bound(idx);

    if (it != sock->mpls_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* MplsDumpHandler::GetNext(Sandesh *input) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_mpls::const_iterator it;
    static vr_mpls_req req, *r;

    r = static_cast<vr_mpls_req *>(input);
    it = sock->mpls_map.upper_bound(r->get_mr_label());

    if (it != sock->mpls_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* MirrorDumpHandler::Get(int idx) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_mirror::const_iterator it;
    static vr_mirror_req req;

    it = sock->mirror_map.find(idx);
    if (it != sock->mirror_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* MirrorDumpHandler::GetFirst(Sandesh *from_req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_mirror::const_iterator it;
    static vr_mirror_req req;
    vr_mirror_req *orig_req;
    orig_req = static_cast<vr_mirror_req *>(from_req);
    int idx;

    idx = orig_req->get_mirr_marker();
    it = sock->mirror_map.upper_bound(idx);

    if (it != sock->mirror_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* MirrorDumpHandler::GetNext(Sandesh *input) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_map_mirror::const_iterator it;
    static vr_mirror_req req, *r;

    r = static_cast<vr_mirror_req *>(input);
    it = sock->mirror_map.upper_bound(r->get_mirr_index());

    if (it != sock->mirror_map.end()) {
        req = it->second;
        return &req;
    }
    return NULL;
}

Sandesh* RouteDumpHandler::GetFirst(Sandesh *from_req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_rt_tree::const_iterator it;
    static vr_route_req req;
    vr_route_req *orig_req, key;
    orig_req = static_cast<vr_route_req *>(from_req);

    if (orig_req->get_rtr_marker()) {
        key.set_rtr_vrf_id(orig_req->get_rtr_vrf_id());
        key.set_rtr_prefix(orig_req->get_rtr_marker());
        key.set_rtr_prefix_len(orig_req->get_rtr_marker_plen());
        it = sock->rt_tree.upper_bound(key);
    } else {
        key.set_rtr_vrf_id(orig_req->get_rtr_vrf_id());
        key.set_rtr_prefix(0);
        key.set_rtr_prefix_len(0);
        it = sock->rt_tree.lower_bound(key);
    }


    if (it != sock->rt_tree.end()) {
        req = *it;
        return &req;
    }
    return NULL;
}

Sandesh* RouteDumpHandler::GetNext(Sandesh *input) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_rt_tree::const_iterator it;
    static vr_route_req req, *r, key;

    r = static_cast<vr_route_req *>(input);

    key.set_rtr_vrf_id(r->get_rtr_vrf_id());
    key.set_rtr_prefix(r->get_rtr_prefix());
    key.set_rtr_prefix_len(r->get_rtr_prefix_len());
    it = sock->rt_tree.upper_bound(key);

    if (it != sock->rt_tree.end()) {
        req = *it;
        return &req;
    }
    return NULL;
}

Sandesh* VrfAssignDumpHandler::GetFirst(Sandesh *from_req) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_vrf_assign_tree::const_iterator it;
    static vr_vrf_assign_req req;
    vr_vrf_assign_req *orig_req, key;
    orig_req = static_cast<vr_vrf_assign_req *>(from_req);

    key.set_var_vif_index(orig_req->get_var_vif_index());
    key.set_var_vlan_id(orig_req->get_var_marker());
    it = sock->vrf_assign_tree.upper_bound(key);

    if (it != sock->vrf_assign_tree.end()) {
        req = *it;
        return &req;
    }
    return NULL;
}

Sandesh* VrfAssignDumpHandler::GetNext(Sandesh *input) {
    KSyncSockTypeMap *sock = KSyncSockTypeMap::GetKSyncSockTypeMap();
    KSyncSockTypeMap::ksync_vrf_assign_tree::const_iterator it;
    static vr_vrf_assign_req req, *r, key;

    r = static_cast<vr_vrf_assign_req *>(input);

    key.set_var_vif_index(r->get_var_vif_index());
    key.set_var_vlan_id(r->get_var_vlan_id());
    it = sock->vrf_assign_tree.upper_bound(key);

    if (it != sock->vrf_assign_tree.end()) {
        req = *it;
        return &req;
    }
    return NULL;
}
