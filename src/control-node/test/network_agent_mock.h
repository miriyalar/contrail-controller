/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef __ctrlplane__network_agent_mock__
#define __ctrlplane__network_agent_mock__

#include <boost/scoped_ptr.hpp>
#include <map>
#include <pugixml/pugixml.hpp>
#include <tbb/compat/condition_variable>
#include <tbb/mutex.h>

#include "base/queue_task.h"

namespace autogen {
struct ItemType;
struct EnetItemType;
class VirtualRouter;
class VirtualMachine;
}

namespace pugi {
class xml_document;
class xml_node;
}

class EventManager;
class XmppChannelConfig;
class XmppClient;
class BgpXmppChannelManager;

namespace test {

class XmppDocumentMock {
public:
    static const char *kControlNodeJID;
    static const char *kNetworkServiceJID;
    static const char *kConfigurationServiceJID;
    static const char *kPubSubNS;

    XmppDocumentMock(const std::string &hostname);
    pugi::xml_document *RouteAddXmlDoc(const std::string &network, 
                                       const std::string &prefix,
                                       const std::string nexthop = "");
    pugi::xml_document *RouteDeleteXmlDoc(const std::string &network, 
                                          const std::string &prefix,
                                          const std::string nexthop = "");
    pugi::xml_document *RouteEnetAddXmlDoc(const std::string &network,
                                           const std::string &prefix,
                                           const std::string nexthop = "");
    pugi::xml_document *RouteEnetDeleteXmlDoc(const std::string &network,
                                              const std::string &prefix,
                                              const std::string nexthop = "");
    pugi::xml_document *RouteMcastAddXmlDoc(const std::string &network, 
                                            const std::string &sg,
                                            const std::string &nexthop,
                                            const std::string &label_range);
    pugi::xml_document *RouteMcastDeleteXmlDoc(const std::string &network, 
                                               const std::string &sg);
    pugi::xml_document *SubscribeXmlDoc(const std::string &network, int id,
                                        std::string type = kNetworkServiceJID);
    pugi::xml_document *UnsubscribeXmlDoc(const std::string &network, int id,
                                        std::string type = kNetworkServiceJID);

    const std::string &hostname() const { return hostname_; }
    const std::string &localaddr() const { return localaddr_; }

    void set_localaddr(const std::string &addr) { localaddr_ = addr; }

private:
    pugi::xml_node PubSubHeader(std::string type);
    pugi::xml_document *SubUnsubXmlDoc(
            const std::string &network, int id, bool sub, std::string type);
    pugi::xml_document *RouteAddDeleteXmlDoc(const std::string &network,
            const std::string &prefix, bool add, const std::string nexthop);
    pugi::xml_document *RouteEnetAddDeleteXmlDoc(const std::string &network,
            const std::string &prefix, const std::string nexthop, bool add);
    pugi::xml_document *RouteMcastAddDeleteXmlDoc(const std::string &network,
            const std::string &sg, const std::string &nexthop,
            const std::string &label_range, bool add);

    std::string hostname_;
    int label_alloc_;
    std::string localaddr_;
    boost::scoped_ptr<pugi::xml_document> xdoc_;
};

class NetworkAgentMock {
private:
    class AgentPeer;
public:
    typedef autogen::ItemType RouteEntry;
    typedef std::map<std::string, RouteEntry *> RouteTable;

    typedef autogen::EnetItemType EnetRouteEntry;
    typedef std::map<std::string, EnetRouteEntry *> EnetRouteTable;

    typedef autogen::VirtualRouter VRouterEntry;
    typedef std::map<std::string, VRouterEntry *> VRouterTable;

    typedef autogen::VirtualMachine VMEntry;
    typedef std::map<std::string, VMEntry *> VMTable;

    template <typename T>
    class Instance {
    public:
        typedef std::map<std::string, T *> TableMap;
        Instance();
        virtual ~Instance();
        void Update(long count);
        void Update(const std::string &node, T *entry);
        void Remove(const std::string &node);
        void Clear();
        int Count() const;
        const T *Lookup(const std::string &node) const;
    private:
        size_t count_;
        TableMap table_;
    };

    template <typename T>
    class InstanceMgr {
        public:
            typedef std::map<std::string, Instance<T> *> InstanceMap;

            InstanceMgr(NetworkAgentMock *parent, std::string type) {
                parent_ = parent;
                type_ = type;
            }

            bool HasSubscribed(const std::string &network);
            void Subscribe(const std::string &network, int id = -1,
                           bool wait_for_established = true);
            void Unsubscribe(const std::string &network, int id = -1,
                             bool wait_for_established = true);
            void Update(const std::string &network, long count);
            void Update(const std::string &network,
                        const std::string &node_name, T *rt_entry);
            void Remove(const std::string &network,
                        const std::string &node_name);
            int Count(const std::string &network) const;
            int Count() const;
            void Clear();
            const T *Lookup(const std::string &network,
                    const std::string &prefix) const;

        private:
            NetworkAgentMock *parent_;
            std::string type_;
            InstanceMap instance_map_;
    };

    NetworkAgentMock(EventManager *evm, const std::string &hostname,
                     int server_port, std::string local_address = "127.0.0.1",
                     std::string server_address = "127.0.0.1");
    ~NetworkAgentMock();

    bool skip_updates_processing() { return skip_updates_processing_; }
    void set_skip_updates_processing(bool set) {
        skip_updates_processing_ = set;
    }
    void SessionDown();
    void SessionUp();

    void Subscribe(const std::string &network, int id = -1,
                   bool wait_for_established = true) {
        route_mgr_->Subscribe(network, id, wait_for_established);
    }
    void Unsubscribe(const std::string &network, int id = -1,
                     bool wait_for_established = true) {
        route_mgr_->Unsubscribe(network, id, wait_for_established);
    }

    int RouteCount(const std::string &network) const;
    int RouteCount() const;
    const RouteEntry *RouteLookup(const std::string &network,
                                  const std::string &prefix) const {
        return route_mgr_->Lookup(network, prefix);
    }

    void AddRoute(const std::string &network, const std::string &prefix,
                  const std::string nexthop = "");
    void DeleteRoute(const std::string &network, const std::string &prefix,
                     const std::string nexthop = "");

    void EnetSubscribe(const std::string &network, int id = -1,
                       bool wait_for_established = true) {
        enet_route_mgr_->Subscribe(network, id, wait_for_established);
    }
    void EnetUnsubscribe(const std::string &network, int id = -1,
                         bool wait_for_established = true) {
        enet_route_mgr_->Unsubscribe(network, id, wait_for_established);
    }

    int EnetRouteCount(const std::string &network) const;
    int EnetRouteCount() const;
    const EnetRouteEntry *EnetRouteLookup(const std::string &network,
                                          const std::string &prefix) const {
        return enet_route_mgr_->Lookup(network, prefix);
    }

    void AddEnetRoute(const std::string &network, const std::string &prefix,
                      const std::string nexthop = "");
    void DeleteEnetRoute(const std::string &network, const std::string &prefix,
                         const std::string nexthop = "");

    void AddMcastRoute(const std::string &network, const std::string &sg,
                       const std::string &nexthop,
                       const std::string &label_range);
    void DeleteMcastRoute(const std::string &network, const std::string &sg);

    bool IsEstablished();
    bool IsSessionEstablished();
    void ClearInstances();

    const std::string &hostname() const { return impl_->hostname(); }
    const std::string &localaddr() const { return impl_->localaddr(); }
    const std::string ToString() const;

    void set_localaddr(const std::string &addr) { impl_->set_localaddr(addr); }
    XmppClient *client() { return client_; }
    void Delete();
    tbb::mutex &get_mutex() { return mutex_; }
    bool down() { return down_; }

    XmppDocumentMock *GetXmlHandler() { return impl_.get(); }
    const std::string local_address() const { return local_address_; }
    void DisableRead(bool disable_read);

    enum RequestType {
        IS_ESTABLISHED,
    };
    struct Request {
        RequestType type;
        bool result;
    };

    bool ProcessRequest(Request *request);

    boost::scoped_ptr<InstanceMgr<RouteEntry> > route_mgr_;
    boost::scoped_ptr<InstanceMgr<EnetRouteEntry> > enet_route_mgr_;
    boost::scoped_ptr<InstanceMgr<VRouterEntry> > vrouter_mgr_;
    boost::scoped_ptr<InstanceMgr<VMEntry> > vm_mgr_;

private:
    static void Initialize();
    AgentPeer *GetAgent();
    XmppChannelConfig *CreateXmppConfig();

    XmppClient *client_;
    std::auto_ptr<AgentPeer> peer_;
    boost::scoped_ptr<XmppDocumentMock> impl_;

    WorkQueue<Request *> work_queue_;
    std::string server_address_;
    std::string local_address_;
    int server_port_;
    bool skip_updates_processing_;
    bool down_;
    tbb::mutex mutex_;
    tbb::mutex work_mutex_;

    std::condition_variable cond_var_;
};

}  // namespace test

#endif /* defined(__ctrlplane__network_agent_mock__) */
