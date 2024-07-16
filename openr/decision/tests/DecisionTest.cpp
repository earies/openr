/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cstdio>

#include <fb303/ServiceData.h>
#include <folly/IPAddress.h>
#include <folly/IPAddressV4.h>
#include <folly/IPAddressV6.h>
#include <folly/Optional.h>
#include <folly/Random.h>
#include <folly/futures/Promise.h>
#include <folly/init/Init.h>
#include <gflags/gflags.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/Thrift.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <openr/common/Constants.h>
#include <openr/common/Flags.h>
#include <openr/common/MplsUtil.h>
#include <openr/common/NetworkUtil.h>
#include <openr/common/Util.h>
#include <openr/decision/Decision.h>
#include <openr/decision/RouteUpdate.h>
#include <openr/if/gen-cpp2/OpenrConfig_types.h>
#include <openr/tests/utils/Utils.h>

DEFINE_bool(stress_test, false, "pass this to run the stress test");

using namespace std;
using namespace openr;
using namespace testing;

namespace fb303 = facebook::fb303;

using apache::thrift::CompactSerializer;

namespace {

/// R1 -> R2, R3, R4
const auto adj12 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 100002);
const auto adj12OnlyUsedBy2 = createAdjacency(
    "2",
    "1/2",
    "2/1",
    "fe80::2",
    "192.168.0.2",
    10,
    100002,
    Constants::kDefaultAdjWeight,
    true);
const auto adj12_1 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 1000021);
const auto adj12_2 =
    createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 20, 1000022);
const auto adj13 =
    createAdjacency("3", "1/3", "3/1", "fe80::3", "192.168.0.3", 10, 100003);
const auto adj14 =
    createAdjacency("4", "1/4", "4/1", "fe80::4", "192.168.0.4", 10, 100004);
// R2 -> R1, R3, R4
const auto adj21 =
    createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 100001);
const auto adj21OnlyUsedBy1 = createAdjacency(
    "1",
    "2/1",
    "1/2",
    "fe80::1",
    "192.168.0.1",
    10,
    100001,
    Constants::kDefaultAdjWeight,
    true);
const auto adj23 =
    createAdjacency("3", "2/3", "3/2", "fe80::3", "192.168.0.3", 10, 100003);
const auto adj24 =
    createAdjacency("4", "2/4", "4/2", "fe80::4", "192.168.0.4", 10, 100004);
// R3 -> R1, R2, R4
const auto adj31 =
    createAdjacency("1", "3/1", "1/3", "fe80::1", "192.168.0.1", 10, 100001);
const auto adj31_old =
    createAdjacency("1", "3/1", "1/3", "fe80::1", "192.168.0.1", 10, 1000011);
const auto adj32 =
    createAdjacency("2", "3/2", "2/3", "fe80::2", "192.168.0.2", 10, 100002);
const auto adj34 =
    createAdjacency("4", "3/4", "4/3", "fe80::4", "192.168.0.4", 10, 100004);
// R4 -> R2, R3
const auto adj41 =
    createAdjacency("1", "4/1", "1/4", "fe80::1", "192.168.0.1", 10, 100001);
const auto adj42 =
    createAdjacency("2", "4/2", "2/4", "fe80::2", "192.168.0.2", 10, 100002);
const auto adj43 =
    createAdjacency("3", "4/3", "3/4", "fe80::3", "192.168.0.3", 10, 100003);
// R5 -> R4
const auto adj54 =
    createAdjacency("4", "5/4", "4/5", "fe80::4", "192.168.0.4", 10, 100001);

const auto addr1 = toIpPrefix("::ffff:10.1.1.1/128");
const auto addr2 = toIpPrefix("::ffff:10.2.2.2/128");
const auto addr3 = toIpPrefix("::ffff:10.3.3.3/128");
const auto addr4 = toIpPrefix("::ffff:10.4.4.4/128");
const auto addr5 = toIpPrefix("::ffff:10.4.4.5/128");
const auto addr6 = toIpPrefix("::ffff:10.4.4.6/128");
const auto addr1V4 = toIpPrefix("10.1.1.1/32");
const auto addr2V4 = toIpPrefix("10.2.2.2/32");
const auto addr3V4 = toIpPrefix("10.3.3.3/32");
const auto addr4V4 = toIpPrefix("10.4.4.4/32");

const auto addr1Cidr = toIPNetwork(addr1);
const auto addr2Cidr = toIPNetwork(addr2);
const auto addr2V4Cidr = toIPNetwork(addr2V4);

const auto bgpAddr1 = toIpPrefix("2401:1::10.1.1.1/32");
const auto bgpAddr2 = toIpPrefix("2401:2::10.2.2.2/32");
const auto bgpAddr3 = toIpPrefix("2401:3::10.3.3.3/32");
const auto bgpAddr4 = toIpPrefix("2401:4::10.4.4.4/32");
const auto bgpAddr1V4 = toIpPrefix("10.11.1.1/16");
const auto bgpAddr2V4 = toIpPrefix("10.22.2.2/16");
const auto bgpAddr3V4 = toIpPrefix("10.33.3.3/16");
const auto bgpAddr4V4 = toIpPrefix("10.43.4.4/16");

const auto addr1V4ConfigPrefixEntry =
    createPrefixEntry(addr1, thrift::PrefixType::CONFIG);
const auto addr2VipPrefixEntry =
    createPrefixEntry(addr1, thrift::PrefixType::VIP);

const auto prefixDb1 = createPrefixDb("1", {createPrefixEntry(addr1)});
const auto prefixDb2 = createPrefixDb("2", {createPrefixEntry(addr2)});
const auto prefixDb3 = createPrefixDb("3", {createPrefixEntry(addr3)});
const auto prefixDb4 = createPrefixDb("4", {createPrefixEntry(addr4)});
const auto prefixDb1V4 = createPrefixDb("1", {createPrefixEntry(addr1V4)});
const auto prefixDb2V4 = createPrefixDb("2", {createPrefixEntry(addr2V4)});
const auto prefixDb3V4 = createPrefixDb("3", {createPrefixEntry(addr3V4)});
const auto prefixDb4V4 = createPrefixDb("4", {createPrefixEntry(addr4V4)});

const thrift::MplsAction labelPopAction{
    createMplsAction(thrift::MplsActionCode::POP_AND_LOOKUP)};
const thrift::MplsAction labelPhpAction{
    createMplsAction(thrift::MplsActionCode::PHP)};
const thrift::MplsAction labelSwapAction1{
    createMplsAction(thrift::MplsActionCode::SWAP, 1)};
const thrift::MplsAction labelSwapAction2{
    createMplsAction(thrift::MplsActionCode::SWAP, 2)};
const thrift::MplsAction labelSwapAction3{
    createMplsAction(thrift::MplsActionCode::SWAP, 3)};
const thrift::MplsAction labelSwapAction4{
    createMplsAction(thrift::MplsActionCode::SWAP, 4)};
const thrift::MplsAction labelSwapAction5{
    createMplsAction(thrift::MplsActionCode::SWAP, 5)};

const thrift::NextHopThrift labelPopNextHop{createNextHop(
    toBinaryAddress(folly::IPAddressV6("::")),
    std::nullopt /* ifName */,
    0 /* metric */,
    labelPopAction,
    kTestingAreaName)};

// timeout to wait until decision debounce
// (i.e. spf recalculation, route rebuild) finished
const std::chrono::milliseconds debounceTimeoutMin{10};
const std::chrono::milliseconds debounceTimeoutMax{250};

// Empty Perf Events
const thrift::AdjacencyDatabase kEmptyAdjDb;
const apache::thrift::optional_field_ref<thrift::PerfEvents const&>
    kEmptyPerfEventRef{kEmptyAdjDb.perfEvents()};

thrift::NextHopThrift
createNextHopFromAdj(
    thrift::Adjacency adj,
    bool isV4,
    int32_t metric,
    std::optional<thrift::MplsAction> mplsAction = std::nullopt,
    const std::string& area = kTestingAreaName,
    bool v4OverV6Nexthop = false,
    int64_t weight = 0) {
  return createNextHop(
      isV4 and not v4OverV6Nexthop ? *adj.nextHopV4() : *adj.nextHopV6(),
      *adj.ifName(),
      metric,
      std::move(mplsAction),
      area,
      *adj.otherNodeName(),
      weight);
}

// Note: use unordered_set bcoz paths in a route can be in arbitrary order
using NextHops = unordered_set<thrift::NextHopThrift>;
using RouteMap = unordered_map<
    pair<string /* node name */, string /* prefix or label */>,
    NextHops>;

using PrefixRoutes = unordered_map<
    pair<string /* node name */, string /* prefix or label */>,
    thrift::UnicastRoute>;

// Note: routeMap will be modified
void
fillRouteMap(
    const string& node, RouteMap& routeMap, const DecisionRouteDb& routeDb) {
  for (auto const& [_, entry] : routeDb.unicastRoutes) {
    auto prefix = folly::IPAddress::networkToString(entry.prefix);
    for (const auto& nextHop : entry.nexthops) {
      VLOG(4) << "node: " << node << " prefix: " << prefix << " -> "
              << toString(nextHop);

      routeMap[make_pair(node, prefix)].emplace(nextHop);
    }
  }
  for (auto const& [_, entry] : routeDb.mplsRoutes) {
    auto topLabelStr = std::to_string(entry.label);
    for (const auto& nextHop : entry.nexthops) {
      VLOG(4) << "node: " << node << " label: " << topLabelStr << " -> "
              << toString(nextHop);
      routeMap[make_pair(node, topLabelStr)].emplace(nextHop);
    }
  }
}

void
fillRouteMap(
    const string& node,
    RouteMap& routeMap,
    const thrift::RouteDatabase& routeDb) {
  for (auto const& route : *routeDb.unicastRoutes()) {
    auto prefix = toString(*route.dest());
    for (const auto& nextHop : *route.nextHops()) {
      VLOG(4) << "node: " << node << " prefix: " << prefix << " -> "
              << toString(nextHop);

      routeMap[make_pair(node, prefix)].emplace(nextHop);
    }
  }
  for (auto const& route : *routeDb.mplsRoutes()) {
    auto topLabelStr = std::to_string(*route.topLabel());
    for (const auto& nextHop : *route.nextHops()) {
      VLOG(4) << "node: " << node << " label: " << topLabelStr << " -> "
              << toString(nextHop);
      routeMap[make_pair(node, topLabelStr)].emplace(nextHop);
    }
  }
}

RouteMap
getRouteMap(
    SpfSolver& spfSolver,
    const vector<string>& nodes,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  RouteMap routeMap;

  for (string const& node : nodes) {
    auto routeDb = spfSolver.buildRouteDb(node, areaLinkStates, prefixState);
    if (not routeDb.has_value()) {
      continue;
    }

    fillRouteMap(node, routeMap, routeDb.value());
  }

  return routeMap;
}

// Note: routeMap will be modified
void
fillPrefixRoutes(
    const string& node,
    PrefixRoutes& prefixRoutes,
    const DecisionRouteDb& routeDb) {
  for (auto const& [_, entry] : routeDb.unicastRoutes) {
    auto prefix = folly::IPAddress::networkToString(entry.prefix);
    prefixRoutes[make_pair(node, prefix)] = entry.toThrift();
  }
}

PrefixRoutes
getUnicastRoutes(
    SpfSolver& spfSolver,
    const vector<string>& nodes,
    std::unordered_map<std::string, LinkState> const& areaLinkStates,
    PrefixState const& prefixState) {
  PrefixRoutes prefixRoutes;

  for (string const& node : nodes) {
    auto routeDb = spfSolver.buildRouteDb(node, areaLinkStates, prefixState);
    if (not routeDb.has_value()) {
      continue;
    }

    fillPrefixRoutes(node, prefixRoutes, routeDb.value());
  }

  return prefixRoutes;
}

void
validatePopLabelRoute(
    RouteMap const& routeMap, std::string const& nodeName, int32_t nodeLabel) {
  const std::pair<std::string, std::string> routeKey{
      nodeName, std::to_string(nodeLabel)};
  ASSERT_EQ(1, routeMap.count(routeKey));
  EXPECT_EQ(routeMap.at(routeKey), NextHops({labelPopNextHop}));
}

void
printRouteDb(const std::optional<thrift::RouteDatabase>& routeDb) {
  for (const auto& ucRoute : *routeDb.value().unicastRoutes()) {
    LOG(INFO) << "dest: " << toString(*ucRoute.dest());
    if (ucRoute.adminDistance().has_value()) {
      LOG(INFO) << "ad_dis: "
                << static_cast<int>(ucRoute.adminDistance().value());
    }

    for (const auto& nh : *ucRoute.nextHops()) {
      LOG(INFO) << "nexthops: " << toString(nh);
    }
  }
}

const auto&
getUnicastNextHops(const thrift::UnicastRoute& r) {
  return *r.nextHops();
}

const auto&
getMplsNextHops(const thrift::MplsRoute& r) {
  return *r.nextHops();
}

// DPERECTAED: utility functions provided for old test callsites that once used
// PrefixState::updatePrefixDatabase() expecting all node route advertisments to
// be synced.
//
// In newly written tests, prefer
// PrefixState::updatePrefix() and PrefixState::deletePrefix() for writing
// PrefixState::getReceivedRoutesFiltered() for reading

thrift::PrefixDatabase
getPrefixDbForNode(
    PrefixState const& state,
    std::string const& name,
    std::string const& area = kTestingAreaName) {
  thrift::PrefixDatabase prefixDb;
  prefixDb.thisNodeName() = name;
  thrift::ReceivedRouteFilter filter;
  filter.nodeName() = name;
  filter.areaName() = area;
  for (auto const& routeDetail : state.getReceivedRoutesFiltered(filter)) {
    prefixDb.prefixEntries()->push_back(*routeDetail.routes()->at(0).route());
  }
  return prefixDb;
}

std::unordered_set<folly::CIDRNetwork>
updatePrefixDatabase(
    PrefixState& state,
    thrift::PrefixDatabase const& prefixDb,
    std::string const& area = kTestingAreaName) {
  auto const& nodeName = *prefixDb.thisNodeName();

  std::unordered_set<PrefixKey> oldKeys, newKeys;
  auto oldDb = getPrefixDbForNode(state, prefixDb.thisNodeName().value(), area);
  for (auto const& entry : *oldDb.prefixEntries()) {
    oldKeys.emplace(nodeName, toIPNetwork(*entry.prefix()), area);
  }
  std::unordered_set<folly::CIDRNetwork> changed;

  for (auto const& entry : *prefixDb.prefixEntries()) {
    PrefixKey key(nodeName, toIPNetwork(*entry.prefix()), area);
    changed.merge(state.updatePrefix(key, entry));
    newKeys.insert(std::move(key));
  }

  for (auto const& key : oldKeys) {
    if (not newKeys.count(key)) {
      changed.merge(state.deletePrefix(key));
    }
  }

  return changed;
}

} // anonymous namespace

//
// Create a broken topology where R1 and R2 connect no one
// Expect no routes coming out of the spfSolver
//
TEST(ShortestPathTest, UnreachableNodes) {
  // no adjacency
  auto adjacencyDb1 = createAdjDb("1", {}, 0);
  auto adjacencyDb2 = createAdjDb("2", {}, 0);

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable segment label */,
      true /* enable adj labels */,
      false /* disable LFA */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                   .topologyChanged);

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());

  for (string const& node : {"1", "2"}) {
    auto routeDb = spfSolver.buildRouteDb(node, areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(0, routeDb->unicastRoutes.size());
    EXPECT_EQ(0, routeDb->mplsRoutes.size()); // No label routes
  }
}

/*
 * 1 - 2 - 3, 1 and 3 both originating same prefix
 * 3 originates higher/better metric than 1
 * 0) nothing drained, we should choose 3 (baseline)
 * Independant / separate scenarios
 * 1) Softdrain 3, we should choose 1
 * 2) HardDrain 3, we should choose 1
 * 3) Set drain_metric at 3, we should choose 1
 */
TEST(SpfSolver, DrainedNodeLeastPreferred) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 0);

  std::string nodeName("2");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable segment label */,
      true /* enable adj labels */,
      true /* enableBestRouteSelection */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
  linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName);
  linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);

  // Originate same prefix, pp=100/300, sp=100/300, d=0;
  const auto prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::CONFIG, createMetrics(100, 100, 0));
  auto prefixHighMetric = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::CONFIG, createMetrics(300, 300, 0));
  const auto prefixDb1 = createPrefixDb("1", {prefix});
  const auto prefixDb2 = createPrefixDb("2", {});
  const auto prefixDb3 = createPrefixDb("3", {prefixHighMetric});

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_TRUE(updatePrefixDatabase(prefixState, prefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

  // 0) nothing drained, we should choose 3 (baseline)
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to node 3
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj23, false, *adj23.metric()), nh);
    // check that drain metric is not set, 3 is not drained
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 1) Softdrain 3, we should choose 1
  adjacencyDb3.nodeMetricIncrementVal() = 100;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to node 3
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj21, false, *adj21.metric()), nh);
    // check that drain metric is not set, 1 is not drained
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 2) HardDrain 3, we should choose 1
  adjacencyDb3.nodeMetricIncrementVal() = 0;
  adjacencyDb3.isOverloaded() = true;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to node 1
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj21, false, *adj21.metric()), nh);
    // check that drain metric is not set, 1 is not drained
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 3) Set drain_metric at 3, we should choose 1
  adjacencyDb3.isOverloaded() = false;
  *prefixHighMetric.metrics()->drain_metric() = 1;
  updatePrefixDatabase(prefixState, createPrefixDb("3", {prefixHighMetric}));
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to node 1
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj21, false, *adj21.metric()), nh);
    // check that drain metric is not set, 1 is not drained
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }
}

//
// R1 and R2 are adjacent, and R1 has this declared in its
// adjacency database. However, R1 is missing the AdjDb from
// R2. It should not be able to compute path to R2 in this case.
//
TEST(ShortestPathTest, MissingNeighborAdjacencyDb) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 0);

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable segment label */,
      true /* enable adj labels */,
      false /* disable LFA */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1's AdjDb and all prefixes, but do not
  // mention the R2's AdjDb. Add R2's prefixes though.
  //

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());

  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(0, routeDb->unicastRoutes.size());
  EXPECT_EQ(0, routeDb->mplsRoutes.size());
}

//
// R1 and R2 are adjacent, and R1 has this declared in its
// adjacency database. R1 received AdjacencyDatabase from R2,
// but it missing adjacency to R1. We should not see routes
// from R1 to R2.
//
TEST(ShortestPathTest, EmptyNeighborAdjacencyDb) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 0);
  auto adjacencyDb2 = createAdjDb("2", {}, 0);

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable segment label */,
      true /* enable adj labels */,
      false /* disable LFA */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1's AdjDb and all prefixes, but do not
  // mention the R2's AdjDb. Add R2's prefixes though.
  //

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                   .topologyChanged);
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());

  // dump routes for both nodes, expect no routing entries

  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(0, routeDb->unicastRoutes.size());

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(0, routeDb->unicastRoutes.size());
}

//
// Query route for unknown neighbor. It should return none
//
TEST(ShortestPathTest, UnknownNode) {
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable segment label */,
      true /* enable adj labels */,
      false /* disable LFA */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  PrefixState prefixState;

  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  EXPECT_FALSE(routeDb.has_value());

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  EXPECT_FALSE(routeDb.has_value());
}

/*
 * 1 - 2 - 3, 1 and 3 both originating same prefix
 * 1) 1 is softdrained(50), 2 will reach prefix via 3
 * 2) both 1, 3 softdrained(50), 2 will reach prefix via both
 * 3) drain 1 with 100, 2 will reach via 3
 * 4) undrain 1, 2 will reach via 1
 */

TEST(SpfSolver, NodeSoftDrainedChoice) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 0);

  std::string nodeName("2");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable segment label */,
      true /* enable adj labels */,
      false /* disable LFA */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1, R2, R3 adjacency + prefix dbs
  //
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
  }
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }

  // Originate same prefix
  const auto prefix1 = createPrefixEntry(addr1, thrift::PrefixType::CONFIG);
  const auto prefixDb1 = createPrefixDb("1", {prefix1});
  const auto prefixDb2 = createPrefixDb("2", {});
  const auto prefixDb3 = createPrefixDb("3", {prefix1});

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_TRUE(updatePrefixDatabase(prefixState, prefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

  const unsigned int nodeIncVal50 = 50;
  const unsigned int nodeIncVal100 = 100;

  // 1] Soft Drain 1; 2 should only have one nexthop
  adjacencyDb1.nodeMetricIncrementVal() = nodeIncVal50;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to node 3
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj23, false, *adj23.metric()), nh);
    // check that drain metric is not set, 3 is not drained
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 2] Soft Drain 3, now both 1 and 3 are drained; 2 should have two nexthop
  adjacencyDb3.nodeMetricIncrementVal() = nodeIncVal50;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check two nexthop (ecmp to both drained)
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(2, ribEntry.nexthops.size());
    // check that drain metric is set
    EXPECT_EQ(1, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 3] soft Drain 1 harder (100), 2 will still have both next hop.
  adjacencyDb1.nodeMetricIncrementVal() = nodeIncVal100;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop to 3
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(2, ribEntry.nexthops.size());
    // check that drain metric is set
    EXPECT_EQ(1, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }

  // 3] undrain 1, 3 is still softdrained. Will choose 1
  adjacencyDb1.nodeMetricIncrementVal() = 0;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check one nexthop
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
    const auto nh = *ribEntry.nexthops.cbegin();
    EXPECT_EQ(createNextHopFromAdj(adj21, false, *adj21.metric()), nh);
    // check that drain metric is not set
    EXPECT_EQ(0, *ribEntry.bestPrefixEntry.metrics()->drain_metric());
  }
}

/*
 * 1-2-3, where both 1 and 3 advertise same prefix but 1 is overloaded.
 * 1 and 2 will choose only 3 (despite 1 advertising the prefix itself)
 * 3 will choose itself
 */
TEST(SpfSolver, NodeOverloadRouteChoice) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 3);

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable segment label */,
      true /* enable adj labels */,
      false /* disable LFA */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1, R2, R3 adjacency + prefix dbs
  //
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged); // label changed for node1
  }
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged);
  }
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged);
  }

  // Originate same prefix differently
  const auto prefix1 = createPrefixEntry(addr1, thrift::PrefixType::CONFIG);
  const auto prefix3 = createPrefixEntry(addr1, thrift::PrefixType::VIP);
  const auto prefixDb1 = createPrefixDb("1", {prefix1});
  const auto prefixDb2 = createPrefixDb("2", {});
  const auto prefixDb3 = createPrefixDb("3", {prefix3});

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_TRUE(updatePrefixDatabase(prefixState, prefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

  //
  // dump routes for all nodes. expect one unicast route, no overload
  //
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check two nexthop
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(2, ribEntry.nexthops.size());
  }
  {
    auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(0, routeDb->unicastRoutes.size()); // self originated
  }
  {
    auto routeDb = spfSolver.buildRouteDb("3", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(0, routeDb->unicastRoutes.size()); // self originated
  }

  // Overload node 1
  adjacencyDb1.isOverloaded() = true;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
    EXPECT_FALSE(res.nodeLabelChanged);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    // check two nexthop
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(1, ribEntry.nexthops.size());
  }
  {
    auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    // Not choosing itself even it originates this prefix
    EXPECT_EQ(1, routeDb->unicastRoutes.size());
    const auto ribEntry = routeDb->unicastRoutes.at(toIPNetwork(addr1));
    EXPECT_EQ(prefix3, ribEntry.bestPrefixEntry);
    // let others know that local route has been considered when picking the
    // route (and lost)
    EXPECT_TRUE(ribEntry.localRouteConsidered);
  }
  {
    auto routeDb = spfSolver.buildRouteDb("3", areaLinkStates, prefixState);
    ASSERT_TRUE(routeDb.has_value());
    EXPECT_EQ(0, routeDb->unicastRoutes.size()); // self originated
  }
}

/**
 * Test to verify adjacencyDatabase update
 */
TEST(SpfSolver, AdjacencyUpdate) {
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 2);

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable segment label */,
      true /* enable adj labels */,
      false /* disable LFA */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;
  //
  // Feed SPF solver with R1 and R2's adjacency + prefix dbs
  //

  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged); // label changed for node1
  }
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName);
    EXPECT_TRUE(res.topologyChanged);
    EXPECT_TRUE(res.nodeLabelChanged);
  }
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());

  //
  // dump routes for both nodes, expect 3 route entries (1 unicast, 2 label) on
  // each (node1-label, node2-label)
  //

  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(2, routeDb->mplsRoutes.size()); // node label route

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(2, routeDb->mplsRoutes.size()); // node label route

  //
  // Update adjacency database of node 1 by changing it's nexthops and verift
  // that update properly responds to the event
  //
  adjacencyDb1.adjacencies()[0].nextHopV6() =
      toBinaryAddress("fe80::1234:b00c");
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.linkAttributesChanged);
  }

  //
  // dump routes for both nodes, expect 3 route entries (1 unicast, 2 label) on
  // each (node1-label, node2-label)
  //

  routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(2, routeDb->mplsRoutes.size()); // node label route

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(2, routeDb->mplsRoutes.size()); // node label route

  //
  // Update adjacency database of node 2 by changing it's nexthops and verift
  // that update properly responds to the event (no spf trigger needed)
  //
  *adjacencyDb2.adjacencies()[0].nextHopV6() =
      toBinaryAddress("fe80::5678:b00c");
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_TRUE(res.linkAttributesChanged);
  }

  //
  // dump routes for both nodes, expect 3 route entries (1 unicast, 2 label) on
  // each (node1-label, node2-label)
  //

  routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(2, routeDb->mplsRoutes.size()); // node label route

  routeDb = spfSolver.buildRouteDb("2", areaLinkStates, prefixState);
  ASSERT_TRUE(routeDb.has_value());
  EXPECT_EQ(1, routeDb->unicastRoutes.size());
  EXPECT_EQ(2, routeDb->mplsRoutes.size()); // node label route

  // Change nodeLabel.
  adjacencyDb1.nodeLabel() = 11;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_FALSE(res.linkAttributesChanged);
    EXPECT_TRUE(res.nodeLabelChanged);
  }

  adjacencyDb2.nodeLabel() = 22;
  {
    auto res =
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName);
    EXPECT_FALSE(res.topologyChanged);
    EXPECT_FALSE(res.linkAttributesChanged);
    EXPECT_TRUE(res.nodeLabelChanged);
  }
}

//
// Node-1 connects to 2 but 2 doesn't report bi-directionality
// Node-2 and Node-3 are bi-directionally connected
//
TEST(MplsRoutes, BasicTest) {
  const std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* disable v4 */,
      true /* enable segment label */,
      false /* disable best route selection */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);

  PrefixState prefixState;
  // Add all adjacency DBs
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj23}, 0); // No node label
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 3);

  EXPECT_EQ(
      LinkState::LinkStateChange(false, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName));
  EXPECT_EQ(
      LinkState::LinkStateChange(false, false, false),
      linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName));

  EXPECT_EQ(
      LinkState::LinkStateChange(false, false, false),
      linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName));

  EXPECT_EQ(
      LinkState::LinkStateChange(true, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName));

  auto routeMap =
      getRouteMap(spfSolver, {"1", "2", "3"}, areaLinkStates, prefixState);
  EXPECT_EQ(3, routeMap.size());

  // Validate 1's routes
  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel());

  // Validate 2's routes (no node label route)

  // Validate 3's routes
  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel());
}

/**
 * node1 connects to node2 and node3. Both are same distance away (10). Both
 * node2 and node3 announces prefix1 with same metric vector. Routes for prefix1
 * is inspected on node1 at each step. Test outline follows
 *
 * 1) prefix1 -> {node2, node3}
 * 2) Increase cost towards node3 to 20; prefix -> {node2}
 * 3) mark link towards node2 as drained; prefix1 -> {node3}
 * 3) Set cost towards node2 to 20 (still drained); prefix1 -> {node3}
 * 4) Undrain link; prefix1 -> {node2, node3}
 */
TEST(BGPRedistribution, IgpMetric) {
  const std::string data1{"data1"};
  const auto expectedAddr = addr1;
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName,
      false /* enableV4 */,
      true /* enable segment label */,
      true /* enableBestRouteSelection */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  //
  // Create BGP prefix
  //
  const auto bgpPrefix2 = createPrefixEntry(
      addr1,
      thrift::PrefixType::BGP,
      data1,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP);
  const auto bgpPrefix3 = createPrefixEntry(
      addr1,
      thrift::PrefixType::BGP,
      data1,
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP);

  //
  // Setup adjacencies
  //
  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 0);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 0);
  auto adjacencyDb3 = createAdjDb("3", {adj31}, 0);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  //
  // Update prefix databases
  //
  auto prefixDb2WithBgp =
      createPrefixDb("2", {createPrefixEntry(addr2), bgpPrefix2});
  auto prefixDb3WithBgp =
      createPrefixDb("3", {createPrefixEntry(addr3), bgpPrefix3});
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2WithBgp).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3WithBgp).empty());

  //
  // Step-1 prefix1 -> {node2, node3}
  //
  auto decisionRouteDb =
      *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  auto routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(3));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10),
                  createNextHopFromAdj(adj13, false, 10))))));

  //
  // Increase cost towards node3 to 20; prefix -> {node2}
  //
  adjacencyDb1.adjacencies()[1].metric() = 20;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                  .topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(3));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10))))));

  //
  // mark link towards node2 as drained; prefix1 -> {node3}
  // No route towards addr2 (node2's loopback)
  //
  adjacencyDb1.adjacencies()[0].isOverloaded() = true;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                  .topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();

  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(2));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj13, false, 20))))));

  //
  // Set cost towards node2 to 20 (still drained); prefix1 -> {node3}
  // No route towards addr2 (node2's loopback)
  //
  adjacencyDb1.adjacencies()[0].metric() = 20;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                  .topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(2));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj13, false, 20))))));

  //
  // Undrain link; prefix1 -> {node2, node3}
  //
  adjacencyDb1.adjacencies()[0].isOverloaded() = false;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                  .topologyChanged);
  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(3));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 20),
                  createNextHopFromAdj(adj13, false, 20))))));
}

TEST(Decision, IgpCost) {
  std::string nodeName("1");
  const auto expectedAddr = addr1;
  SpfSolver spfSolver(
      nodeName,
      false /* enableV4 */,
      true /* enable segment label */,
      true /* enableBestRouteSelection */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;

  // Test topology: spine
  // Setup adjacencies: note each link cost is 10
  // 1     4 (SSW)
  // |  x  |
  // 2     3 (FSW)

  // Setup adjacency
  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj24}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj31, adj34}, 3);
  auto adjacencyDb4 = createAdjDb("4", {adj42, adj43}, 4);
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb4, kTestingAreaName)
                  .topologyChanged);

  // Setup prefixes. node2 annouces the prefix
  const auto node2Prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(200, 0, 0));
  EXPECT_FALSE(
      updatePrefixDatabase(prefixState, createPrefixDb("2", {node2Prefix}))
          .empty());

  // Case-1 node1 route to 2 with direct link: igp cost = 1 * 10
  {
    auto decisionRouteDb =
        *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
    auto route = decisionRouteDb.unicastRoutes.at(toIPNetwork(expectedAddr));
    EXPECT_EQ(route.igpCost, 10);
  }

  // Case-2 link 21 broken, node1 route to 2 (1->3->4->2): igp cost = 3 * 10
  {
    auto newAdjacencyDb2 = createAdjDb("2", {adj24}, 4);
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(newAdjacencyDb2, kTestingAreaName)
            .topologyChanged);
    auto decisionRouteDb =
        *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
    auto route = decisionRouteDb.unicastRoutes.at(toIPNetwork(expectedAddr));
    EXPECT_EQ(route.igpCost, 30);
  }
}

TEST(Decision, BestRouteSelection) {
  std::string nodeName("1");
  const auto expectedAddr = addr1;
  SpfSolver spfSolver(
      nodeName,
      false /* enableV4 */,
      true /* enable segment label */,
      true /* enableBestRouteSelection */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;

  //
  // Setup adjacencies
  // 2 <--> 1 <--> 3
  //
  auto adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj31}, 3);
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  //
  // Setup prefixes. node2 and node3 announces the same prefix with same metrics
  // and different types. The type shouldn't have any effect on best route
  // selection.
  //
  const auto node2Prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(200, 0, 0));
  const auto node3Prefix = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::BGP, createMetrics(200, 0, 0));
  EXPECT_FALSE(
      updatePrefixDatabase(prefixState, createPrefixDb("2", {node2Prefix}))
          .empty());
  EXPECT_FALSE(
      updatePrefixDatabase(prefixState, createPrefixDb("3", {node3Prefix}))
          .empty());

  //
  // Verifies that best routes cache is empty
  //
  EXPECT_TRUE(spfSolver.getBestRoutesCache().empty());

  //
  // Case-1 node1 ECMP towards {node2, node3}
  //
  auto decisionRouteDb =
      *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  auto routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(1));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10),
                  createNextHopFromAdj(adj13, false, 10))))));

  //
  // Verify that prefix-state report two best routes
  //
  {
    auto bestRoutesCache = spfSolver.getBestRoutesCache();
    ASSERT_EQ(1, bestRoutesCache.count(toIPNetwork(addr1)));
    auto& bestRoutes = bestRoutesCache.at(toIPNetwork(addr1));
    EXPECT_EQ(2, bestRoutes.allNodeAreas.size());
    EXPECT_EQ(1, bestRoutes.allNodeAreas.count({"2", kTestingAreaName}));
    EXPECT_EQ(1, bestRoutes.allNodeAreas.count({"3", kTestingAreaName}));
    EXPECT_EQ("2", bestRoutes.bestNodeArea.first);
  }

  //
  // Case-2 node1 prefers node2 (prefix metrics)
  //
  const auto node2PrefixPreferred = createPrefixEntryWithMetrics(
      addr1, thrift::PrefixType::DEFAULT, createMetrics(200, 100, 0));
  EXPECT_FALSE(updatePrefixDatabase(
                   prefixState, createPrefixDb("2", {node2PrefixPreferred}))
                   .empty());

  decisionRouteDb = *spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  routeDb = decisionRouteDb.toThrift();
  EXPECT_THAT(*routeDb.unicastRoutes(), testing::SizeIs(1));
  EXPECT_THAT(
      *routeDb.unicastRoutes(),
      testing::Contains(AllOf(
          Truly([&expectedAddr](auto i) { return i.dest() == expectedAddr; }),
          ResultOf(
              getUnicastNextHops,
              testing::UnorderedElementsAre(
                  createNextHopFromAdj(adj12, false, 10))))));
  //
  // Verify that prefix-state report only one best route
  //
  {
    auto bestRoutesCache = spfSolver.getBestRoutesCache();
    ASSERT_EQ(1, bestRoutesCache.count(toIPNetwork(addr1)));
    auto& bestRoutes = bestRoutesCache.at(toIPNetwork(addr1));
    EXPECT_EQ(1, bestRoutes.allNodeAreas.size());
    EXPECT_EQ(1, bestRoutes.allNodeAreas.count({"2", kTestingAreaName}));
    EXPECT_EQ("2", bestRoutes.bestNodeArea.first);
  }
}

//
// Test topology:
// connected bidirectionally
//  1 <----> 2 <----> 3
// partitioned
//  1 <----  2  ----> 3
//
class ConnectivityTest : public ::testing::TestWithParam<bool> {};

TEST_P(ConnectivityTest, GraphConnectedOrPartitioned) {
  auto partitioned = GetParam();

  auto adjacencyDb1 = createAdjDb("1", {}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {}, 3);
  if (!partitioned) {
    adjacencyDb1 = createAdjDb("1", {adj12}, 1);
    adjacencyDb3 = createAdjDb("3", {adj32}, 3);
  }

  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, true /* enable segment label */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  EXPECT_EQ(
      LinkState::LinkStateChange(false, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName));
  EXPECT_EQ(
      LinkState::LinkStateChange(!partitioned, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName));
  EXPECT_EQ(
      LinkState::LinkStateChange(!partitioned, false, true),
      linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName));

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

  // route from 1 to 3
  auto routeDb = spfSolver.buildRouteDb("1", areaLinkStates, prefixState);
  bool foundRouteV6 = false;
  bool foundRouteNodeLabel = false;
  if (routeDb.has_value()) {
    for (auto const& [prefix, _] : routeDb->unicastRoutes) {
      if (toIpPrefix(prefix) == addr3) {
        foundRouteV6 = true;
        break;
      }
    }
    for (auto const& [label, _] : routeDb->mplsRoutes) {
      if (label == 3) {
        foundRouteNodeLabel = true;
      }
    }
  }

  EXPECT_EQ(partitioned, !foundRouteV6);
  EXPECT_EQ(partitioned, !foundRouteNodeLabel);
}

INSTANTIATE_TEST_CASE_P(
    PartitionedTopologyInstance, ConnectivityTest, ::testing::Bool());

//
// Overload node test in a linear topology with shortest path calculation
//
// 1<--->2<--->3
//   10     10
//
TEST(ConnectivityTest, NodeHardDrainTest) {
  std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, true /* enable segment label */);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, nodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  // Add all adjacency DBs
  auto adjacencyDb1 = createAdjDb("1", {adj12}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj32}, 3);

  // Make node-2 overloaded
  adjacencyDb2.isOverloaded() = true;

  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  auto routeMap =
      getRouteMap(spfSolver, {"1", "2", "3"}, areaLinkStates, prefixState);

  // We only expect 4 unicast routes, 7 node label routes because node-1 and
  // node-3 are disconnected.
  // node-1 => node-2 (label + unicast)
  // node-2 => node-1, node-3 (label + unicast)
  // node-3 => node-2 (label + unicast)
  EXPECT_EQ(11, routeMap.size());

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(
          adj12, false, *adj12.metric(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel());

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj23, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(
          adj21, false, *adj21.metric(), labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(
          adj23, false, *adj23.metric(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel());

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(
          adj32, false, *adj32.metric(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel());
}

/*
 * Interface soft-drain test will mimick the soft-drain behavior to change
 * adj metric on one side, aka, uni-directionally. The test will verify both
 * ends of the link will react to this drain behavior and change SPF calculation
 * result accordinly.
 *
 * The test forms a circle topology for SPF calculation.
 *
 *         20       10
 *     1<------>2<------>3(new)
 *     ^   10       10   ^
 *     |                 |
 *     |        10       |
 *     |-----------------|
 *              10
 */
TEST(ConnectivityTest, InterfaceSoftDrainTest) {
  const std::string nodeName("1");
  SpfSolver spfSolver(
      nodeName, false /* disable v4 */, true /* enable segment label */);

  // Initialize link-state and prefix-state obj
  std::unordered_map<std::string, LinkState> areaLinkStates = {
      {kTestingAreaName, LinkState(kTestingAreaName, nodeName)}};
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  /*
   * Create adjacency DBs with:
   *
   * node1 -> {node2(metric = 10)}
   * node2 -> {node1(metric = 10), node3(metric = 10)}
   * node3 -> {node1(metric = 10), node2(metric = 10)}
   */
  auto adjacencyDb1 = createAdjDb("1", {adj12_1}, 1);
  auto adjacencyDb2 = createAdjDb("2", {adj21, adj23}, 2);
  auto adjacencyDb3 = createAdjDb("3", {adj32, adj31_old}, 3);

  {
    EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
    EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb2).empty());
    EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb3).empty());

    // No bi-directional adjacencies yet. No topo change.
    EXPECT_FALSE(
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
            .topologyChanged);
    // node2 <-> node3 has bi-directional adjs. Expect topo change.
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
            .topologyChanged);
    // node1 <-> node2 has bi-directional adjs. Expect topo change.
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
            .topologyChanged);
  }

  /*
   * add/update adjacency of node1 with old versions
   * node1 -> {node2(metric = 20), node3(metric = 10)}
   * node2 -> {node1(metric = 10), node3(metric = 10)}
   * node3 -> {node1(metric = 10), node2(metric = 10)}
   */
  {
    // Update adjDb to add node1 -> node3 to form bi-dir adj. Expect topo
    // change.
    auto adjDb1 = createAdjDb("1", {adj12_1, adj13}, 1);
    EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjDb1, kTestingAreaName)
                    .topologyChanged);
    // Update adjDb1 to increase node1 -> node2 metric. Expect topo change.
    adjDb1 = createAdjDb("1", {adj12_2, adj13}, 1);
    EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjDb1, kTestingAreaName)
                    .topologyChanged);
  }

  auto routeMap =
      getRouteMap(spfSolver, {"1", "2", "3"}, areaLinkStates, prefixState);

  // We only expect 6 unicast routes, 9 node label routes
  // node-1 => node-2, node-3
  // node-2 => node-1, node-3
  // node-3 => node-2, node-1
  EXPECT_EQ(15, routeMap.size());

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(adj12_2, false, 20),
           createNextHopFromAdj(adj13, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj12_2, false, 20, labelPhpAction),
           createNextHopFromAdj(adj13, false, 20, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(
          adj13, false, *adj13.metric(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel());

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj23, false, 10)}));
  // SPF will choose the max metric between node1 and node2. Hence create ECMP
  // towards node1 and node3
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops(
          {createNextHopFromAdj(adj21, false, 20),
           createNextHopFromAdj(adj23, false, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj21, false, 20, labelPhpAction),
           createNextHopFromAdj(adj23, false, 20, labelSwapAction1)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(
          adj23, false, *adj23.metric(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel());

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(
          adj31, false, *adj31.metric(), labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(
          adj32, false, *adj32.metric(), labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel());

  // adjacency update (remove adjacency) for node1
  adjacencyDb1 = createAdjDb("1", {adj12_2}, 0);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                  .topologyChanged);
  adjacencyDb3 = createAdjDb("3", {adj32}, 0);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                   .topologyChanged);

  adjacencyDb1 = createAdjDb("1", {adj12_2, adj13}, 0);
  EXPECT_FALSE(linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
                   .topologyChanged);
}

//
// Test topology:
//
//  1------2
//  | \     |
//  |   \   |
//  3------4
//
// Test both IP v4 & v6
// 1,2,3,4 are simply meshed with each other with 1 parallet links
//
class SimpleRingMeshTopologyFixture
    : public ::testing::TestWithParam<
          std::tuple<bool, std::optional<thrift::PrefixType>>> {
 public:
  SimpleRingMeshTopologyFixture() : v4Enabled(std::get<0>(GetParam())) {}

 protected:
  void
  CustomSetUp(
      bool useNodeSegmentLabel,
      std::optional<thrift::PrefixType> prefixType = std::nullopt,
      bool createNewBgpRoute = false) {
    std::string nodeName("1");
    spfSolver =
        std::make_unique<SpfSolver>(nodeName, v4Enabled, useNodeSegmentLabel);
    adjacencyDb1 = createAdjDb("1", {adj12, adj13, adj14}, 1);
    adjacencyDb2 = createAdjDb("2", {adj21, adj23, adj24}, 2);
    adjacencyDb3 = createAdjDb("3", {adj31, adj32, adj34}, 3);
    adjacencyDb4 = createAdjDb("4", {adj41, adj42, adj43}, 4);

    areaLinkStates.emplace(
        kTestingAreaName, LinkState(kTestingAreaName, nodeName));
    auto& linkState = areaLinkStates.at(kTestingAreaName);

    EXPECT_EQ(
        LinkState::LinkStateChange(false, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb4, kTestingAreaName));

    auto pdb1 = v4Enabled ? prefixDb1V4 : prefixDb1;
    auto pdb2 = v4Enabled ? prefixDb2V4 : prefixDb2;
    auto pdb3 = v4Enabled ? prefixDb3V4 : prefixDb3;
    auto pdb4 = v4Enabled ? prefixDb4V4 : prefixDb4;

    auto bgp1 = v4Enabled ? bgpAddr1V4 : bgpAddr1;
    auto bgp2 = v4Enabled ? bgpAddr2V4 : bgpAddr2;
    auto bgp3 = v4Enabled ? bgpAddr3V4 : bgpAddr3;
    auto bgp4 = v4Enabled ? bgpAddr4V4 : bgpAddr4;

    updatePrefixDatabase(prefixState, pdb1);
    updatePrefixDatabase(prefixState, pdb2);
    updatePrefixDatabase(prefixState, pdb3);
    updatePrefixDatabase(prefixState, pdb4);
  }

  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  bool v4Enabled{false};

  std::unique_ptr<SpfSolver> spfSolver;
  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;
};

//
// Test topology:
//
//  1------2
//  |      |
//  |      |
//  3------4
//
// Test both IP v4 & v6
//
class SimpleRingTopologyFixture
    : public ::testing::TestWithParam<
          std::tuple<bool, std::optional<thrift::PrefixType>>> {
 public:
  SimpleRingTopologyFixture() : v4Enabled(std::get<0>(GetParam())) {}

 protected:
  void
  CustomSetUp(
      bool useNodeSegmentLabel,
      std::optional<thrift::PrefixType> prefixType = std::nullopt,
      bool createNewBgpRoute = false) {
    std::string nodeName("1");
    spfSolver =
        std::make_unique<SpfSolver>(nodeName, v4Enabled, useNodeSegmentLabel);
    adjacencyDb1 = createAdjDb("1", {adj12, adj13}, 1);
    adjacencyDb2 = createAdjDb("2", {adj21, adj24}, 2);
    adjacencyDb3 = createAdjDb("3", {adj31, adj34}, 3);
    adjacencyDb4 = createAdjDb("4", {adj42, adj43}, 4);

    areaLinkStates.emplace(
        kTestingAreaName, LinkState(kTestingAreaName, nodeName));
    auto& linkState = areaLinkStates.at(kTestingAreaName);

    EXPECT_EQ(
        LinkState::LinkStateChange(false, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName));
    EXPECT_EQ(
        LinkState::LinkStateChange(true, false, true),
        linkState.updateAdjacencyDatabase(adjacencyDb4, kTestingAreaName));

    auto pdb1 = v4Enabled ? prefixDb1V4 : prefixDb1;
    auto pdb2 = v4Enabled ? prefixDb2V4 : prefixDb2;
    auto pdb3 = v4Enabled ? prefixDb3V4 : prefixDb3;
    auto pdb4 = v4Enabled ? prefixDb4V4 : prefixDb4;

    auto bgp1 = v4Enabled ? bgpAddr1V4 : bgpAddr1;
    auto bgp2 = v4Enabled ? bgpAddr2V4 : bgpAddr2;
    auto bgp3 = v4Enabled ? bgpAddr3V4 : bgpAddr3;
    auto bgp4 = v4Enabled ? bgpAddr4V4 : bgpAddr4;

    updatePrefixDatabase(prefixState, pdb1);
    updatePrefixDatabase(prefixState, pdb2);
    updatePrefixDatabase(prefixState, pdb3);
    updatePrefixDatabase(prefixState, pdb4);
  }

  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  bool v4Enabled{false};

  std::unique_ptr<SpfSolver> spfSolver;
  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;

  void
  verifyRouteInUpdateNoDelete(
      std::string nodeName, int32_t mplsLabel, const DecisionRouteDb& compDb) {
    // verify route DB change in node 1.
    auto deltaRoutes = compDb.calculateUpdate(
        spfSolver->buildRouteDb(nodeName, areaLinkStates, prefixState).value());

    EXPECT_EQ(deltaRoutes.mplsRoutesToUpdate.count(mplsLabel), 1);
    EXPECT_EQ(deltaRoutes.mplsRoutesToDelete.size(), 0);
  }
};

INSTANTIATE_TEST_CASE_P(
    SimpleRingTopologyInstance,
    SimpleRingTopologyFixture,
    ::testing::Values(
        std::make_tuple(true, std::nullopt),
        std::make_tuple(false, std::nullopt),
        std::make_tuple(true, thrift::PrefixType::BGP),
        std::make_tuple(false, thrift::PrefixType::BGP)));

//
// Verify SpfSolver finds the shortest path
//
TEST_P(SimpleRingTopologyFixture, ShortestPathTest) {
  CustomSetUp(true /* use node segment label */);
  fb303::fbData->resetAllData();
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  EXPECT_EQ(28, routeMap.size());

  // validate router 1
  const auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters.at("decision.spf_runs.count"), 4);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops(
          {createNextHopFromAdj(adj12, v4Enabled, 20),
           createNextHopFromAdj(adj13, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj12, false, 20, labelSwapAction4),
           createNextHopFromAdj(adj13, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops(
          {createNextHopFromAdj(adj21, v4Enabled, 20),
           createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
           createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel());

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops(
          {createNextHopFromAdj(adj31, v4Enabled, 20),
           createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
           createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel());

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops(
          {createNextHopFromAdj(adj42, v4Enabled, 20),
           createNextHopFromAdj(adj43, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj42, false, 20, labelSwapAction1),
           createNextHopFromAdj(adj43, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel());
}

//
// Verify duplicate mpls routes case
// let two nodes announcing same mpls label. Verify that the one with higher
// name value would win.
// change one node to use a different mpls label. verify routes gets programmed
// and no withdraw happened.
//
TEST_P(SimpleRingTopologyFixture, DuplicateMplsRoutes) {
  CustomSetUp(true /* use node segment label */);
  fb303::fbData->resetAllData();
  // make node1's mpls label same as node2.
  adjacencyDb1.nodeLabel() = 2;
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);

  // verify route DB change in node 1, 2 ,3.
  // verify that only one route to mpls lable 1 is installed in all nodes
  DecisionRouteDb emptyRouteDb;
  verifyRouteInUpdateNoDelete("1", 2, emptyRouteDb);

  verifyRouteInUpdateNoDelete("2", 2, emptyRouteDb);

  verifyRouteInUpdateNoDelete("3", 2, emptyRouteDb);

  auto counters = fb303::fbData->getCounters();
  // verify the counters to be 3 because each node will noticed a duplicate
  // for mpls label 1.
  EXPECT_EQ(counters.at("decision.duplicate_node_label.count.60"), 3);

  auto compDb1 =
      spfSolver->buildRouteDb("1", areaLinkStates, prefixState).value();
  auto compDb2 =
      spfSolver->buildRouteDb("2", areaLinkStates, prefixState).value();
  auto compDb3 =
      spfSolver->buildRouteDb("3", areaLinkStates, prefixState).value();

  counters = fb303::fbData->getCounters();
  // now the counter should be 6, becasue we called buildRouteDb 3 times.
  EXPECT_EQ(counters.at("decision.duplicate_node_label.count.60"), 6);

  // change nodelabel of node 1 to be 1. Now each node has it's own
  // mpls label, there should be no duplicate.
  // verify that there is an update entry for mpls route to label 1.
  // verify that no withdrawals of mpls routes to label 1.
  adjacencyDb1.nodeLabel() = 1;
  linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName);
  verifyRouteInUpdateNoDelete("1", 2, compDb1);

  verifyRouteInUpdateNoDelete("2", 2, compDb2);

  verifyRouteInUpdateNoDelete("3", 2, compDb3);

  // because there is no duplicate anymore, so that counter should keep as 6.
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters.at("decision.duplicate_node_label.count.60"), 6);
}

//
// Use the same topology, but test multi-path routing
//
TEST_P(SimpleRingTopologyFixture, MultiPathTest) {
  CustomSetUp(true /* use node segment label */);
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  EXPECT_EQ(28, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops(
          {createNextHopFromAdj(adj12, v4Enabled, 20),
           createNextHopFromAdj(adj13, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj12, false, 20, labelSwapAction4),
           createNextHopFromAdj(adj13, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops(
          {createNextHopFromAdj(adj21, v4Enabled, 20),
           createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
           createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel());

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops(
          {createNextHopFromAdj(adj31, v4Enabled, 20),
           createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
           createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel());

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops(
          {createNextHopFromAdj(adj42, v4Enabled, 20),
           createNextHopFromAdj(adj43, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj42, false, 20, labelSwapAction1),
           createNextHopFromAdj(adj43, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel());
}

//
// attach nodes to outside world, e.g., POP
// verify all non-POP nodes find their closest POPs
//
TEST_P(SimpleRingTopologyFixture, AttachedNodesTest) {
  CustomSetUp(true /* enable node segment label */);
  // Advertise default prefixes from node-1 and node-4
  auto defaultRoutePrefix = v4Enabled ? "0.0.0.0/0" : "::/0";
  auto defaultRoute = toIpPrefix(defaultRoutePrefix);
  auto prefixDb1 = createPrefixDb(
      "1", {createPrefixEntry(addr1), createPrefixEntry(defaultRoute)});
  auto prefixDb4 = createPrefixDb(
      "4", {createPrefixEntry(addr4), createPrefixEntry(defaultRoute)});
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb1).empty());
  EXPECT_FALSE(updatePrefixDatabase(prefixState, prefixDb4).empty());

  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) + 2 (default routes) = 14
  // Node label routes => 4 * 4 = 16
  EXPECT_EQ(30, routeMap.size());

  // validate router 1
  // no default route boz it's attached
  // i.e., spfSolver(false), bcoz we set node 1 to be "1" distance away from the
  // dummy node and its neighbors are all further away, thus there is no route
  // to the dummy node
  EXPECT_EQ(0, routeMap.count({"1", defaultRoutePrefix}));

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", defaultRoutePrefix)],
      NextHops(
          {createNextHopFromAdj(adj21, v4Enabled, 10),
           createNextHopFromAdj(adj24, v4Enabled, 10)}));

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", defaultRoutePrefix)],
      NextHops(
          {createNextHopFromAdj(adj31, v4Enabled, 10),
           createNextHopFromAdj(adj34, v4Enabled, 10)}));

  // validate router 4
  // no default route boz it's attached
  EXPECT_EQ(0, routeMap.count({"4", defaultRoutePrefix}));
}

//
// Verify overload bit setting of a node's adjacency DB with multipath
// enabled. Make node-3 and node-2 overloaded and verify routes.
// It will disconnect node-1 with node-4 but rests should be reachable
//
TEST_P(SimpleRingTopologyFixture, OverloadNodeTest) {
  CustomSetUp(true /* enable node segment label */);
  adjacencyDb2.isOverloaded() = true;
  adjacencyDb3.isOverloaded() = true;
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
                  .topologyChanged);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 2 + 3 + 3 + 2 = 10
  // Node label routes => 3 + 4 + 4 + 3 = 14
  EXPECT_EQ(24, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj13, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj13, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)})); // No LFA
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops(
          {createNextHopFromAdj(adj21, v4Enabled, 20),
           createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj21, false, 20, labelSwapAction3),
           createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel());

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops(
          {createNextHopFromAdj(adj31, v4Enabled, 20),
           createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj31, false, 20, labelSwapAction2),
           createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj31, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj31, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel());

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel());
}

//
// Verify overload bit setting of individual adjacencies with multipath
// enabled. node-3 will get disconnected
//
TEST_P(SimpleRingTopologyFixture, OverloadLinkTest) {
  CustomSetUp(true /* enable node segment label */);
  adjacencyDb3.adjacencies()[0].isOverloaded() = true; // make adj31 overloaded
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  EXPECT_EQ(28, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 30)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj12, false, 30, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj24, false, 20, labelSwapAction3)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel());

  // validate router 3
  // no routes for router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj34, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj34, false, 20, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj34, v4Enabled, 30)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj34, false, 30, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel());

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr3V4 : addr3))],
      NextHops({createNextHopFromAdj(adj43, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj43, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel());

  // Now also make adj34 overloaded which will disconnect the node-3
  adjacencyDb3.adjacencies()[1].isOverloaded() = true;
  EXPECT_TRUE(linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
                  .topologyChanged);

  routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 2 + 2 + 0 + 2 = 6
  // Node label routes => 3 * 3 + 1 = 10
  EXPECT_EQ(16, routeMap.size());

  // validate router 1

  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj12, false, 20, labelSwapAction4)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj12, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj12, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr4V4 : addr4))],
      NextHops({createNextHopFromAdj(adj24, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj24, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj21, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj21, false, 10, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel());

  // validate router 3

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel());

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr2V4 : addr2))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 10)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj42, false, 10, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(v4Enabled ? addr1V4 : addr1))],
      NextHops({createNextHopFromAdj(adj42, v4Enabled, 20)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj42, false, 20, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel());
}

/* add this block comment to suppress multiline breaker "\"s below
//
// Test topology: ring with parallel adjacencies, *x* denotes metric
//    ---*11*---
//   /          \
//  1----*11*----2
//  |\          /|
//  | ---*20*--- |
// *11*         *11*
//  |            |
//  | ---*11*--- |
//  |/          \|
//  3----*20*----4
//   \          /
//    ---*20*---
//
*/
class ParallelAdjRingTopologyFixture
    : public ::testing::TestWithParam<std::optional<thrift::PrefixType>> {
 public:
  ParallelAdjRingTopologyFixture() = default;

 protected:
  void
  CustomSetUp(
      bool useNodeSegmentLabel,
      std::optional<thrift::PrefixType> prefixType = std::nullopt) {
    std::string nodeName("1");
    spfSolver =
        std::make_unique<SpfSolver>(nodeName, false, useNodeSegmentLabel);
    // R1 -> R2
    adj12_1 =
        createAdjacency("2", "2/1", "1/1", "fe80::2:1", "192.168.2.1", 11, 201);
    adj12_2 =
        createAdjacency("2", "2/2", "1/2", "fe80::2:2", "192.168.2.2", 11, 202);
    adj12_3 =
        createAdjacency("2", "2/3", "1/3", "fe80::2:3", "192.168.2.3", 20, 203);
    // R1 -> R3
    adj13_1 =
        createAdjacency("3", "3/1", "1/1", "fe80::3:1", "192.168.3.1", 11, 301);

    // R2 -> R1
    adj21_1 =
        createAdjacency("1", "1/1", "2/1", "fe80::1:1", "192.168.1.1", 11, 101);
    adj21_2 =
        createAdjacency("1", "1/2", "2/2", "fe80::1:2", "192.168.1.2", 11, 102);
    adj21_3 =
        createAdjacency("1", "1/3", "2/3", "fe80::1:3", "192.168.1.3", 20, 103);
    // R2 -> R4
    adj24_1 =
        createAdjacency("4", "4/1", "2/1", "fe80::4:1", "192.168.4.1", 11, 401);

    // R3 -> R1
    adj31_1 =
        createAdjacency("1", "1/1", "3/1", "fe80::1:1", "192.168.1.1", 11, 101);
    // R3 -> R4
    adj34_1 =
        createAdjacency("4", "4/1", "3/1", "fe80::4:1", "192.168.4.1", 11, 401);
    adj34_2 =
        createAdjacency("4", "4/2", "3/2", "fe80::4:2", "192.168.4.2", 20, 402);
    adj34_3 =
        createAdjacency("4", "4/3", "3/3", "fe80::4:3", "192.168.4.3", 20, 403);

    // R4 -> R2
    adj42_1 =
        createAdjacency("2", "2/1", "4/1", "fe80::2:1", "192.168.2.1", 11, 201);
    adj43_1 =
        createAdjacency("3", "3/1", "4/1", "fe80::3:1", "192.168.3.1", 11, 301);
    adj43_2 =
        createAdjacency("3", "3/2", "4/2", "fe80::3:2", "192.168.3.2", 20, 302);
    adj43_3 =
        createAdjacency("3", "3/3", "4/3", "fe80::3:3", "192.168.3.3", 20, 303);

    adjacencyDb1 = createAdjDb("1", {adj12_1, adj12_2, adj12_3, adj13_1}, 1);

    adjacencyDb2 = createAdjDb("2", {adj21_1, adj21_2, adj21_3, adj24_1}, 2);

    adjacencyDb3 = createAdjDb("3", {adj31_1, adj34_1, adj34_2, adj34_3}, 3);

    adjacencyDb4 = createAdjDb("4", {adj42_1, adj43_1, adj43_2, adj43_3}, 4);

    // Adjacency db's
    areaLinkStates.emplace(
        kTestingAreaName, LinkState(kTestingAreaName, nodeName));
    auto& linkState = areaLinkStates.at(kTestingAreaName);
    EXPECT_FALSE(
        linkState.updateAdjacencyDatabase(adjacencyDb1, kTestingAreaName)
            .topologyChanged);
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb2, kTestingAreaName)
            .topologyChanged);
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb3, kTestingAreaName)
            .topologyChanged);
    EXPECT_TRUE(
        linkState.updateAdjacencyDatabase(adjacencyDb4, kTestingAreaName)
            .topologyChanged);

    // Prefix db's
    updatePrefixDatabase(prefixState, prefixDb1);
    updatePrefixDatabase(prefixState, prefixDb2);
    updatePrefixDatabase(prefixState, prefixDb3);
    updatePrefixDatabase(prefixState, prefixDb4);
  }

  thrift::Adjacency adj12_1, adj12_2, adj12_3, adj13_1, adj21_1, adj21_2,
      adj21_3, adj24_1, adj31_1, adj34_1, adj34_2, adj34_3, adj42_1, adj43_1,
      adj43_2, adj43_3;
  thrift::AdjacencyDatabase adjacencyDb1, adjacencyDb2, adjacencyDb3,
      adjacencyDb4;

  std::unique_ptr<SpfSolver> spfSolver;
  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;
};

TEST_F(ParallelAdjRingTopologyFixture, ShortestPathTest) {
  CustomSetUp(true /* enable segment label */);
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  EXPECT_EQ(28, routeMap.size());

  // validate router 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr4))],
      NextHops(
          {createNextHopFromAdj(adj12_2, false, 22),
           createNextHopFromAdj(adj13_1, false, 22),
           createNextHopFromAdj(adj12_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj12_2, false, 22, labelSwapAction4),
           createNextHopFromAdj(adj13_1, false, 22, labelSwapAction4),
           createNextHopFromAdj(adj12_1, false, 22, labelSwapAction4)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(adj12_2, false, 11),
           createNextHopFromAdj(adj12_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj12_2, false, 11, labelPhpAction),
           createNextHopFromAdj(adj12_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel());

  // validate router 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops(
          {createNextHopFromAdj(adj21_2, false, 22),
           createNextHopFromAdj(adj21_1, false, 22),
           createNextHopFromAdj(adj24_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj21_2, false, 22, labelSwapAction3),
           createNextHopFromAdj(adj21_1, false, 22, labelSwapAction3),
           createNextHopFromAdj(adj24_1, false, 22, labelSwapAction3)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops(
          {createNextHopFromAdj(adj21_2, false, 11),
           createNextHopFromAdj(adj21_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj21_2, false, 11, labelPhpAction),
           createNextHopFromAdj(adj21_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel());

  // validate router 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(adj31_1, false, 22),
           createNextHopFromAdj(adj34_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj31_1, false, 22, labelSwapAction2),
           createNextHopFromAdj(adj34_1, false, 22, labelSwapAction2)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel());

  // validate router 4
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11, labelPhpAction)}));
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops(
          {createNextHopFromAdj(adj42_1, false, 22),
           createNextHopFromAdj(adj43_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj42_1, false, 22, labelSwapAction1),
           createNextHopFromAdj(adj43_1, false, 22, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel());
}

//
// Use the same topology, but test multi-path routing
//
TEST_F(ParallelAdjRingTopologyFixture, MultiPathTest) {
  CustomSetUp(true /* enable segment label */);
  auto routeMap = getRouteMap(
      *spfSolver, {"1", "2", "3", "4"}, areaLinkStates, prefixState);

  // Unicast routes => 4 * (4 - 1) = 12
  // Node label routes => 4 * 4 = 16
  EXPECT_EQ(28, routeMap.size());

  // validate router 1

  // adj "2/3" is also selected in spite of large metric
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr4))],
      NextHops(
          {createNextHopFromAdj(adj12_1, false, 22),
           createNextHopFromAdj(adj12_2, false, 22),
           createNextHopFromAdj(adj13_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj12_1, false, 22, labelSwapAction4),
           createNextHopFromAdj(adj12_2, false, 22, labelSwapAction4),
           createNextHopFromAdj(adj13_1, false, 22, labelSwapAction4)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj13_1, false, 11, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(adj12_1, false, 11),
           createNextHopFromAdj(adj12_2, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("1", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj12_1, false, 11, labelPhpAction),
           createNextHopFromAdj(adj12_2, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "1", *adjacencyDb1.nodeLabel());

  // validate router 2

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr4))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj24_1, false, 11, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops(
          {createNextHopFromAdj(adj21_1, false, 22),
           createNextHopFromAdj(adj21_2, false, 22),
           createNextHopFromAdj(adj24_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj21_1, false, 22, labelSwapAction3),
           createNextHopFromAdj(adj21_2, false, 22, labelSwapAction3),
           createNextHopFromAdj(adj24_1, false, 22, labelSwapAction3)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops(
          {createNextHopFromAdj(adj21_1, false, 11),
           createNextHopFromAdj(adj21_2, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("2", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj21_1, false, 11, labelPhpAction),
           createNextHopFromAdj(adj21_2, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "2", *adjacencyDb2.nodeLabel());

  // validate router 3

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr4))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb4.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj34_1, false, 11, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(adj31_1, false, 22),
           createNextHopFromAdj(adj34_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj31_1, false, 22, labelSwapAction2),
           createNextHopFromAdj(adj34_1, false, 22, labelSwapAction2)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("3", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj31_1, false, 11, labelPhpAction)}));

  validatePopLabelRoute(routeMap, "3", *adjacencyDb3.nodeLabel());

  // validate router 4

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr3))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb3.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj43_1, false, 11, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb2.nodeLabel()))],
      NextHops({createNextHopFromAdj(adj42_1, false, 11, labelPhpAction)}));

  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr1))],
      NextHops(
          {createNextHopFromAdj(adj42_1, false, 22),
           createNextHopFromAdj(adj43_1, false, 22)}));
  EXPECT_EQ(
      routeMap[make_pair("4", std::to_string(*adjacencyDb1.nodeLabel()))],
      NextHops(
          {createNextHopFromAdj(adj42_1, false, 22, labelSwapAction1),
           createNextHopFromAdj(adj43_1, false, 22, labelSwapAction1)}));

  validatePopLabelRoute(routeMap, "4", *adjacencyDb4.nodeLabel());
}

//
// Test topology:
//
//  n * n grid
// A box m has up to 4 interfaces named 0/1, 0/2, 0/3, and 0/4
//                       m + n
//                         |
//                        0/4
//                         |
//         m-1 ----0/3---- m ----0/1---- m + 1
//                         |
//                        0/2
//                         |
//                       m - n

// add adjacencies to neighbor at grid(i, j)
void
addAdj(
    int i,
    int j,
    string ifName,
    vector<thrift::Adjacency>& adjs,
    int n,
    string otherIfName) {
  if (i < 0 || i >= n || j < 0 || j >= n) {
    return;
  }

  auto neighbor = i * n + j;
  adjs.emplace_back(createThriftAdjacency(
      fmt::format("{}", neighbor),
      ifName,
      fmt::format("fe80::{}", neighbor),
      fmt::format("192.168.{}.{}", neighbor / 256, neighbor % 256),
      1,
      100001 + neighbor /* adjacency-label */,
      false /* overload-bit */,
      100,
      10000 /* timestamp */,
      1 /* weight */,
      otherIfName));
}

string
nodeToPrefixV6(int node) {
  return fmt::format("::ffff:10.1.{}.{}/128", node / 256, node % 256);
}

void
createGrid(LinkState& linkState, PrefixState& prefixState, int n) {
  LOG(INFO) << "grid: " << n << " by " << n;
  // confined bcoz of min("fe80::{}", "192.168.{}.{}", "::ffff:10.1.{}.{}")
  EXPECT_TRUE(n * n < 10000) << "n is too large";

  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      auto node = i * n + j;
      auto nodeName = fmt::format("{}", node);

      // adjacency
      vector<thrift::Adjacency> adjs;
      addAdj(i, j + 1, "0/1", adjs, n, "0/3");
      addAdj(i - 1, j, "0/2", adjs, n, "0/4");
      addAdj(i, j - 1, "0/3", adjs, n, "0/1");
      addAdj(i + 1, j, "0/4", adjs, n, "0/2");
      auto adjacencyDb = createAdjDb(nodeName, adjs, node + 1);
      linkState.updateAdjacencyDatabase(adjacencyDb, kTestingAreaName);

      // prefix
      auto addrV6 = toIpPrefix(nodeToPrefixV6(node));
      updatePrefixDatabase(
          prefixState, createPrefixDb(nodeName, {createPrefixEntry(addrV6)}));
    }
  }
}

class GridTopologyFixture : public ::testing::TestWithParam<int> {
 public:
  GridTopologyFixture()
      : spfSolver(
            nodeName, false, true /* enable node segment label */, false) {}

 protected:
  void
  SetUp() override {
    n = GetParam();
    areaLinkStates.emplace(
        kTestingAreaName, LinkState(kTestingAreaName, kTestingNodeName));
    auto& linkState = areaLinkStates.at(kTestingAreaName);
    createGrid(linkState, prefixState, n);
  }

  // n * n grid
  int n;
  std::string nodeName{"1"};
  SpfSolver spfSolver;

  std::unordered_map<std::string, LinkState> areaLinkStates;
  PrefixState prefixState;
};

INSTANTIATE_TEST_CASE_P(
    GridTopology, GridTopologyFixture, ::testing::Range(2, 17, 2));

// distance from node a to b in the grid n*n of unit link cost
int
gridDistance(int a, int b, int n) {
  int x_a = a % n, x_b = b % n;
  int y_a = a / n, y_b = b / n;

  return abs(x_a - x_b) + abs(y_a - y_b);
}

TEST_P(GridTopologyFixture, ShortestPathTest) {
  vector<string> allNodes;
  for (int i = 0; i < n * n; ++i) {
    allNodes.push_back(fmt::format("{}", i));
  }

  auto routeMap = getRouteMap(spfSolver, allNodes, areaLinkStates, prefixState);

  // unicastRoutes => n^2 * (n^2 - 1)
  // node label routes => n^2 * n^2
  // adj label routes => 2 * 2 * n * (n - 1) (each link is reported twice)
  // Total => 2n^4 - n^2
  EXPECT_EQ(2 * n * n * n * n - n * n, routeMap.size());

  int src{0}, dst{0};
  NextHops nextHops;
  // validate route
  // 1) from corner to corner
  // primary diagnal
  src = 0;
  dst = n * n - 1;
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops = routeMap[make_pair(fmt::format("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric());

  // secondary diagnal
  src = n - 1;
  dst = n * (n - 1);
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops = routeMap[make_pair(fmt::format("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric());
  src = 0;
  dst = folly::Random::rand32() % (n * n - 1) + 1;
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops = routeMap[make_pair(fmt::format("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric());
  while ((dst = folly::Random::rand32() % (n * n)) == src) {
  }
  LOG(INFO) << "distance " << src << " -> " << dst << ": "
            << gridDistance(src, dst, n);
  nextHops = routeMap[make_pair(fmt::format("{}", src), nodeToPrefixV6(dst))];
  EXPECT_EQ(gridDistance(src, dst, n), *nextHops.begin()->metric());
}

// measure SPF execution time for large networks
TEST(GridTopology, StressTest) {
  if (!FLAGS_stress_test) {
    return;
  }
  std::string nodeName("1");
  SpfSolver spfSolver(nodeName, false, true, true);

  std::unordered_map<std::string, LinkState> areaLinkStates;
  areaLinkStates.emplace(
      kTestingAreaName, LinkState(kTestingAreaName, kTestingNodeName));
  auto& linkState = areaLinkStates.at(kTestingAreaName);
  PrefixState prefixState;

  createGrid(linkState, prefixState, 99);
  spfSolver.buildRouteDb("523", areaLinkStates, prefixState);
}

//
// Start the decision thread and simulate KvStore communications
// Expect proper RouteDatabase publications to appear
//
class DecisionTestFixture : public ::testing::Test {
 protected:
  void
  SetUp() override {
    // Reset all global counters
    fb303::fbData->resetAllData();

    auto tConfig = createConfig();
    config = std::make_shared<Config>(tConfig);

    decision = make_shared<Decision>(
        config,
        peerUpdatesQueue.getReader(),
        kvStoreUpdatesQueue.getReader(),
        staticRouteUpdatesQueue.getReader(),
        routeUpdatesQueue);

    decisionThread = std::make_unique<std::thread>([this]() {
      LOG(INFO) << "Decision thread starting";
      decision->run();
      LOG(INFO) << "Decision thread finishing";
    });
    decision->waitUntilRunning();

    // Reset initial KvStore sync event as not sent.
    kvStoreSyncEventSent = false;

    // Override default rib policy file with file based on thread id.
    // This ensures stress run will use different file for each run.
    FLAGS_rib_policy_file = fmt::format(
        "/dev/shm/rib_policy.txt.{}",
        std::hash<std::thread::id>{}(std::this_thread::get_id()));

    // Publish initial peers.
    publishInitialPeers();
  }

  void
  TearDown() override {
    peerUpdatesQueue.close();
    kvStoreUpdatesQueue.close();
    staticRouteUpdatesQueue.close();
    routeUpdatesQueue.close();

    // Delete default rib policy file.
    remove(FLAGS_rib_policy_file.c_str());

    LOG(INFO) << "Stopping the decision thread";
    decision->stop();
    decisionThread->join();
    LOG(INFO) << "Decision thread got stopped";
  }

  virtual openr::thrift::OpenrConfig
  createConfig() {
    auto tConfig = getBasicOpenrConfig(
        "1",
        {},
        true /* enable v4 */,
        true /* enableSegmentRouting */,
        true /* dryrun */,
        false /* enableV4OverV6Nexthop */);

    // timeout to wait until decision debounce
    // (i.e. spf recalculation, route rebuild) finished
    tConfig.decision_config()->debounce_min_ms() = debounceTimeoutMin.count();
    tConfig.decision_config()->debounce_max_ms() = debounceTimeoutMax.count();
    tConfig.enable_best_route_selection() = true;
    tConfig.decision_config()->save_rib_policy_min_ms() = 500;
    tConfig.decision_config()->save_rib_policy_max_ms() = 2000;
    return tConfig;
  }

  virtual void
  publishInitialPeers() {
    thrift::PeersMap peers;
    peers.emplace("2", thrift::PeerSpec());
    PeerEvent peerEvent{
        {kTestingAreaName, AreaPeerEvent(peers, {} /*peersToDel*/)}};
    peerUpdatesQueue.push(std::move(peerEvent));
  }

  //
  // member methods
  //

  void
  verifyReceivedRoutes(const folly::CIDRNetwork& network, bool isRemoved) {
    auto startTime = std::chrono::steady_clock::now();
    while (true) {
      auto endTime = std::chrono::steady_clock::now();
      if (endTime - startTime > debounceTimeoutMax) {
        ASSERT_TRUE(0) << fmt::format(
            "Timeout verifying prefix: {} in prefix-state. Time limit: {}",
            folly::IPAddress::networkToString(network),
            debounceTimeoutMax.count());
      }

      // Expect best route selection to be populated in route-details for addr2
      thrift::ReceivedRouteFilter filter;
      filter.prefixes() = std::vector<thrift::IpPrefix>({toIpPrefix(network)});
      auto routes = decision->getReceivedRoutesFiltered(filter).get();
      if ((not isRemoved) and routes->size()) {
        return;
      }
      if (isRemoved and routes->empty()) {
        return;
      }
      // yield CPU
      std::this_thread::yield();
    }
  }

  std::unordered_map<std::string, thrift::RouteDatabase>
  dumpRouteDb(const vector<string>& allNodes) {
    std::unordered_map<std::string, thrift::RouteDatabase> routeMap;

    for (string const& node : allNodes) {
      auto resp = decision->getDecisionRouteDb(node).get();
      EXPECT_TRUE(resp);
      EXPECT_EQ(node, *resp->thisNodeName());

      // Sort next-hop lists to ease verification code
      for (auto& route : *resp->unicastRoutes()) {
        std::sort(route.nextHops()->begin(), route.nextHops()->end());
      }
      for (auto& route : *resp->mplsRoutes()) {
        std::sort(route.nextHops()->begin(), route.nextHops()->end());
      }

      routeMap[node] = std::move(*resp);
    }

    return routeMap;
  }

  DecisionRouteUpdate
  recvRouteUpdates() {
    auto maybeRouteDb = routeUpdatesQueueReader.get();
    EXPECT_FALSE(maybeRouteDb.hasError());
    auto routeDbDelta = maybeRouteDb.value();
    return routeDbDelta;
  }

  // publish routeDb
  void
  sendKvPublication(
      const thrift::Publication& tPublication,
      bool prefixPubExists = true,
      bool withSelfAdj = false) {
    kvStoreUpdatesQueue.push(tPublication);
    if (prefixPubExists and (not kvStoreSyncEventSent)) {
      // Send KvStore initial synced event.
      kvStoreUpdatesQueue.push(thrift::InitializationEvent::KVSTORE_SYNCED);
      kvStoreSyncEventSent = true;

      if (withSelfAdj) {
        // Send Self Adjacencies synced event.
        kvStoreUpdatesQueue.push(
            thrift::InitializationEvent::ADJACENCY_DB_SYNCED);
      }
    }
  }

  void
  sendStaticRoutesUpdate(const thrift::RouteDatabaseDelta& publication) {
    DecisionRouteUpdate routeUpdate;
    for (const auto& unicastRoute : *publication.unicastRoutesToUpdate()) {
      auto nhs = std::unordered_set<thrift::NextHopThrift>(
          unicastRoute.nextHops()->begin(), unicastRoute.nextHops()->end());
      routeUpdate.addRouteToUpdate(
          RibUnicastEntry(toIPNetwork(*unicastRoute.dest()), std::move(nhs)));
    }
    for (const auto& prefix : *publication.unicastRoutesToDelete()) {
      routeUpdate.unicastRoutesToDelete.push_back(toIPNetwork(prefix));
    }
    for (const auto& mplsRoute : *publication.mplsRoutesToUpdate()) {
      auto nhs = std::unordered_set<thrift::NextHopThrift>(
          mplsRoute.nextHops()->begin(), mplsRoute.nextHops()->end());
      routeUpdate.addMplsRouteToUpdate(
          RibMplsEntry(*mplsRoute.topLabel(), std::move(nhs)));
    }
    for (const auto& label : *publication.mplsRoutesToDelete()) {
      routeUpdate.mplsRoutesToDelete.push_back(label);
    }
    staticRouteUpdatesQueue.push(routeUpdate);
  }

  thrift::Value
  createPrefixValue(
      const string& node,
      int64_t version,
      thrift::PrefixDatabase const& prefixDb) {
    return createThriftValue(
        version,
        node,
        writeThriftObjStr(prefixDb, serializer),
        Constants::kTtlInfinity /* ttl */,
        0 /* ttl version */,
        0 /* hash */);
  }

  thrift::Value
  createPrefixValue(
      const string& node,
      int64_t version,
      const vector<thrift::IpPrefix>& prefixes = {},
      const string& area = kTestingAreaName) {
    vector<thrift::PrefixEntry> prefixEntries;
    for (const auto& prefix : prefixes) {
      prefixEntries.emplace_back(createPrefixEntry(prefix));
    }
    return createPrefixValue(
        node, version, createPrefixDb(node, prefixEntries));
  }

  /**
   * Check whether two DecisionRouteUpdates to be equal
   */
  bool
  checkEqualRoutesDelta(
      DecisionRouteUpdate& lhsC, thrift::RouteDatabaseDelta& rhs) {
    auto lhs = lhsC.toThrift();
    std::sort(
        lhs.unicastRoutesToUpdate()->begin(),
        lhs.unicastRoutesToUpdate()->end());
    std::sort(
        rhs.unicastRoutesToUpdate()->begin(),
        rhs.unicastRoutesToUpdate()->end());

    std::sort(
        lhs.unicastRoutesToDelete()->begin(),
        lhs.unicastRoutesToDelete()->end());
    std::sort(
        rhs.unicastRoutesToDelete()->begin(),
        rhs.unicastRoutesToDelete()->end());

    return *lhs.unicastRoutesToUpdate() == *rhs.unicastRoutesToUpdate() &&
        *lhs.unicastRoutesToDelete() == *rhs.unicastRoutesToDelete();
  }

  //
  // member variables
  //

  // Thrift serializer object for serializing/deserializing of thrift objects
  // to/from bytes
  CompactSerializer serializer{};

  std::shared_ptr<Config> config;
  messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue;
  messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
  messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
  messaging::RQueue<DecisionRouteUpdate> routeUpdatesQueueReader{
      routeUpdatesQueue.getReader()};

  // Decision owned by this wrapper.
  std::shared_ptr<Decision> decision{nullptr};

  // Thread in which decision will be running.
  std::unique_ptr<std::thread> decisionThread{nullptr};

  // Initial KvStore synced signal is sent.
  bool kvStoreSyncEventSent{false};
};

TEST_F(DecisionTestFixture, StopDecisionWithoutInitialPeers) {
  // Close all queues.
  routeUpdatesQueue.close();
  kvStoreUpdatesQueue.close();
  staticRouteUpdatesQueue.close();
  // Initial peers are not received yet.
  peerUpdatesQueue.close();

  // decision module could stop.
  decision->stop();
}

// The following topology is used:
//
// 1---2---3
//
// We upload the link 1---2 with the initial sync and later publish
// the 2---3 link information. We then request the full routing dump
// from the decision process via respective socket.

TEST_F(DecisionTestFixture, BasicOperations) {
  //
  // publish the link state info to KvStore
  //

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});
  auto routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  auto routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  // self mpls route and node 2 mpls route label route
  EXPECT_EQ(2, routeDbDelta.mplsRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());

  auto routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes()->begin(), routeDb.unicastRoutes()->end());
  std::sort(routeDb.mplsRoutes()->begin(), routeDb.mplsRoutes()->end());

  auto routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));

  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));
  //
  // publish the link state info to KvStore via the KvStore pub socket
  // we simulate adding a new router R3
  //

  // Some tricks here; we need to bump the time-stamp on router 2's data, so
  // it can override existing; for router 3 we publish new key-value

  publication = createThriftPublication(
      {{"adj:3", createAdjValue(serializer, "3", 1, {adj32}, false, 3)},
       {"adj:2", createAdjValue(serializer, "2", 3, {adj21, adj23}, false, 2)},
       {"adj:4",
        createAdjValue(serializer, "4", 1, {}, false, 4)}, // No adjacencies
       createPrefixKeyValue("3", 1, addr3)},
      {},
      {},
      {});
  routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes()->begin(),
      routeDbBefore.unicastRoutes()->end());
  std::sort(
      routeDbBefore.mplsRoutes()->begin(), routeDbBefore.mplsRoutes()->end());
  sendKvPublication(publication);
  // validate routers

  // receive my local Decision routeDbDelta publication
  routeDbDelta = recvRouteUpdates();
  // only expect to add a route to addr3
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(
      routeDbDelta.unicastRoutesToUpdate.begin()->second.prefix,
      toIPNetwork(addr3));
  EXPECT_EQ(1, routeDbDelta.mplsRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());

  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes()->begin(), routeDb.unicastRoutes()->end());
  std::sort(routeDb.mplsRoutes()->begin(), routeDb.mplsRoutes()->end());
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  fillRouteMap("1", routeMap, routeDb);
  // 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3))],
      NextHops({createNextHopFromAdj(adj12, false, 20)}));

  // dump other nodes' routeDB
  auto routeDbMap = dumpRouteDb({"2", "3"});
  EXPECT_EQ(2, routeDbMap["2"].unicastRoutes()->size());
  EXPECT_EQ(2, routeDbMap["3"].unicastRoutes()->size());
  for (auto& [key, value] : routeDbMap) {
    fillRouteMap(key, routeMap, value);
  }

  // 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3))],
      NextHops({createNextHopFromAdj(adj23, false, 10)}));

  // 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj32, false, 20)}));

  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2))],
      NextHops({createNextHopFromAdj(adj32, false, 10)}));

  // remove 3
  publication = createThriftPublication(
      thrift::KeyVals{},
      {"adj:3", "prefix:3", "adj:4"} /* expired keys */,
      {},
      {});

  routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes()->begin(),
      routeDbBefore.unicastRoutes()->end());
  std::sort(
      routeDbBefore.mplsRoutes()->begin(), routeDbBefore.mplsRoutes()->end());

  sendKvPublication(publication);
  routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToDelete.size());
  EXPECT_EQ(1, routeDbDelta.mplsRoutesToDelete.size());
  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes()->begin(), routeDb.unicastRoutes()->end());
  std::sort(routeDb.mplsRoutes()->begin(), routeDb.mplsRoutes()->end());

  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12, false, 10)}));

  publication = createThriftPublication(
      {{"adj:3", createAdjValue(serializer, "3", 1, {adj32}, false, 3)},
       {"adj:2", createAdjValue(serializer, "2", 4, {adj21, adj23}, false, 2)},
       createPrefixKeyValue("3", 1, addr3)},
      {},
      {},
      {});
  routeDbBefore = dumpRouteDb({"1"})["1"];
  std::sort(
      routeDbBefore.unicastRoutes()->begin(),
      routeDbBefore.unicastRoutes()->end());
  std::sort(
      routeDbBefore.mplsRoutes()->begin(), routeDbBefore.mplsRoutes()->end());
  sendKvPublication(publication);
  // validate routers

  // receive my local Decision routeDbDelta publication
  routeDbDelta = recvRouteUpdates();
  // only expect to add a route to addr3
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(
      routeDbDelta.unicastRoutesToUpdate.begin()->second.prefix,
      toIPNetwork(addr3));
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(1, routeDbDelta.mplsRoutesToUpdate.size());

  routeDb = dumpRouteDb({"1"})["1"];
  std::sort(routeDb.unicastRoutes()->begin(), routeDb.unicastRoutes()->end());
  std::sort(routeDb.mplsRoutes()->begin(), routeDb.mplsRoutes()->end());
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
}

/**
 * Publish all types of update to Decision and expect that Decision emits
 * a full route database that includes all the routes as its first update.
 *
 * Types of information updated
 * - Adjacencies (with MPLS labels)
 * - Prefixes
 */
TEST_F(DecisionTestFixture, InitialRouteUpdate) {
  // Send adj publication
  sendKvPublication(
      createThriftPublication(
          {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
           {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)}},
          {},
          {},
          {}),
      false /*prefixPubExists*/);

  // Send prefix publication
  sendKvPublication(createThriftPublication(
      {createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {}));

  // Receive & verify all the expected updates
  auto routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  EXPECT_EQ(0, routeDbDelta.mplsRoutesToDelete.size());
  EXPECT_EQ(0, routeDbDelta.unicastRoutesToDelete.size());
}

/*
 * Route Origination Test:
 *  - Test 1:
 *    - static prefixes advertised from `PrefixManager`
 *    - expect `routesToUpdate` contains prefixes advertised;
 *  - Test 2:
 *    - advertise SAME prefix from `Decision`(i.e. prefix update in KvStore)
 *    - expect `routesToUpdate` contains prefixes BUT NHs overridden
 *      by `decision`;
 *  - Test 3:
 *    - withdraw static prefixes from `PrefixManager`
 *    - expect `routesToUpdate` contains prefixes BUT NHs overridden
 *      by `deicision`;
 *  - Test 4:
 *    - re-advertise static prefixes from `PrefixManager`
 *    - expect `routesToUpdate` contains prefixes BUT NHs overridden
 *      by `deicision`;
 *  - Test 5:
 *    - withdraw prefixes from `Decision`(i.e. prefix deleted in KvStore)
 *    - expect `routesToUpdate` contains static prefixes from `PrefixManager`
 *  - Test 6:
 *    - withdraw static prefixes from `PrefixManager`
 *    - expect `routesToDelete` contains static prefixes
 *  - Test7: Received self-advertised prefix publication from KvStore.
 *    - No routes will be generated by SpfSolver for self originated prefixes.
 *    - Since there are no existing routes for the prefix, no delete routes
 *      will be generated.
 */
TEST_F(DecisionTestFixture, RouteOrigination) {
  // eventbase to control the pace of tests
  OpenrEventBase evb;

  // prepare prefix/nexthops structure
  const std::string prefixV4 = "10.0.0.1/24";
  const std::string prefixV6 = "fe80::1/64";

  thrift::NextHopThrift nhV4, nhV6;
  nhV4.address() = toBinaryAddress(Constants::kLocalRouteNexthopV4.toString());
  nhV6.address() = toBinaryAddress(Constants::kLocalRouteNexthopV6.toString());

  const auto networkV4 = folly::IPAddress::createNetwork(prefixV4);
  const auto networkV6 = folly::IPAddress::createNetwork(prefixV6);
  auto routeV4 = createUnicastRoute(toIpPrefix(prefixV4), {nhV4});
  auto routeV6 = createUnicastRoute(toIpPrefix(prefixV6), {nhV6});

  // Send adj publication
  // ATTN: to trigger `buildRouteDb()`. Must provide LinkState
  //      info containing self-node id("1")
  auto scheduleAt = std::chrono::milliseconds{0};
  evb.scheduleTimeout(scheduleAt, [&]() noexcept {
    sendKvPublication(createThriftPublication(
        {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
         {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)}},
        {},
        {},
        {}));
  });

  //
  // Test1: advertise prefixes from `PrefixManager`
  //
  evb.scheduleTimeout(scheduleAt += 3 * debounceTimeoutMax, [&]() noexcept {
    auto routeDbDelta = recvRouteUpdates();

    LOG(INFO) << "Advertising static prefixes from PrefixManager";

    thrift::RouteDatabaseDelta routeDb;
    routeDb.unicastRoutesToUpdate()->emplace_back(routeV4);
    routeDb.unicastRoutesToUpdate()->emplace_back(routeV6);
    sendStaticRoutesUpdate(std::move(routeDb));
  });

  // wait for debouncer to fire
  evb.scheduleTimeout(
      scheduleAt += (debounceTimeoutMax + std::chrono::milliseconds(100)),
      [&]() noexcept {
        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(2));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        const auto& routeToUpdate = routeDbDelta.unicastRoutesToUpdate;
        ASSERT_TRUE(routeToUpdate.count(networkV4));
        ASSERT_TRUE(routeToUpdate.count(networkV6));

        EXPECT_THAT(
            routeToUpdate.at(networkV4), testing::Truly([&networkV4](auto i) {
              return i.prefix == networkV4 and i.doNotInstall == false;
            }));
        EXPECT_THAT(
            routeToUpdate.at(networkV6), testing::Truly([&networkV6](auto i) {
              return i.prefix == networkV6 and i.doNotInstall == false;
            }));
        // NOTE: no SAME route from decision, program DROP route
        EXPECT_THAT(
            routeToUpdate.at(networkV4).nexthops,
            testing::UnorderedElementsAre(nhV4));
        EXPECT_THAT(
            routeToUpdate.at(networkV6).nexthops,
            testing::UnorderedElementsAre(nhV6));
      });

  //
  // Test2: advertise SAME prefixes from `Decision`
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        LOG(INFO) << "Advertising SAME prefixes from Decision";

        sendKvPublication(createThriftPublication(
            {createPrefixKeyValue("2", 1, toIpPrefix(prefixV4)),
             createPrefixKeyValue("2", 1, toIpPrefix(prefixV6))},
            {},
            {},
            {}));

        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(2));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        const auto& routeToUpdate = routeDbDelta.unicastRoutesToUpdate;
        ASSERT_TRUE(routeToUpdate.count(networkV4));
        ASSERT_TRUE(routeToUpdate.count(networkV6));

        // NOTE: route from decision takes higher priority
        EXPECT_THAT(
            routeToUpdate.at(networkV4).nexthops,
            Not(testing::UnorderedElementsAre(nhV4)));
        EXPECT_THAT(
            routeToUpdate.at(networkV6).nexthops,
            Not(testing::UnorderedElementsAre(nhV6)));
      });

  //
  // Test3: withdraw prefixes from `PrefixManager`
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        LOG(INFO) << "Withdrawing static prefixes from PrefixManager";

        thrift::RouteDatabaseDelta routeDb;
        routeDb.unicastRoutesToDelete()->emplace_back(toIpPrefix(networkV4));
        routeDb.unicastRoutesToDelete()->emplace_back(toIpPrefix(networkV6));
        sendStaticRoutesUpdate(std::move(routeDb));
      });

  // wait for debouncer to fire
  evb.scheduleTimeout(
      scheduleAt += (debounceTimeoutMax + std::chrono::milliseconds(100)),
      [&]() noexcept {
        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(2));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        const auto& routeToUpdate = routeDbDelta.unicastRoutesToUpdate;
        ASSERT_TRUE(routeToUpdate.count(networkV4));
        ASSERT_TRUE(routeToUpdate.count(networkV6));

        // NOTE: route from Decision is the ONLY output
        EXPECT_THAT(
            routeToUpdate.at(networkV4).nexthops,
            Not(testing::UnorderedElementsAre(nhV4)));
        EXPECT_THAT(
            routeToUpdate.at(networkV6).nexthops,
            Not(testing::UnorderedElementsAre(nhV6)));
      });

  //
  // Test4: re-advertise prefixes from `PrefixManager`
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        LOG(INFO) << "Re-advertising static prefixes from PrefixManager";

        thrift::RouteDatabaseDelta routeDb;
        routeDb.unicastRoutesToUpdate()->emplace_back(routeV4);
        routeDb.unicastRoutesToUpdate()->emplace_back(routeV6);
        sendStaticRoutesUpdate(std::move(routeDb));
      });

  // wait for debouncer to fire
  evb.scheduleTimeout(
      scheduleAt += (debounceTimeoutMax + std::chrono::milliseconds(100)),
      [&]() noexcept {
        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(2));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        const auto& routeToUpdate = routeDbDelta.unicastRoutesToUpdate;
        ASSERT_TRUE(routeToUpdate.count(networkV4));
        ASSERT_TRUE(routeToUpdate.count(networkV6));

        // NOTE: route from decision takes higher priority
        EXPECT_THAT(
            routeToUpdate.at(networkV4).nexthops,
            Not(testing::UnorderedElementsAre(nhV4)));
        EXPECT_THAT(
            routeToUpdate.at(networkV6).nexthops,
            Not(testing::UnorderedElementsAre(nhV6)));
      });

  //
  // Test5: withdraw prefixes from `Decision`
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        LOG(INFO) << "Withdrawing prefixes from Decision";

        sendKvPublication(createThriftPublication(
            {createPrefixKeyValue(
                 "2", 1, toIpPrefix(prefixV4), kTestingAreaName, true),
             createPrefixKeyValue(
                 "2", 1, toIpPrefix(prefixV6), kTestingAreaName, true)},
            {},
            {},
            {}));

        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(2));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        const auto& routeToUpdate = routeDbDelta.unicastRoutesToUpdate;
        ASSERT_TRUE(routeToUpdate.count(networkV4));
        ASSERT_TRUE(routeToUpdate.count(networkV6));

        // NOTE: no routes from decision. Program DROP routes.
        EXPECT_THAT(
            routeToUpdate.at(networkV4).nexthops,
            testing::UnorderedElementsAre(nhV4));
        EXPECT_THAT(
            routeToUpdate.at(networkV6).nexthops,
            testing::UnorderedElementsAre(nhV6));
      });

  //
  // Test6: withdraw prefixes from `PrefixManager`
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        LOG(INFO) << "Withdrawing prefixes from PrefixManager";

        thrift::RouteDatabaseDelta routeDb;
        routeDb.unicastRoutesToDelete()->emplace_back(toIpPrefix(networkV4));
        routeDb.unicastRoutesToDelete()->emplace_back(toIpPrefix(networkV6));
        sendStaticRoutesUpdate(std::move(routeDb));
      });

  // wait for debouncer to fire
  evb.scheduleTimeout(
      scheduleAt += (debounceTimeoutMax + std::chrono::milliseconds(100)),
      [&]() noexcept {
        // Receive & verify all the expected updates
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(0));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(2));

        EXPECT_THAT(
            routeDbDelta.unicastRoutesToDelete,
            testing::UnorderedElementsAre(networkV4, networkV6));
      });

  //
  // Test7: Received self-advertised publication from KvStore. No routes will be
  // generated.
  //
  evb.scheduleTimeout(
      scheduleAt += std::chrono::milliseconds(100), [&]() noexcept {
        sendKvPublication(createThriftPublication(
            {createPrefixKeyValue(
                "1", 1, toIpPrefix(prefixV4), kTestingAreaName)},
            {},
            {},
            {}));
        // No unicast routes are generated.
        auto routeDbDelta = recvRouteUpdates();
        EXPECT_THAT(routeDbDelta.unicastRoutesToUpdate, testing::SizeIs(0));
        EXPECT_THAT(routeDbDelta.unicastRoutesToDelete, testing::SizeIs(0));

        evb.stop();
      });

  // magic happens
  evb.run();
}

// The following topology is used:
//  1--- A ---2
//  |         |
//  B         A
//  |         |
//  3--- B ---4
//
// area A: adj12, adj24
// area B: adj13, adj34
TEST_F(DecisionTestFixture, MultiAreaBestPathCalculation) {
  //
  // publish area A adj and prefix
  // "1" originate addr1 into A
  // "2" originate addr2 into A
  //
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21, adj24}, false, 2)},
       {"adj:4", createAdjValue(serializer, "4", 1, {adj42}, false, 4)},
       createPrefixKeyValue("1", 1, addr1, kTestingAreaName),
       createPrefixKeyValue("2", 1, addr2, kTestingAreaName)},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      kTestingAreaName);

  sendKvPublication(publication);
  recvRouteUpdates();

  //
  // publish area B adj and prefix
  // "3" originate addr3 into B
  // "4" originate addr4 into B
  //
  publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj13}, false, 1)},
       {"adj:3", createAdjValue(serializer, "3", 1, {adj31, adj34}, false, 3)},
       {"adj:4", createAdjValue(serializer, "4", 1, {adj43}, false, 4)},
       createPrefixKeyValue("3", 1, addr3, "B"),
       createPrefixKeyValue("4", 1, addr4, "B")},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      "B");
  sendKvPublication(publication);
  recvRouteUpdates();

  auto routeDb1 = dumpRouteDb({"1"})["1"];
  auto routeDb2 = dumpRouteDb({"2"})["2"];
  auto routeDb3 = dumpRouteDb({"3"})["3"];
  auto routeDb4 = dumpRouteDb({"4"})["4"];

  // routeDb1 from node "1"
  {
    auto routeToAddr2 = createUnicastRoute(
        addr2,
        {createNextHopFromAdj(
            adj12, false, 10, std::nullopt, kTestingAreaName)});
    auto routeToAddr3 = createUnicastRoute(
        addr3, {createNextHopFromAdj(adj13, false, 10, std::nullopt, "B")});
    // addr4 is only originated in area B
    auto routeToAddr4 = createUnicastRoute(
        addr4, {createNextHopFromAdj(adj13, false, 20, std::nullopt, "B")});
    EXPECT_THAT(*routeDb1.unicastRoutes(), testing::SizeIs(3));
    EXPECT_THAT(
        *routeDb1.unicastRoutes(),
        testing::UnorderedElementsAre(
            routeToAddr2, routeToAddr3, routeToAddr4));
  }

  // routeDb2 from node "2" will only see addr1 in area A
  {
    auto routeToAddr1 = createUnicastRoute(
        addr1,
        {createNextHopFromAdj(
            adj21, false, 10, std::nullopt, kTestingAreaName)});
    EXPECT_THAT(*routeDb2.unicastRoutes(), testing::SizeIs(1));
    EXPECT_THAT(
        *routeDb2.unicastRoutes(), testing::UnorderedElementsAre(routeToAddr1));
  }

  // routeDb3 will only see addr4 in area B
  {
    auto routeToAddr4 = createUnicastRoute(
        addr4, {createNextHopFromAdj(adj34, false, 10, std::nullopt, "B")});
    EXPECT_THAT(*routeDb3.unicastRoutes(), testing::SizeIs(1));
    EXPECT_THAT(
        *routeDb3.unicastRoutes(), testing::UnorderedElementsAre(routeToAddr4));
  }

  // routeDb4
  {
    auto routeToAddr2 = createUnicastRoute(
        addr2,
        {createNextHopFromAdj(
            adj42, false, 10, std::nullopt, kTestingAreaName)});
    auto routeToAddr3 = createUnicastRoute(
        addr3, {createNextHopFromAdj(adj43, false, 10, std::nullopt, "B")});
    // addr1 is only originated in area A
    auto routeToAddr1 = createUnicastRoute(
        addr1,
        {createNextHopFromAdj(
            adj42, false, 20, std::nullopt, kTestingAreaName)});
    EXPECT_THAT(*routeDb4.unicastRoutes(), testing::SizeIs(3));
    EXPECT_THAT(
        *routeDb4.unicastRoutes(),
        testing::UnorderedElementsAre(
            routeToAddr2, routeToAddr3, routeToAddr1));
  }

  //
  // "1" originate addr1 into B
  //
  publication = createThriftPublication(
      {createPrefixKeyValue("1", 1, addr1, "B")},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      "B");
  sendKvPublication(publication);
  recvRouteUpdates();

  routeDb3 = dumpRouteDb({"3"})["3"];
  routeDb4 = dumpRouteDb({"4"})["4"];

  // routeMap3 now should see addr1 in areaB
  {
    auto routeToAddr1 = createUnicastRoute(
        addr1, {createNextHopFromAdj(adj31, false, 10, std::nullopt, "B")});
    EXPECT_THAT(*routeDb3.unicastRoutes(), testing::Contains(routeToAddr1));
  }

  // routeMap4 now could reach addr1 through areaA or areaB
  {
    auto routeToAddr1 = createUnicastRoute(
        addr1,
        {createNextHopFromAdj(adj43, false, 20, std::nullopt, "B"),
         createNextHopFromAdj(
             adj42, false, 20, std::nullopt, kTestingAreaName)});
    EXPECT_THAT(*routeDb4.unicastRoutes(), testing::Contains(routeToAddr1));
  }
}

// MultiArea Tology topology is used:
//  1--- A ---2
//  |
//  B
//  |
//  3
//
// area A: adj12
// area B: adj13
TEST_F(DecisionTestFixture, SelfReditributePrefixPublication) {
  //
  // publish area A adj and prefix
  // "2" originate addr2 into A
  //
  auto originKeyStr =
      PrefixKey("2", toIPNetwork(addr2), kTestingAreaName).getPrefixKeyV2();
  auto originPfx = createPrefixEntry(addr2);
  originPfx.area_stack() = {"65000"};
  auto originPfxVal =
      createPrefixValue("2", 1, createPrefixDb("2", {originPfx}));

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       {originKeyStr, originPfxVal}},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      kTestingAreaName);

  sendKvPublication(publication);
  recvRouteUpdates();

  //
  // publish area B adj and prefix
  //
  publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj13}, false, 1)},
       {"adj:3", createAdjValue(serializer, "3", 1, {adj31}, false, 3)}},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      "B");
  sendKvPublication(publication);
  recvRouteUpdates();

  //
  // "1" reditribute addr2 into B
  //   - this should not cause prefix db update
  //   - not route update
  //
  auto redistributeKeyStr =
      PrefixKey("1", toIPNetwork(addr2), "B").getPrefixKeyV2();
  auto redistributePfx = createPrefixEntry(addr2, thrift::PrefixType::RIB);
  redistributePfx.area_stack() = {"65000", kTestingAreaName};
  auto redistributePfxVal =
      createPrefixValue("1", 1, createPrefixDb("1", {redistributePfx}, "B"));

  publication = createThriftPublication(
      {{redistributeKeyStr, redistributePfxVal}},
      {}, /* expiredKeys */
      {}, /* nodeIds */
      {}, /* keysToUpdate */
      "B");
  sendKvPublication(publication);

  // wait for publication to be processed
  /* sleep override */
  std::this_thread::sleep_for(
      debounceTimeoutMax + std::chrono::milliseconds(100));

  EXPECT_EQ(0, routeUpdatesQueueReader.size());
}

/**
 * Exhaustively RibPolicy feature in Decision. The intention here is to
 * verify the functionality of RibPolicy in Decision module. RibPolicy
 * is also unit-tested for it's complete correctness and we don't aim
 * it here.
 *
 * Test covers
 * - Get policy without setting (exception case)
 * - Set policy
 * - Get policy after setting
 * - Verify that set-policy triggers the route database change (apply policy)
 * - Set the policy with 0 weight. See that route dis-appears
 * - Expire policy. Verify it triggers the route database change (undo policy)
 */
TEST_F(DecisionTestFixture, RibPolicy) {
  // Setup topology and prefixes. 1 unicast route will be computed
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});
  sendKvPublication(publication);

  // Expect route update. Verify next-hop weight to be 0 (ECMP)
  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
    EXPECT_EQ(
        0,
        *updates.unicastRoutesToUpdate.begin()
             ->second.nexthops.begin()
             ->weight());
  }

  // Get policy test. Expect failure
  EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);

  // Create rib policy
  thrift::RibRouteActionWeight actionWeight;
  actionWeight.neighbor_to_weight()->emplace("2", 2);
  thrift::RibPolicyStatement policyStatement;
  policyStatement.matcher()->prefixes() =
      std::vector<thrift::IpPrefix>({addr2});
  policyStatement.action()->set_weight() = actionWeight;
  thrift::RibPolicy policy;
  policy.statements()->emplace_back(policyStatement);
  policy.ttl_secs() = 1;

  // Set rib policy
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());

  // Get rib policy and verify
  {
    auto retrievedPolicy = decision->getRibPolicy().get();
    EXPECT_EQ(*policy.statements(), *retrievedPolicy.statements());
    EXPECT_GE(*policy.ttl_secs(), *retrievedPolicy.ttl_secs());
  }

  // Expect the route database change with next-hop weight to be 2
  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
    EXPECT_EQ(
        2,
        *updates.unicastRoutesToUpdate.begin()
             ->second.nexthops.begin()
             ->weight());
  }

  // Set the policy with empty weight. Expect route remains intact and error
  // counter is reported
  policy.statements()->at(0).action()->set_weight()->neighbor_to_weight()["2"] =
      0;
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());
  {
    auto updates = recvRouteUpdates();
    EXPECT_EQ(1, updates.unicastRoutesToUpdate.size());
    ASSERT_EQ(0, updates.unicastRoutesToDelete.size());
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.count(toIPNetwork(addr2)));
    for (auto& nh :
         updates.unicastRoutesToUpdate.at(toIPNetwork(addr2)).nexthops) {
      EXPECT_EQ(0, *nh.weight());
    }
    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(1, counters.at("decision.rib_policy.invalidated_routes.count"));
  }

  // trigger addr2 recalc by flapping the advertisement
  publication = createThriftPublication(
      {createPrefixKeyValue(
          "2", 2, addr2, kTestingAreaName, true /* withdraw */)},
      {},
      {},
      {});
  sendKvPublication(publication);
  publication = createThriftPublication(
      {createPrefixKeyValue("2", 3, addr2)}, {}, {}, {});
  sendKvPublication(publication);

  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
    ASSERT_EQ(0, updates.unicastRoutesToDelete.size());
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.count(toIPNetwork(addr2)));
    for (auto& nh :
         updates.unicastRoutesToUpdate.at(toIPNetwork(addr2)).nexthops) {
      EXPECT_EQ(0, *nh.weight());
    }
    auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(2, counters.at("decision.rib_policy.invalidated_routes.count"));
  }

  // Let the policy expire. Wait for another route database change
  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(0, updates.unicastRoutesToUpdate.size());

    auto retrievedPolicy = decision->getRibPolicy().get();
    EXPECT_GE(0, *retrievedPolicy.ttl_secs());
  }
}

/**
 * Verifies that error is set if RibPolicy is invalid
 */
TEST_F(DecisionTestFixture, RibPolicyError) {
  // Set empty rib policy
  auto sf = decision->setRibPolicy(thrift::RibPolicy{});

  // Expect an error to be set immediately (validation happens inline)
  EXPECT_TRUE(sf.isReady());
  EXPECT_TRUE(sf.hasException());
  EXPECT_THROW(std::move(sf).get(), thrift::OpenrError);
}

/**
 * Verifies that a policy gets cleared
 */
TEST_F(DecisionTestFixture, RibPolicyClear) {
  // Setup topology and prefixes. 1 unicast route will be computed
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       {"prefix:1", createPrefixValue("1", 1, {addr1})},
       {"prefix:2", createPrefixValue("2", 1, {addr2})}},
      {},
      {},
      {});
  sendKvPublication(publication);

  // Expect route update.
  {
    auto updates = recvRouteUpdates();
    ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
    EXPECT_EQ(
        0,
        *updates.unicastRoutesToUpdate.begin()
             ->second.nexthops.begin()
             ->weight());
  }

  // Get policy test. Expect failure
  EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);

  // Create rib policy
  thrift::RibRouteActionWeight actionWeight;
  actionWeight.neighbor_to_weight()->emplace("2", 2);
  actionWeight.neighbor_to_weight()->emplace("1", 1);

  thrift::RibPolicyStatement policyStatement;
  policyStatement.matcher()->prefixes() =
      std::vector<thrift::IpPrefix>({addr2});
  policyStatement.action()->set_weight() = actionWeight;
  thrift::RibPolicy policy;
  policy.statements()->emplace_back(policyStatement);
  policy.ttl_secs() = 1;

  // Set rib policy
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());

  // Get rib policy and verify
  {
    auto retrievedPolicy = decision->getRibPolicy().get();
    EXPECT_EQ(*policy.statements(), *retrievedPolicy.statements());
    EXPECT_GE(*policy.ttl_secs(), *retrievedPolicy.ttl_secs());
  }

  // Expect route update. Verify next-hop weight to be 2 (ECMP)
  auto updates = recvRouteUpdates();
  ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
  EXPECT_EQ(
      2,
      *updates.unicastRoutesToUpdate.begin()
           ->second.nexthops.begin()
           ->weight());

  // Clear rib policy and expect nexthop weight change
  EXPECT_NO_THROW(decision->clearRibPolicy());

  updates = recvRouteUpdates();
  ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
  EXPECT_EQ(
      0,
      *updates.unicastRoutesToUpdate.begin()
           ->second.nexthops.begin()
           ->weight());

  // Verify that get rib policy throws no exception
  EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);
}

/**
 * Verifies that set/get APIs throws exception if RibPolicy feature is not
 * enabled.
 */
class DecisionNoRibPolicyTestFixture : public DecisionTestFixture {
  openr::thrift::OpenrConfig
  createConfig() override {
    auto tConfig = DecisionTestFixture::createConfig();
    // Disable rib_policy feature
    tConfig.enable_rib_policy() = false;

    return tConfig;
  }
};

TEST_F(DecisionNoRibPolicyTestFixture, RibPolicyFeatureKnob) {
  ASSERT_FALSE(config->isRibPolicyEnabled());

  // dummy event to unblock decision module from initialization
  PeerEvent event;
  peerUpdatesQueue.push(std::move(event));

  // SET
  {
    // Create valid rib policy
    thrift::RibRouteActionWeight actionWeight;
    actionWeight.neighbor_to_weight()->emplace("2", 2);
    thrift::RibPolicyStatement policyStatement;
    policyStatement.matcher()->prefixes() =
        std::vector<thrift::IpPrefix>({addr2});
    policyStatement.action()->set_weight() = actionWeight;
    thrift::RibPolicy policy;
    policy.statements()->emplace_back(policyStatement);
    policy.ttl_secs() = 1;

    auto sf = decision->setRibPolicy(policy);
    EXPECT_TRUE(sf.isReady());
    EXPECT_TRUE(sf.hasException());
    EXPECT_THROW(std::move(sf).get(), thrift::OpenrError);
  }

  // GET
  {
    auto sf = decision->getRibPolicy();
    EXPECT_TRUE(sf.isReady());
    EXPECT_TRUE(sf.hasException());
    EXPECT_THROW(std::move(sf).get(), thrift::OpenrError);
  }
}

/**
 * Test graceful restart support of Rib policy in Decision.
 *
 * Test covers
 * - Set policy
 * - Get policy after setting
 * - Wait longer than debounce time so Decision have saved Rib policy
 * - Create a new Decision instance to load the still live Rib policy
 * - Setup initial topology and prefixes to trigger route computation
 * - Verify that loaded Rib policy is applied on generated routes
 */
TEST_F(DecisionTestFixture, GracefulRestartSupportForRibPolicy) {
  auto saveRibPolicyMaxMs =
      *config->getConfig().decision_config()->save_rib_policy_max_ms();

  // Get policy test. Expect failure
  EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);

  // Create rib policy
  thrift::RibRouteActionWeight actionWeight;
  actionWeight.neighbor_to_weight()->emplace("2", 2);
  thrift::RibPolicyStatement policyStatement;
  policyStatement.matcher()->prefixes() =
      std::vector<thrift::IpPrefix>({addr2});
  policyStatement.action()->set_weight() = actionWeight;
  thrift::RibPolicy policy;
  policy.statements()->emplace_back(policyStatement);
  // Set policy ttl as long as 10*saveRibPolicyMaxMs.
  policy.ttl_secs() = saveRibPolicyMaxMs * 10 / 1000;

  // Set rib policy
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());

  // Get rib policy and verify
  {
    auto retrievedPolicy = decision->getRibPolicy().get();
    EXPECT_EQ(*policy.statements(), *retrievedPolicy.statements());
    EXPECT_GE(*policy.ttl_secs(), *retrievedPolicy.ttl_secs());
  }

  std::unique_ptr<Decision> decision{nullptr};
  std::unique_ptr<std::thread> decisionThread{nullptr};
  int scheduleAt{0};

  OpenrEventBase evb;
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += saveRibPolicyMaxMs),
      [&]() noexcept {
        // Wait for saveRibPolicyMaxMs to make sure Rib policy is saved to file.
        messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue;
        messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue;
        messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
        messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
        auto routeUpdatesQueueReader = routeUpdatesQueue.getReader();
        decision = std::make_unique<Decision>(
            config,
            peerUpdatesQueue.getReader(),
            kvStoreUpdatesQueue.getReader(),
            staticRouteUpdatesQueue.getReader(),
            routeUpdatesQueue);
        decisionThread =
            std::make_unique<std::thread>([&]() { decision->run(); });
        decision->waitUntilRunning();

        // Publish initial batch of detected peers.
        thrift::PeersMap peers;
        peers.emplace("2", thrift::PeerSpec());
        PeerEvent peerEvent{
            {kTestingAreaName, AreaPeerEvent(peers, {} /*peersToDel*/)}};
        peerUpdatesQueue.push(std::move(peerEvent));

        // Setup topology and prefixes. 1 unicast route will be computed
        auto publication = createThriftPublication(
            {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
             {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
             {"prefix:1", createPrefixValue("1", 1, {addr1})},
             {"prefix:2", createPrefixValue("2", 1, {addr2})}},
            {},
            {},
            {},
            kTestingAreaName);
        kvStoreUpdatesQueue.push(publication);
        kvStoreUpdatesQueue.push(thrift::InitializationEvent::KVSTORE_SYNCED);

        // Expect route update with live rib policy applied.
        auto maybeRouteDb = routeUpdatesQueueReader.get();
        EXPECT_FALSE(maybeRouteDb.hasError());
        auto updates = maybeRouteDb.value();
        ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
        EXPECT_EQ(
            2,
            *updates.unicastRoutesToUpdate.begin()
                 ->second.nexthops.begin()
                 ->weight());

        // Get rib policy and verify
        auto retrievedPolicy = decision->getRibPolicy().get();
        EXPECT_EQ(*policy.statements(), *retrievedPolicy.statements());
        EXPECT_GE(*policy.ttl_secs(), *retrievedPolicy.ttl_secs());

        kvStoreUpdatesQueue.close();
        staticRouteUpdatesQueue.close();
        routeUpdatesQueue.close();
        peerUpdatesQueue.close();

        evb.stop();
      });

  // let magic happen
  evb.run();
  decision->stop();
  decisionThread->join();
}

/**
 * Test Decision ignores expired rib policy.
 *
 * Test covers
 * - Set policy
 * - Get policy after setting
 * - Wait long enough so Decision have saved Rib policy and policy expired
 * - Create a new Decision instance which will skip loading expired Rib policy
 * - Setup initial topology and prefixes to trigger route computation
 * - Verify that expired Rib policy is not applied on generated routes
 */
TEST_F(DecisionTestFixture, SaveReadStaleRibPolicy) {
  auto saveRibPolicyMaxMs =
      *config->getConfig().decision_config()->save_rib_policy_max_ms();

  // Get policy test. Expect failure
  EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);

  // Create rib policy
  thrift::RibRouteActionWeight actionWeight;
  actionWeight.neighbor_to_weight()->emplace("2", 2);
  thrift::RibPolicyStatement policyStatement;
  policyStatement.matcher()->prefixes() =
      std::vector<thrift::IpPrefix>({addr2});
  policyStatement.action()->set_weight() = actionWeight;
  thrift::RibPolicy policy;
  policy.statements()->emplace_back(policyStatement);
  policy.ttl_secs() = saveRibPolicyMaxMs / 1000;

  // Set rib policy
  EXPECT_NO_THROW(decision->setRibPolicy(policy).get());

  // Get rib policy and verify
  auto retrievedPolicy = decision->getRibPolicy().get();
  EXPECT_EQ(*policy.statements(), *retrievedPolicy.statements());
  EXPECT_GE(*policy.ttl_secs(), *retrievedPolicy.ttl_secs());

  std::unique_ptr<Decision> decision{nullptr};
  std::unique_ptr<std::thread> decisionThread{nullptr};

  int scheduleAt{0};
  OpenrEventBase evb;
  evb.scheduleTimeout(
      std::chrono::milliseconds(scheduleAt += 2 * saveRibPolicyMaxMs), [&]() {
        // Wait for 2 * saveRibPolicyMaxMs.
        // This makes sure expired rib policy is saved to file.
        messaging::ReplicateQueue<PeerEvent> peerUpdatesQueue;
        messaging::ReplicateQueue<KvStorePublication> kvStoreUpdatesQueue;
        messaging::ReplicateQueue<DecisionRouteUpdate> staticRouteUpdatesQueue;
        messaging::ReplicateQueue<DecisionRouteUpdate> routeUpdatesQueue;
        auto routeUpdatesQueueReader = routeUpdatesQueue.getReader();
        decision = std::make_unique<Decision>(
            config,
            peerUpdatesQueue.getReader(),
            kvStoreUpdatesQueue.getReader(),
            staticRouteUpdatesQueue.getReader(),
            routeUpdatesQueue);
        decisionThread = std::make_unique<std::thread>([&]() {
          LOG(INFO) << "Decision thread starting";
          decision->run();
          LOG(INFO) << "Decision thread finishing";
        });
        decision->waitUntilRunning();

        // Publish initial batch of detected peers.
        thrift::PeersMap peers;
        peers.emplace("2", thrift::PeerSpec());
        PeerEvent peerEvent{
            {kTestingAreaName, AreaPeerEvent(peers, {} /*peersToDel*/)}};
        peerUpdatesQueue.push(std::move(peerEvent));

        // Setup topology and prefixes. 1 unicast route will be computed
        auto publication = createThriftPublication(
            {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
             {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
             {"prefix:1", createPrefixValue("1", 1, {addr1})},
             {"prefix:2", createPrefixValue("2", 1, {addr2})}},
            {},
            {},
            {});
        kvStoreUpdatesQueue.push(publication);
        kvStoreUpdatesQueue.push(thrift::InitializationEvent::KVSTORE_SYNCED);

        // Expect route update without rib policy applied.
        auto maybeRouteDb = routeUpdatesQueueReader.get();
        EXPECT_FALSE(maybeRouteDb.hasError());
        auto updates = maybeRouteDb.value();
        ASSERT_EQ(1, updates.unicastRoutesToUpdate.size());
        EXPECT_EQ(
            0,
            *updates.unicastRoutesToUpdate.begin()
                 ->second.nexthops.begin()
                 ->weight());

        // Expired rib policy was not loaded.
        EXPECT_THROW(decision->getRibPolicy().get(), thrift::OpenrError);

        kvStoreUpdatesQueue.close();
        staticRouteUpdatesQueue.close();
        routeUpdatesQueue.close();
        peerUpdatesQueue.close();

        evb.stop();
      });

  // let magic happen
  evb.run();
  decision->stop();
  decisionThread->join();
}

// The following topology is used:
//
//         100
//  1--- ---------- 2
//   \_           _/
//      \_ ____ _/
//          800

// We upload parallel link 1---2 with the initial sync and later bring down
// the one with lower metric. We then verify updated route database is
// received
//

TEST_F(DecisionTestFixture, ParallelLinks) {
  auto adj12_1 =
      createAdjacency("2", "1/2-1", "2/1-1", "fe80::2", "192.168.0.2", 100, 0);
  auto adj12_2 =
      createAdjacency("2", "1/2-2", "2/1-2", "fe80::2", "192.168.0.2", 800, 0);
  auto adj21_1 =
      createAdjacency("1", "2/1-1", "1/2-1", "fe80::1", "192.168.0.1", 100, 0);
  auto adj21_2 =
      createAdjacency("1", "2/1-2", "1/2-2", "fe80::1", "192.168.0.1", 800, 0);

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12_1, adj12_2})},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21_1, adj21_2})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});
  auto routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  auto routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  auto routeDb = dumpRouteDb({"1"})["1"];
  auto routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 100)}));

  publication = createThriftPublication(
      {{"adj:2", createAdjValue(serializer, "2", 2, {adj21_2})}}, {}, {}, {});

  routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  routeDb = dumpRouteDb({"1"})["1"];
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_2, false, 800)}));

  // restore the original state
  publication = createThriftPublication(
      {{"adj:2", createAdjValue(serializer, "2", 2, {adj21_1, adj21_2})}},
      {},
      {},
      {});
  routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  routeDb = dumpRouteDb({"1"})["1"];
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_1, false, 100)}));

  // overload the least cost link
  auto adj21_1_overloaded = adj21_1;
  adj21_1_overloaded.isOverloaded() = true;

  publication = createThriftPublication(
      {{"adj:2",
        createAdjValue(serializer, "2", 2, {adj21_1_overloaded, adj21_2})}},
      {},
      {},
      {});
  routeDbBefore = dumpRouteDb({"1"})["1"];
  sendKvPublication(publication);
  // receive my local Decision routeDb publication
  routeDbDelta = recvRouteUpdates();
  EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
  routeDb = dumpRouteDb({"1"})["1"];
  routeDelta = findDeltaRoutes(routeDb, routeDbBefore);
  EXPECT_TRUE(checkEqualRoutesDelta(routeDbDelta, routeDelta));
  routeMap.clear();
  fillRouteMap("1", routeMap, routeDb);
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj12_2, false, 800)}));
}

// The following topology is used:
//
// 1---2---3---4
//
// We upload the link 1---2 with the initial sync and later publish
// the 2---3 & 3---4 link information. We expect it to trigger SPF only once.
//
TEST_F(DecisionTestFixture, PubDebouncing) {
  //
  // publish the link state info to KvStore
  //

  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12})},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);
  EXPECT_EQ(0, counters["decision.route_build_runs.count"]);

  sendKvPublication(publication);
  recvRouteUpdates();

  // validate SPF after initial sync, no rebouncing here
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(1, counters["decision.spf_runs.count"]);
  EXPECT_EQ(1, counters["decision.route_build_runs.count"]);

  //
  // publish the link state info to KvStore via the KvStore pub socket
  // we simulate adding a new router R3
  //

  // Some tricks here; we need to bump the time-stamp on router 2's data, so
  // it can override existing; for router 3 we publish new key-value
  publication = createThriftPublication(
      {{"adj:3", createAdjValue(serializer, "3", 1, {adj32})},
       {"adj:2", createAdjValue(serializer, "2", 3, {adj21, adj23})},
       createPrefixKeyValue("3", 1, addr3)},
      {},
      {},
      {});
  sendKvPublication(publication);

  // we simulate adding a new router R4

  // Some tricks here; we need to bump the time-stamp on router 3's data, so
  // it can override existing;

  publication = createThriftPublication(
      {{"adj:4", createAdjValue(serializer, "4", 1, {adj43})},
       {"adj:3", createAdjValue(serializer, "3", 5, {adj32, adj34})}},
      {},
      {},
      {});
  sendKvPublication(publication);
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(2, counters["decision.spf_runs.count"]);
  EXPECT_EQ(2, counters["decision.route_build_runs.count"]);

  //
  // Only publish prefix updates
  //
  auto getRouteForPrefixCount =
      counters.at("decision.get_route_for_prefix.count");
  publication = createThriftPublication(
      {createPrefixKeyValue("4", 1, addr4)}, {}, {}, {});
  sendKvPublication(publication);
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(2, counters["decision.spf_runs.count"]);
  // only prefix changed no full rebuild needed
  EXPECT_EQ(2, counters["decision.route_build_runs.count"]);

  EXPECT_EQ(
      getRouteForPrefixCount + 1,
      counters["decision.get_route_for_prefix.count"]);

  //
  // publish adj updates right after prefix updates
  // Decision is supposed to only trigger spf recalculation

  // Some tricks here; we need to bump the time-stamp on router 4's data, so
  // it can override existing;
  publication = createThriftPublication(
      {createPrefixKeyValue("4", 2, addr4),
       createPrefixKeyValue("4", 2, addr5)},
      {},
      {},
      {});
  sendKvPublication(publication);

  publication = createThriftPublication(
      {{"adj:2", createAdjValue(serializer, "2", 5, {adj21})}}, {}, {}, {});
  sendKvPublication(publication);
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  EXPECT_EQ(3, counters["decision.spf_runs.count"]);
  EXPECT_EQ(3, counters["decision.route_build_runs.count"]);

  //
  // publish multiple prefix updates in a row
  // Decision is supposed to process prefix update only once

  // Some tricks here; we need to bump the version on router 4's data, so
  // it can override existing;

  getRouteForPrefixCount = counters.at("decision.get_route_for_prefix.count");
  publication = createThriftPublication(
      {createPrefixKeyValue("4", 5, addr4)}, {}, {}, {});
  sendKvPublication(publication);

  publication = createThriftPublication(
      {createPrefixKeyValue("4", 7, addr4),
       createPrefixKeyValue("4", 7, addr6)},
      {},
      {},
      {});
  sendKvPublication(publication);

  publication = createThriftPublication(
      {createPrefixKeyValue("4", 8, addr4),
       createPrefixKeyValue("4", 8, addr5),
       createPrefixKeyValue("4", 8, addr6)},
      {},
      {},
      {});
  sendKvPublication(publication);
  recvRouteUpdates();

  counters = fb303::fbData->getCounters();
  // only prefix has changed so spf_runs is unchanged
  EXPECT_EQ(3, counters["decision.spf_runs.count"]);
  // addr6 is seen to have been advertised in this  interval
  EXPECT_EQ(
      getRouteForPrefixCount + 1,
      counters["decision.get_route_for_prefix.count"]);
}

//
// Send unrelated key-value pairs to Decision
// Make sure they do not trigger SPF runs, but rather ignored
//
TEST_F(DecisionTestFixture, NoSpfOnIrrelevantPublication) {
  //
  // publish the link state info to KvStore, but use different markers
  // those must be ignored by the decision module
  //
  auto publication = createThriftPublication(
      {{"adj2:1", createAdjValue(serializer, "1", 1, {adj12})},
       {"adji2:2", createAdjValue(serializer, "2", 1, {adj21})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(3 * debounceTimeoutMax);

  // make sure the counter did not increment
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);
}

//
// Send duplicate key-value pairs to Decision
// Make sure subsquent duplicates are ignored.
//
TEST_F(DecisionTestFixture, NoSpfOnDuplicatePublication) {
  //
  // publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //
  auto const publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12})},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(0, counters["decision.spf_runs.count"]);

  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(3 * debounceTimeoutMax);

  // make sure counter is incremented
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(1, counters["decision.spf_runs.count"]);

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);

  // wait for SPF to finish
  /* sleep override */
  std::this_thread::sleep_for(3 * debounceTimeoutMax);

  // make sure counter is not incremented
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(1, counters["decision.spf_runs.count"]);
}

/**
 * Test to verify route calculation when a prefix is advertised from more than
 * one node.
 *
 *
 *  node4(p4)
 *     |
 *   5 |
 *     |         10
 *  node1(p1) --------- node2(p2)
 *     |
 *     | 10
 *     |
 *  node3(p2)
 */
TEST_F(DecisionTestFixture, DuplicatePrefixes) {
  // Note: local copy overwriting global ones, to be changed in this test
  auto adj14 =
      createAdjacency("4", "1/4", "4/1", "fe80::4", "192.168.0.4", 5, 0);
  auto adj41 =
      createAdjacency("1", "4/1", "1/4", "fe80::1", "192.168.0.1", 5, 0);
  auto adj12 =
      createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 0);
  auto adj21 =
      createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 0);

  //
  // publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj14, adj12, adj13})},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21})},
       {"adj:3", createAdjValue(serializer, "3", 1, {adj31})},
       {"adj:4", createAdjValue(serializer, "4", 1, {adj41})},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2),
       // node3 has same address w/ node2
       createPrefixKeyValue("3", 1, addr2),
       createPrefixKeyValue("4", 1, addr4)},
      {},
      {},
      {});

  sendKvPublication(publication);
  recvRouteUpdates();

  // Expect best route selection to be populated in route-details for addr2
  {
    thrift::ReceivedRouteFilter filter;
    filter.prefixes() = std::vector<thrift::IpPrefix>({addr2});
    auto routes = decision->getReceivedRoutesFiltered(filter).get();
    ASSERT_EQ(1, routes->size());

    auto const& routeDetails = routes->at(0);
    EXPECT_EQ(2, routeDetails.bestKeys()->size());
    EXPECT_EQ("2", routeDetails.bestKey()->node().value());
  }

  // Query new information
  // validate routers
  auto routeMapList = dumpRouteDb({"1", "2", "3", "4"});
  EXPECT_EQ(4, routeMapList.size()); // 1 route per neighbor
  RouteMap routeMap;
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap, value);
  }

  // 1
  EXPECT_EQ(2, routeMapList["1"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops(
          {createNextHopFromAdj(adj12, false, 10),
           createNextHopFromAdj(adj13, false, 10)}));

  // 2
  EXPECT_EQ(2, routeMapList["2"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 10)}));

  // 3
  EXPECT_EQ(2, routeMapList["3"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  // 4
  EXPECT_EQ(2, routeMapList["4"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj41, false, 15)}));

  /**
   * Overload node-2 and node-4. Now we on node-1 will only route p2 toward
   * node-3 but will still have route p4 toward node-4 since it's unicast
   *
   *  node4(p4)
   *     |
   *   5 |
   *     |         10     (overloaded)
   *  node1(p1) --------- node2(p2)
   *     |
   *     | 10
   *     |
   *  node3(p2)
   */

  publication = createThriftPublication(
      {{"adj:2",
        createAdjValue(serializer, "2", 1, {adj21}, true /* overloaded */)},
       {"adj:4",
        createAdjValue(serializer, "4", 1, {adj41}, true /* overloaded */)}},
      {},
      {},
      {});

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);
  recvRouteUpdates();

  routeMapList = dumpRouteDb({"1"});
  RouteMap routeMap2;
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap2, value);
  }
  EXPECT_EQ(
      routeMap2[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj13, false, 10)}));

  EXPECT_EQ(
      routeMap2[make_pair("1", toString(addr4))],
      NextHops({createNextHopFromAdj(adj14, false, 5)}));

  /**
   * Increase the distance between node-1 and node-2 to 100. Now we on node-1
   * will reflect weights into nexthops and FIB will not do multipath
   *
   *  node4(p4)
   *     |
   *   5 |
   *     |         100
   *  node1(p1) --------- node2(p2)
   *     |
   *     | 10
   *     |
   *  node3(p2)
   */
  adj12.metric() = 100;
  adj21.metric() = 100;

  publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 2, {adj12, adj13, adj14})},
       {"adj:2", createAdjValue(serializer, "2", 2, {adj21, adj23})}},
      {},
      {},
      {});

  // Send same publication again to Decision using pub socket
  sendKvPublication(publication);
  recvRouteUpdates();

  // Query new information
  // validate routers
  routeMapList = dumpRouteDb({"1", "2", "3", "4"});
  EXPECT_EQ(4, routeMapList.size()); // 1 route per neighbor
  routeMap.clear();
  for (auto& [key, value] : routeMapList) {
    fillRouteMap(key, routeMap, value);
  }

  // 1
  EXPECT_EQ(2, routeMapList["1"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2))],
      NextHops({createNextHopFromAdj(adj13, false, 10)}));

  // 2
  EXPECT_EQ(2, routeMapList["2"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1))],
      NextHops({createNextHopFromAdj(adj21, false, 100)}));

  // 3
  EXPECT_EQ(2, routeMapList["3"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1))],
      NextHops({createNextHopFromAdj(adj31, false, 10)}));

  // 4
  EXPECT_EQ(2, routeMapList["4"].unicastRoutes()->size());
  EXPECT_EQ(
      routeMap[make_pair("4", toString(addr2))],
      NextHops({createNextHopFromAdj(adj41, false, 15)}));
}

/**
 * Tests reliability of Decision SUB socket. We overload SUB socket with lot
 * of messages and make sure none of them are lost. We make decision compute
 * routes for a large network topology taking good amount of CPU time. We
 * do not try to validate routes here instead we validate messages processed
 * by decision and message sent by us.
 *
 * Topology consists of 1000 nodes linear where node-i connects to 3 nodes
 * before it and 3 nodes after it.
 *
 */
TEST_F(DecisionTestFixture, DecisionSubReliability) {
  thrift::Publication initialPub;
  initialPub.area() = kTestingAreaName;

  std::string keyToDup;

  // Create full topology
  for (int i = 1; i <= 1000; i++) {
    const std::string src = folly::to<std::string>(i);

    // Create prefixDb value
    const auto addr = toIpPrefix(fmt::format("face:cafe:babe::{}/128", i));
    auto kv = createPrefixKeyValue(src, 1, addr);
    if (1 == i) {
      // arbitrarily choose the first key to send duplicate publications for
      keyToDup = kv.first;
    }
    initialPub.keyVals()->emplace(kv);

    // Create adjDb value
    vector<thrift::Adjacency> adjs;
    for (int j = std::max(1, i - 3); j <= std::min(1000, i + 3); j++) {
      if (i == j) {
        continue;
      }
      const std::string dst = folly::to<std::string>(j);
      auto adj = createAdjacency(
          dst,
          fmt::format("{}/{}", src, dst),
          fmt::format("{}/{}", dst, src),
          fmt::format("fe80::{}", dst),
          "192.168.0.1" /* unused */,
          10 /* metric */,
          0 /* adj label */);
      adjs.emplace_back(std::move(adj));
    }
    initialPub.keyVals()->emplace(
        fmt::format("adj:{}", src), createAdjValue(serializer, src, 1, adjs));
  }

  //
  // publish initial link state info to KvStore, This should trigger the
  // SPF run.
  //
  sendKvPublication(initialPub);

  //
  // Hammer Decision with lot of duplicate publication for 2 * ThrottleTimeout
  // We want to ensure that we hammer Decision for atleast once during it's
  // SPF run. This will cause lot of pending publications on Decision. This
  // is not going to cause any SPF computation
  //
  thrift::Publication duplicatePub;
  duplicatePub.area() = kTestingAreaName;
  duplicatePub.keyVals()[keyToDup] = initialPub.keyVals()->at(keyToDup);
  int64_t totalSent = 0;
  auto start = std::chrono::steady_clock::now();
  while (true) {
    auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    if (diff > (2 * debounceTimeoutMax)) {
      LOG(INFO) << "Hammered decision with " << totalSent
                << " updates. Stopping";
      break;
    }
    ++totalSent;
    sendKvPublication(duplicatePub);
  }

  // Receive RouteUpdate from Decision
  auto routeUpdates1 = recvRouteUpdates();
  // Route to all nodes except mine.
  EXPECT_EQ(999, routeUpdates1.unicastRoutesToUpdate.size());

  //
  // Advertise prefix update. Decision gonna take some good amount of time to
  // process this last update (as it has many queued updates).
  //
  thrift::Publication newPub;
  newPub.area() = kTestingAreaName;

  auto newAddr = toIpPrefix("face:b00c:babe::1/128");
  newPub.keyVals() = {createPrefixKeyValue("1", 1, newAddr)};
  LOG(INFO) << "Advertising prefix update";
  sendKvPublication(newPub);
  // Receive RouteDelta from Decision
  auto routeUpdates2 = recvRouteUpdates();
  // Expect no routes delta
  EXPECT_EQ(0, routeUpdates2.unicastRoutesToUpdate.size());

  //
  // Verify counters information
  //

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(1, counters["decision.spf_runs.count"]);
}

//
// This test aims to verify counter reporting from Decision module
//
TEST_F(DecisionTestFixture, Counters) {
  // Verifiy some initial/default counters
  {
    decision->updateGlobalCounters();
    const auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(counters.at("decision.num_nodes"), 1);
  }

  // set up first publication

  // Node1 and Node2 has both v4/v6 loopbacks, Node3 has only V6
  auto bgpPrefixEntry1 = createPrefixEntry( // Missing loopback
      toIpPrefix("10.2.0.0/16"),
      thrift::PrefixType::BGP,
      "data=10.2.0.0/16",
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP);
  auto bgpPrefixEntry2 = createPrefixEntry( // Missing metric vector
      toIpPrefix("10.3.0.0/16"),
      thrift::PrefixType::BGP,
      "data=10.3.0.0/16",
      thrift::PrefixForwardingType::IP,
      thrift::PrefixForwardingAlgorithm::SP_ECMP,
      std::nullopt /* missing metric vector */);
  auto bgpPrefixEntry3 = createPrefixEntry( // Conflicting forwarding type
      toIpPrefix("10.3.0.0/16"),
      thrift::PrefixType::BGP,
      "data=10.3.0.0/16",
      thrift::PrefixForwardingType::SR_MPLS,
      thrift::PrefixForwardingAlgorithm::SP_ECMP);
  thrift::KeyVals pubKvs = {
      {"adj:1", createAdjValue(serializer, "1", 1, {adj12, adj13}, false, 1)},
      {"adj:2", createAdjValue(serializer, "2", 1, {adj21, adj23}, false, 2)},
      {"adj:3",
       createAdjValue(serializer, "3", 1, {adj31}, false, 3 << 20)}, // invalid
                                                                     // mpls
                                                                     // label
      {"adj:4",
       createAdjValue(serializer, "4", 1, {}, false, 4)} // Disconnected
                                                         // node
  };
  pubKvs.emplace(createPrefixKeyValue("1", 1, addr1));
  pubKvs.emplace(createPrefixKeyValue("1", 1, addr1V4));

  pubKvs.emplace(createPrefixKeyValue("2", 1, addr2));
  pubKvs.emplace(createPrefixKeyValue("2", 1, addr2V4));

  pubKvs.emplace(createPrefixKeyValue("3", 1, addr3));
  pubKvs.emplace(createPrefixKeyValue("3", 1, bgpPrefixEntry1));
  pubKvs.emplace(createPrefixKeyValue("3", 1, bgpPrefixEntry3));

  pubKvs.emplace(createPrefixKeyValue("4", 1, addr4));
  pubKvs.emplace(createPrefixKeyValue("4", 1, bgpPrefixEntry2));

  // Node1 connects to 2/3, Node2 connects to 1, Node3 connects to 1
  // Node2 has partial adjacency
  auto publication0 = createThriftPublication(pubKvs, {}, {}, {});
  sendKvPublication(publication0);
  const auto routeDb = recvRouteUpdates();
  for (const auto& [_, uniRoute] : routeDb.unicastRoutesToUpdate) {
    EXPECT_NE(
        folly::IPAddress::networkToString(uniRoute.prefix), "10.1.0.0/16");
  }

  // Verify counters
  decision->updateGlobalCounters();
  const auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters.at("decision.num_partial_adjacencies"), 1);
  EXPECT_EQ(counters.at("decision.num_complete_adjacencies"), 2);
  EXPECT_EQ(counters.at("decision.num_nodes"), 4);
  EXPECT_EQ(counters.at("decision.num_prefixes"), 8);
  EXPECT_EQ(counters.at("decision.no_route_to_prefix.count.60"), 1);
  EXPECT_EQ(counters.at("decision.skipped_mpls_route.count.60"), 1);
  EXPECT_EQ(counters.at("decision.no_route_to_label.count.60"), 1);

  // fully disconnect node 2
  auto publication1 = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 2, {adj13}, false, 1)}},
      {},
      {},
      {});
  sendKvPublication(publication1);
  // wait for update
  recvRouteUpdates();

  decision->updateGlobalCounters();
  EXPECT_EQ(
      fb303::fbData->getCounters().at("decision.num_partial_adjacencies"), 0);
}

TEST_F(DecisionTestFixture, ExceedMaxBackoff) {
  for (int i = debounceTimeoutMin.count(); true; i *= 2) {
    auto nodeName = std::to_string(i);
    auto publication = createThriftPublication(
        {createPrefixKeyValue(nodeName, 1, addr1)}, {}, {}, {});
    sendKvPublication(publication);
    if (i >= debounceTimeoutMax.count()) {
      break;
    }
  }

  // wait for debouncer to try to fire
  /* sleep override */
  std::this_thread::sleep_for(
      debounceTimeoutMax + std::chrono::milliseconds(100));
  // send one more update
  auto publication = createThriftPublication(
      {createPrefixKeyValue("2", 1, addr1)}, {}, {}, {});
  sendKvPublication(publication);
}

//
// Mixed type prefix announcements (e.g. prefix1 with type BGP and type RIB )
// are allowed when enableBestRouteSelection_ = true,
// Otherwise prefix will be skipped in route programming.
//
TEST_F(DecisionTestFixture, PrefixWithMixedTypeRoutes) {
  // Verifiy some initial/default counters
  {
    decision->updateGlobalCounters();
    const auto counters = fb303::fbData->getCounters();
    EXPECT_EQ(counters.at("decision.num_nodes"), 1);
  }

  // set up first publication

  // node 2/3 announce loopbacks
  {
    const auto prefixDb2 = createPrefixDb(
        "2", {createPrefixEntry(addr2), createPrefixEntry(addr2V4)});
    const auto prefixDb3 = createPrefixDb(
        "3", {createPrefixEntry(addr3), createPrefixEntry(addr3V4)});

    // Node1 connects to 2/3, Node2 connects to 1, Node3 connects to 1
    auto publication = createThriftPublication(
        {{"adj:1",
          createAdjValue(serializer, "1", 1, {adj12, adj13}, false, 1)},
         {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
         {"adj:3", createAdjValue(serializer, "3", 1, {adj31}, false, 3)},
         createPrefixKeyValue("2", 1, addr2),
         createPrefixKeyValue("2", 1, addr2V4),
         createPrefixKeyValue("3", 1, addr3),
         createPrefixKeyValue("3", 1, addr3V4)},
        {},
        {},
        {});
    sendKvPublication(publication);
    recvRouteUpdates();
  }

  // Node2 annouce prefix in BGP type,
  // Node3 announce prefix in Rib type
  {
    auto bgpPrefixEntry = createPrefixEntry(
        toIpPrefix("10.1.0.0/16"),
        thrift::PrefixType::BGP,
        "data=10.1.0.0/16",
        thrift::PrefixForwardingType::IP,
        thrift::PrefixForwardingAlgorithm::SP_ECMP);
    auto ribPrefixEntry = createPrefixEntry(
        toIpPrefix("10.1.0.0/16"),
        thrift::PrefixType::RIB,
        "",
        thrift::PrefixForwardingType::IP,
        thrift::PrefixForwardingAlgorithm::SP_ECMP);

    auto publication = createThriftPublication(
        // node 2 announce BGP prefix with loopback
        {createPrefixKeyValue("2", 1, bgpPrefixEntry),
         createPrefixKeyValue("3", 1, ribPrefixEntry)},
        {},
        {},
        {});
    sendKvPublication(publication);
    recvRouteUpdates();
  }
}

/**
 * Test fixture for testing initial RIB computation in OpenR initialization
 * process.
 */
class InitialRibBuildTestFixture : public DecisionTestFixture {
  openr::thrift::OpenrConfig
  createConfig() override {
    auto tConfig = DecisionTestFixture::createConfig();

    // Set config originated prefixes.
    thrift::OriginatedPrefix originatedPrefixV4;
    originatedPrefixV4.prefix() = toString(addr1V4);
    originatedPrefixV4.minimum_supporting_routes() = 0;
    originatedPrefixV4.install_to_fib() = true;
    tConfig.originated_prefixes() = {originatedPrefixV4};

    // Enable Vip service.
    tConfig.enable_vip_service() = true;
    tConfig.vip_service_config() = vipconfig::config::VipServiceConfig();

    return tConfig;
  }

  void
  publishInitialPeers() override {
    // Do not publish peers information. Test case below will handle that.
  }
};

/*
 * Verify OpenR initialzation could succeed at current node (1),
 * - Receives adjacencies 1->2 (only used by 2) and 2->1 (only used by 1)
 * - Receives initial up peers 2 and 3 (Decision needs to wait for adjacencies
 *   with both peers).
 * - Receives CONFIG type static routes.
 * - Receives BGP or VIP type static routes
 * - Receives peer down event for node 3 (Decision does not need to wait for
 *   adjacencies with peer 3 anymore).
 * - Initial route computation is triggered, generating static routes.
 * - Receives updated adjacency 1->2 (can be used by anyone). Node 1 and 2 get
 *   connected, thus computed routes for prefixes advertised by node 2 and
 *   label route of node 2.
 */
TEST_F(InitialRibBuildTestFixture, PrefixWithVipRoutes) {
  // Send adj publication (current node is 1).
  // * adjacency "1->2" can only be used by node 2,
  // * adjacency "2->1" can only be used by node 1.
  // Link 1<->2 is not up since "1->2" cannot be used by node 1.
  // However, the two adjacencies will unblock
  sendKvPublication(
      createThriftPublication(
          {{"adj:1",
            createAdjValue(serializer, "1", 1, {adj12OnlyUsedBy2}, false, 1)},
           {"adj:2",
            createAdjValue(serializer, "2", 1, {adj21OnlyUsedBy1}, false, 2)}},
          {},
          {},
          {}),
      false /*prefixPubExists*/);

  int scheduleAt{0};
  OpenrEventBase evb;
  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // KvStore publication is not process yet since initial peers are not
        // received.
        auto adjDb = decision->getDecisionAdjacenciesFiltered().get();
        ASSERT_EQ(adjDb->size(), 0);

        // Add initial UP peers "2" and "3".
        // Initial RIB computation will be blocked until dual directional
        // adjacencies are received for both peers.
        thrift::PeersMap peers;
        peers.emplace("2", thrift::PeerSpec());
        peers.emplace("3", thrift::PeerSpec());
        PeerEvent peerEvent{
            {kTestingAreaName, AreaPeerEvent(peers, {} /*peersToDel*/)}};
        peerUpdatesQueue.push(std::move(peerEvent));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // KvStore publication is processed and adjacence is extracted.
        auto adjDb = decision->getDecisionAdjacenciesFiltered().get();
        ASSERT_NE(adjDb->size(), 0);

        // Received KvStoreSynced signal.
        auto publication = createThriftPublication(
            /* prefix key format v2 */
            {createPrefixKeyValue("2", 1, addr1, kTestingAreaName, false)},
            /* expired-keys */
            {});
        sendKvPublication(publication);
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Initial RIB computation not triggered yet.
        EXPECT_EQ(0, routeUpdatesQueueReader.size());

        // Received static unicast routes for config originated prefixes.
        DecisionRouteUpdate configStaticRoutes;
        configStaticRoutes.prefixType = thrift::PrefixType::CONFIG;

        configStaticRoutes.addRouteToUpdate(RibUnicastEntry(
            toIPNetwork(addr1V4),
            {},
            addr1V4ConfigPrefixEntry,
            Constants::kDefaultArea.toString()));
        staticRouteUpdatesQueue.push(std::move(configStaticRoutes));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Initial RIB computation not triggered yet.
        EXPECT_EQ(0, routeUpdatesQueueReader.size());

        // Received static unicast routes for VIP prefixes.
        DecisionRouteUpdate vipStaticRoutes;
        vipStaticRoutes.prefixType = thrift::PrefixType::VIP;
        vipStaticRoutes.addRouteToUpdate(RibUnicastEntry(
            toIPNetwork(addr2V4),
            {},
            addr2VipPrefixEntry,
            Constants::kDefaultArea.toString()));
        staticRouteUpdatesQueue.push(std::move(vipStaticRoutes));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Initial RIB computation not triggered yet.
        EXPECT_EQ(0, routeUpdatesQueueReader.size());

        // Initial UP peer "3" goes down. Open/R initialization does not wait
        // for adjacency with the peer.
        PeerEvent newPeerEvent{
            {kTestingAreaName, AreaPeerEvent({} /*peersToAdd*/, {"3"})}};
        peerUpdatesQueue.push(std::move(newPeerEvent));
      });

  evb.scheduleTimeout(
      std::chrono::milliseconds(
          scheduleAt += 2 * Constants::kKvStoreSyncThrottleTimeout.count()),
      [&]() {
        // Initial RIB computation is triggered.
        // Generated static routes and node label route for node 1.
        auto routeDbDelta = recvRouteUpdates();

        // Static config originated route and static VIP route.
        EXPECT_EQ(2, routeDbDelta.unicastRoutesToUpdate.size());
        // Node label routes for the node itself (1).
        EXPECT_EQ(1, routeDbDelta.mplsRoutesToUpdate.size());
        EXPECT_EQ(1, routeDbDelta.mplsRoutesToUpdate.count(1));

        // Send adj publication.
        // Updated adjacency for peer "2" is received,
        // * adjacency "1->2" can be used by all nodes.
        sendKvPublication(createThriftPublication(
            {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)}},
            {},
            {},
            {}));

        routeDbDelta = recvRouteUpdates();
        // Unicast route for addr1 advertised by node 2.
        EXPECT_EQ(1, routeDbDelta.unicastRoutesToUpdate.size());
        EXPECT_EQ(
            routeDbDelta.unicastRoutesToUpdate.begin()->second.prefix,
            toIPNetwork(addr1));
        // Node label route for node 2.
        EXPECT_EQ(1, routeDbDelta.mplsRoutesToUpdate.size());
        EXPECT_EQ(1, routeDbDelta.mplsRoutesToUpdate.count(2));

        evb.stop();
      });
  // let magic happen
  evb.run();
}

/**
 * Test fixture for testing Decision module with V4 over V6 nexthop feature.
 */
class DecisionV4OverV6NexthopTestFixture : public DecisionTestFixture {
 protected:
  /**
   * The only differences between this test fixture and the DecisionTetFixture
   * is the config where here we enable V4OverV6Nexthop
   */
  openr::thrift::OpenrConfig
  createConfig() override {
    tConfig_ = getBasicOpenrConfig(
        "1", /* nodeName */
        {}, /* areaCfg */
        true, /* enable v4 */
        true, /* enableSegmentRouting */
        false, /* dryrun */
        true /* enableV4OverV6Nexthop */);
    return tConfig_;
  }

  openr::thrift::OpenrConfig tConfig_;
};

/**
 * Similar as the BasicalOperations, we test the Decision module with the
 * v4_over_v6_nexthop feature enabled.
 *
 * We are using the topology: 1---2---3
 *
 * We upload the link 1--2 with initial sync and later publish the 2---3 link
 * information. We check the nexthop from full routing dump and other fields.
 */
TEST_F(DecisionV4OverV6NexthopTestFixture, BasicOperationsV4OverV6Nexthop) {
  // First make sure the v4 over v6 nexthop is enabled
  EXPECT_TRUE(*tConfig_.v4_over_v6_nexthop());

  // public the link state info to KvStore
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       createPrefixKeyValue("1", 1, addr1V4),
       createPrefixKeyValue("2", 1, addr2V4)},
      {},
      {},
      {});

  sendKvPublication(publication);
  auto routeDbDelta = recvRouteUpdates();

  auto routeDb = dumpRouteDb({"1"})["1"];

  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj12,
          true /*isV4*/,
          10,
          std::nullopt,
          kTestingAreaName,
          true /*v4OverV6Nexthop*/)}));

  // for router 3 we publish new key-value
  publication = createThriftPublication(
      {{"adj:3", createAdjValue(serializer, "3", 1, {adj32}, false, 3)},
       {"adj:2", createAdjValue(serializer, "2", 2, {adj21, adj23}, false, 2)},
       createPrefixKeyValue("3", 1, addr3V4)},
      {},
      {},
      {});

  sendKvPublication(publication);

  routeDb = dumpRouteDb({"1"})["1"];
  fillRouteMap("1", routeMap, routeDb);

  // nexthop checking for node 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj12, true, 10, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3V4))],
      NextHops({createNextHopFromAdj(
          adj12, true, 20, std::nullopt, kTestingAreaName, true)}));

  auto routeDbMap = dumpRouteDb({"2", "3"});
  for (auto& [key, value] : routeDbMap) {
    fillRouteMap(key, routeMap, value);
  }

  // nexthop checking for node 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1V4))],
      NextHops({createNextHopFromAdj(
          adj21, true, 10, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3V4))],
      NextHops({createNextHopFromAdj(
          adj23, true, 10, std::nullopt, kTestingAreaName, true)}));

  // nexthop checking for node 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1V4))],
      NextHops({createNextHopFromAdj(
          adj32, true, 20, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj32, true, 10, std::nullopt, kTestingAreaName, true)}));
}

/**
 * Test fixture for testing Decision module with V4 over V6 nexthop feature.
 */
class DecisionV4OverV6NexthopWithNoV4TestFixture : public DecisionTestFixture {
 protected:
  /**
   * The only differences between this test fixture and the DecisionTetFixture
   * is the config where here we enable V4OverV6Nexthop
   */
  openr::thrift::OpenrConfig
  createConfig() override {
    tConfig_ = getBasicOpenrConfig(
        "1", /* nodeName */
        {}, /* areaCfg */
        false, /* enable v4 */
        true, /* enableSegmentRouting */
        false, /* dryrun */
        true /* enableV4OverV6Nexthop */);
    return tConfig_;
  }

  openr::thrift::OpenrConfig tConfig_;
};

/**
 * Similar as the BasicalOperations, we test the Decision module with the
 * v4_over_v6_nexthop feature enabled.
 *
 * We are using the topology: 1---2---3
 *
 * We upload the link 1--2 with initial sync and later publish the 2---3 link
 * information. We check the nexthop from full routing dump and other fields.
 */
TEST_F(
    DecisionV4OverV6NexthopWithNoV4TestFixture,
    BasicOperationsV4OverV6NexthopWithNoV4Interface) {
  // First make sure the v4 over v6 nexthop is enabled
  EXPECT_TRUE(*tConfig_.v4_over_v6_nexthop());

  // public the link state info to KvStore
  auto publication = createThriftPublication(
      {{"adj:1", createAdjValue(serializer, "1", 1, {adj12}, false, 1)},
       {"adj:2", createAdjValue(serializer, "2", 1, {adj21}, false, 2)},
       createPrefixKeyValue("1", 1, addr1V4),
       createPrefixKeyValue("2", 1, addr2V4)},
      {},
      {},
      {});

  sendKvPublication(publication);
  auto routeDbDelta = recvRouteUpdates();

  auto routeDb = dumpRouteDb({"1"})["1"];

  RouteMap routeMap;
  fillRouteMap("1", routeMap, routeDb);

  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj12,
          true /*isV4*/,
          10,
          std::nullopt,
          kTestingAreaName,
          true /*v4OverV6Nexthop*/)}));

  // for router 3 we publish new key-value
  publication = createThriftPublication(
      {{"adj:3", createAdjValue(serializer, "3", 1, {adj32}, false, 3)},
       {"adj:2", createAdjValue(serializer, "2", 2, {adj21, adj23}, false, 2)},
       createPrefixKeyValue("3", 1, addr3V4)},
      {},
      {},
      {});

  sendKvPublication(publication);

  routeDb = dumpRouteDb({"1"})["1"];
  fillRouteMap("1", routeMap, routeDb);

  // nexthop checking for node 1
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj12, true, 10, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("1", toString(addr3V4))],
      NextHops({createNextHopFromAdj(
          adj12, true, 20, std::nullopt, kTestingAreaName, true)}));

  auto routeDbMap = dumpRouteDb({"2", "3"});
  for (auto& [key, value] : routeDbMap) {
    fillRouteMap(key, routeMap, value);
  }

  // nexthop checking for node 2
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr1V4))],
      NextHops({createNextHopFromAdj(
          adj21, true, 10, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("2", toString(addr3V4))],
      NextHops({createNextHopFromAdj(
          adj23, true, 10, std::nullopt, kTestingAreaName, true)}));

  // nexthop checking for node 3
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr1V4))],
      NextHops({createNextHopFromAdj(
          adj32, true, 20, std::nullopt, kTestingAreaName, true)}));
  EXPECT_EQ(
      routeMap[make_pair("3", toString(addr2V4))],
      NextHops({createNextHopFromAdj(
          adj32, true, 10, std::nullopt, kTestingAreaName, true)}));
}

TEST(DecisionPendingUpdates, needsFullRebuild) {
  openr::detail::DecisionPendingUpdates updates("node1");
  LinkState::LinkStateChange linkStateChange;

  linkStateChange.linkAttributesChanged = true;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  updates.applyLinkStateChange("node1", linkStateChange, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_TRUE(updates.needsFullRebuild());

  updates.reset();
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  linkStateChange.linkAttributesChanged = false;
  linkStateChange.topologyChanged = true;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_TRUE(updates.needsFullRebuild());

  updates.reset();
  linkStateChange.topologyChanged = false;
  linkStateChange.nodeLabelChanged = true;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_TRUE(updates.needsFullRebuild());
}

TEST(DecisionPendingUpdates, updatedPrefixes) {
  openr::detail::DecisionPendingUpdates updates("node1");

  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_TRUE(updates.updatedPrefixes().empty());

  // empty update no change
  updates.applyPrefixStateChange({}, kEmptyPerfEventRef);
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_TRUE(updates.updatedPrefixes().empty());

  updates.applyPrefixStateChange(
      {addr1Cidr, toIPNetwork(addr2V4)}, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_THAT(
      updates.updatedPrefixes(),
      testing::UnorderedElementsAre(addr1Cidr, addr2V4Cidr));
  updates.applyPrefixStateChange({addr2Cidr}, kEmptyPerfEventRef);
  EXPECT_TRUE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_THAT(
      updates.updatedPrefixes(),
      testing::UnorderedElementsAre(addr1Cidr, addr2V4Cidr, addr2Cidr));

  updates.reset();
  EXPECT_FALSE(updates.needsRouteUpdate());
  EXPECT_FALSE(updates.needsFullRebuild());
  EXPECT_TRUE(updates.updatedPrefixes().empty());
}

/*
 * @brief  Verify that we report counters of link event propagation time
 *         correctly. It is verified in the context of updateAdjacencyDatabase
 */
TEST_F(DecisionTestFixture, LinkEventPropagationTime) {
  auto now = getUnixTimeStampMs();
  std::string nodeName("1");
  auto linkState = LinkState(kTestingAreaName, nodeName);
  fb303::fbData->resetAllData();

  auto adj1 =
      createAdjacency("2", "1/2", "2/1", "fe80::2", "192.168.0.2", 10, 100002);
  auto adjDb1 = createAdjDb("1", {adj1}, 1);
  linkState.updateAdjacencyDatabase(adjDb1, kTestingAreaName);

  /*
   * Up link event during initialization
   * Propagation time reporting is skipped during initialization
   */
  auto adj2 =
      createAdjacency("1", "2/1", "1/2", "fe80::1", "192.168.0.1", 10, 100001);
  auto adjDb2 = createAdjDb("2", {adj2}, 2);
  thrift::LinkStatusRecords rec1;
  rec1.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::UP;
  rec1.linkStatusMap()["2/1"].unixTs() = now - 10;
  adjDb2.linkStatusRecords() = rec1;
  linkState.updateAdjacencyDatabase(adjDb2, kTestingAreaName, true);

  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters["decision.linkstate.up.propagation_time_ms.avg.60"], 0);

  /*
   * Down link event post initialization
   * Propagation time reporting is to occur from here onwards
   */
  adjDb2 = createAdjDb("2", {}, 2);
  rec1.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::DOWN;
  rec1.linkStatusMap()["2/1"].unixTs() = now - 100;
  adjDb2.linkStatusRecords() = rec1;
  linkState.updateAdjacencyDatabase(adjDb2, kTestingAreaName);

  counters = fb303::fbData->getCounters();
  EXPECT_GE(
      counters["decision.linkstate.down.propagation_time_ms.avg.60"], 100);

  // Down link event with timestamp is not updated, then it's skipped
  fb303::fbData->resetAllData();
  adjDb2 = createAdjDb("2", {}, 2);
  rec1.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::DOWN;
  rec1.linkStatusMap()["2/1"].unixTs() = 0;
  adjDb2.linkStatusRecords() = rec1;
  linkState.updateAdjacencyDatabase(adjDb2, kTestingAreaName);
  counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters["decision.linkstate.down.propagation_time_ms.avg.60"], 0);
}

/*
 * @brief  Verify that we report counters of link event propagation time
 *         correctly. It is verified in the context of decision module,
 *         around use of ADJACENCY_DB_SYNCED signal
 */
TEST_F(DecisionTestFixture, LinkPropagationWithBasicOperations) {
  auto now = getUnixTimeStampMs();
  fb303::fbData->resetAllData();

  thrift::LinkStatusRecords lsRec1, lsRec2;
  lsRec1.linkStatusMap()["1/2"].status() = thrift::LinkStatusEnum::UP;
  lsRec1.linkStatusMap()["1/2"].unixTs() = now - 10;
  lsRec2.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::UP;
  lsRec2.linkStatusMap()["2/1"].unixTs() = now - 10;

  auto publication = createThriftPublication(
      {{"adj:1",
        createAdjValueWithLinkStatus(
            serializer, "1", 1, {adj12}, lsRec1, false, 1)},
       {"adj:2",
        createAdjValueWithLinkStatus(
            serializer, "2", 1, {adj21}, lsRec2, false, 2)},
       createPrefixKeyValue("1", 1, addr1),
       createPrefixKeyValue("2", 1, addr2)},
      {},
      {},
      {});
  sendKvPublication(publication, true, true);

  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  /*
   * Initial updates, before ADJACENCY_DB_SYNCED shall not produce
   * propagation time counters
   */
  auto counters = fb303::fbData->getCounters();
  EXPECT_EQ(counters["decision.linkstate.up.propagation_time_ms.avg.60"], 0);

  lsRec1.linkStatusMap()["1/2"].status() = thrift::LinkStatusEnum::DOWN;
  lsRec1.linkStatusMap()["1/2"].unixTs() = now - 4;
  lsRec2.linkStatusMap()["2/1"].status() = thrift::LinkStatusEnum::DOWN;
  lsRec2.linkStatusMap()["2/1"].unixTs() = now - 4;

  publication = createThriftPublication(
      {{"adj:1",
        createAdjValueWithLinkStatus(serializer, "1", 2, {}, lsRec1, false, 1)},
       {"adj:2",
        createAdjValueWithLinkStatus(
            serializer, "2", 2, {}, lsRec2, false, 2)}},
      {},
      {},
      {});
  sendKvPublication(publication);

  /*
   * This publication is after ADJACENCY_DB_SYNCED, verify that it produces
   * propagation time counters
   */
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  counters = fb303::fbData->getCounters();
  EXPECT_GT(counters["decision.linkstate.down.propagation_time_ms.avg.60"], 1);
  EXPECT_LT(
      counters["decision.linkstate.down.propagation_time_ms.avg.60"], 4000);
}

TEST(DecisionPendingUpdates, perfEvents) {
  openr::detail::DecisionPendingUpdates updates("node1");
  LinkState::LinkStateChange linkStateChange;
  updates.applyLinkStateChange("node2", linkStateChange, kEmptyPerfEventRef);
  EXPECT_THAT(*updates.perfEvents()->events(), testing::SizeIs(1));
  EXPECT_EQ(
      *updates.perfEvents()->events()->front().eventDescr(),
      "DECISION_RECEIVED");
  thrift::PrefixDatabase perfEventDb;
  perfEventDb.perfEvents() = openr::thrift::PerfEvents();
  auto& earlierEvents = *perfEventDb.perfEvents();
  earlierEvents.events()->push_back({});
  *earlierEvents.events()->back().nodeName() = "node3";
  *earlierEvents.events()->back().eventDescr() = "EARLIER";
  earlierEvents.events()->back().unixTs() = 1;
  updates.applyPrefixStateChange({}, perfEventDb.perfEvents());

  // expect what we hasd to be displaced by this
  EXPECT_THAT(*updates.perfEvents()->events(), testing::SizeIs(2));
  EXPECT_EQ(*updates.perfEvents()->events()->front().eventDescr(), "EARLIER");
  EXPECT_EQ(
      *updates.perfEvents()->events()->back().eventDescr(),
      "DECISION_RECEIVED");
}

int
main(int argc, char* argv[]) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  folly::init(&argc, &argv);
  google::InstallFailureSignalHandler();

  // Run the tests
  return RUN_ALL_TESTS();
}
