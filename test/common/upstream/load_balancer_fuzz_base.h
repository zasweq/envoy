#include "envoy/config/cluster/v3/cluster.pb.h"

#include "common/upstream/load_balancer_impl.h"

#include "test/common/upstream/load_balancer_fuzz.pb.validate.h"
#include "test/fuzz/random.h"
#include "test/mocks/common.h"
#include "test/mocks/runtime/mocks.h"
#include "test/mocks/upstream/cluster_info.h"
#include "test/mocks/upstream/host_set.h"
#include "test/mocks/upstream/load_balancer_context.h"
#include "test/mocks/upstream/priority_set.h"

namespace Envoy {
namespace Upstream {

// This class implements replay logic, and also handles the initial setup of static host sets and
// the subsequent updates to those sets.
class LoadBalancerFuzzBase {
public:
  LoadBalancerFuzzBase() : stats_(ClusterInfoImpl::generateStats(stats_store_)){};

  // Initializes load balancer components shared amongst every load balancer, random_, and
  // priority_set_
  void initializeLbComponents(const test::common::upstream::LoadBalancerTestCase& input);
  void updateHealthFlagsForAHostSet(const uint64_t host_priority, const uint32_t num_healthy_hosts,
                                    const uint32_t num_degraded_hosts,
                                    const uint32_t num_excluded_hosts,
                                    const std::string random_bytestring);
  // These two actions have a lot of logic attached to them. However, all the logic that the load
  // balancer needs to run its algorithm is already encapsulated within the load balancer. Thus,
  // once the load balancer is constructed, all this class has to do is call lb_->peekAnotherHost()
  // and lb_->chooseHost().
  void prefetch();
  void chooseHost();
  ~LoadBalancerFuzzBase() = default;
  void replay(const Protobuf::RepeatedPtrField<test::common::upstream::LbAction>& actions);

  void clearStaticHostsHealthFlags();

  // These public objects shared amongst all types of load balancers will be used to construct load
  // balancers in specific load balancer fuzz classes
  Stats::IsolatedStoreImpl stats_store_;
  ClusterStats stats_;
  NiceMock<Runtime::MockLoader> runtime_;
  Random::PsuedoRandomGenerator64 random_;
  NiceMock<MockPrioritySet> priority_set_;
  static std::shared_ptr<MockClusterInfo> info_;
  std::unique_ptr<LoadBalancerBase> lb_;

private:
  // Untrusted upstreams don't have the ability to change the host set size, so keep it constant
  // over the fuzz iteration.
  void
  initializeASingleHostSet(const test::common::upstream::SetupPriorityLevel& setup_priority_level,
                           const uint8_t priority_level);

  // There are used to construct the priority set at the beginning of the fuzz iteration
  uint16_t port_ = 80; //TODO: switch to host index which doesn't start at 80
  uint8_t num_priority_levels_ = 0;

  // This map used when updating health flags - making sure the health flags are updated hosts in
  // localities Key - index of host within full host list, value - locality level host at index is
  // in
  absl::node_hash_map<uint8_t, uint8_t> locality_indexes_;

  // Will statically initialize 10000? hosts in this vector
  // Will have to clear flags at the end of each iteration here
  static HostVector initialized_hosts_;
};

} // namespace Upstream
} // namespace Envoy
