/*
 * Copyright (c) 2013 Juniper Networks, Inc. All rights reserved.
 */

#ifndef ctrlplane_static_route_h
#define ctrlplane_static_route_h

#include <map>
#include <set>

#include "bgp/bgp_condition_listener.h"
#include "bgp/inet/inet_route.h"
#include "schema/bgp_schema_types.h"

struct StaticRouteRequest;
class StaticRoute;

typedef ConditionMatchPtr StaticRoutePtr;

class StaticRouteMgr {
public:
    // Map of Static Route prefix to the StaticRoute match object
    typedef std::map<Ip4Prefix, StaticRoutePtr> StaticRouteMap;

    StaticRouteMgr(RoutingInstance *instance);
    ~StaticRouteMgr();

    // Config
    void ProcessStaticRouteConfig();
    void UpdateStaticRouteConfig();
    void FlushStaticRouteConfig();
    void LocateStaticRoutePrefix(const autogen::StaticRouteType &cfg);
    void RemoveStaticRoutePrefix(const Ip4Prefix &static_route);
    void StopStaticRouteDone(BgpTable *table, ConditionMatch *info);

    // Work Queue
    static int static_route_task_id_;
    void EnqueueStaticRouteReq(StaticRouteRequest *req);
    bool StaticRouteEventCallback(StaticRouteRequest *req);

    bool ResolvePendingStaticRouteConfig();

    RoutingInstance *routing_instance() { return instance_; }

    const StaticRouteMap &static_route_map() const {
        return static_route_map_;
    }
private:
    RoutingInstance *instance_;
    StaticRouteMap  static_route_map_;
    WorkQueue<StaticRouteRequest *> *static_route_queue_;
    // Task trigger to resolve any pending static route config commit
    boost::scoped_ptr<TaskTrigger> resolve_trigger_;

    DISALLOW_COPY_AND_ASSIGN(StaticRouteMgr);
};

#endif
