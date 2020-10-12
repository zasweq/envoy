#include "test/common/upstream/load_balancer_fuzz_base.h"

#include "test/common/upstream/utility.h"

namespace Envoy {
namespace Upstream {

void LoadBalancerFuzzBase::initializeASingleHostSet(test::common::upstream::SetupHostSet setup_host_set,
                                                    uint8_t index_of_host_set) {
  uint32_t num_hosts_in_host_set = setup_host_set.num_hosts_in_host_set();
  ENVOY_LOG_MISC(trace, "Will attempt to initialize host set {} with {} hosts.", index_of_host_set,
                 num_hosts_in_host_set);
  MockHostSet& host_set = *priority_set_.getMockHostSet(index_of_host_set);
  uint32_t hosts_made = 0;
  // Cap each host set at 256 hosts for efficiency
  uint32_t max_num_hosts_in_host_set = 256;
  // Leave port clause in for future changes
  while (hosts_made < std::min(num_hosts_in_host_set, max_num_hosts_in_host_set) && port_ < 65535) {
    host_set.hosts_.push_back(makeTestHost(info_, "tcp://127.0.0.1:" + std::to_string(port_)));
    ++port_;
    ++hosts_made;
  }

  /*if (setup_host_set.num_hosts_locality_one() + setup_host_set.num_hosts_locality_two() + setup_host_set.num_hosts_locality_three() == 0) {
    return;
  }*/
  //Do we really always want 3 localities?
  host_set.hosts_per_locality_.get()->get() = {{}, {}, {}};

  // Setup 3 subsets to construct localities with - these are also static the whole iteration
  Fuzz::ProperSubsetSelector subset_selector(setup_host_set.random_bytestring());

  // These represent the indexes of the host set list to put in each locality level 
  std::vector<std::vector<uint8_t>> localities = subset_selector.constructSubsets(
      3, {setup_host_set.num_hosts_locality_one(), setup_host_set.num_hosts_locality_two(), setup_host_set.num_hosts_locality_three()}, num_hosts_in_host_set);

  for (uint8_t index : localities[0]) {
    ENVOY_LOG_MISC(trace, "Added host at index {} to locality 1", index);
    host_set.hosts_per_locality_.get()->get().at(0).emplace_back(host_set.hosts_[index]); //Wrong thing to push back
    locality_indexes_[index] = 1;
  }
  
  for (uint8_t index : localities[1]) {
    ENVOY_LOG_MISC(trace, "Added host at index {} to locality 2", index);
    host_set.hosts_per_locality_.get()->get().at(1).emplace_back(host_set.hosts_[index]);
    locality_indexes_[index] = 2;
  }

  for (uint8_t index : localities[2]) {
    ENVOY_LOG_MISC(trace, "Added host at index {} to locality 3", index);
    host_set.hosts_per_locality_.get()->get().at(2).emplace_back(host_set.hosts_[index]);
    locality_indexes_[index] = 3;
  }
}

// Initializes random and fixed host sets
void LoadBalancerFuzzBase::initializeLbComponents(
    const test::common::upstream::LoadBalancerTestCase& input) {
  random_.initializeSeed(input.seed_for_prng());
  uint8_t index_of_host_set = 0;
  for (const auto& setup_host_set : input.setup_host_sets()) {
    initializeASingleHostSet(setup_host_set, index_of_host_set);
    index_of_host_set++;
  }
  num_host_sets_ = index_of_host_set;
}

//Update host sets should be refactored to this high level idea: construct a update_hosts_params object, then pipe it into updateHosts on a priority set

// Updating host sets is shared amongst all the load balancer tests. Since logically, we're just
// setting the mock priority set to have certain values, and all load balancers interface with host
// sets and their health statuses, this action maps to all load balancers.
void LoadBalancerFuzzBase::updateHealthFlagsForAHostSet(uint64_t host_index,
                                                        uint32_t num_healthy_hosts,
                                                        uint32_t num_degraded_hosts,
                                                        uint32_t num_excluded_hosts,
                                                        std::string random_bytestring) {
  uint8_t index_of_host_set = host_index % num_host_sets_;
  ENVOY_LOG_MISC(trace, "Updating health flags for host set: {}", index_of_host_set);
  MockHostSet& host_set = *priority_set_.getMockHostSet(index_of_host_set);
  // This downcast will not overflow because size is capped by port numbers
  uint32_t host_set_size = host_set.hosts_.size();
  host_set.healthy_hosts_.clear();
  host_set.degraded_hosts_.clear();
  host_set.excluded_hosts_.clear();


  std::vector<HostVector>& healthy_hosts_per_locality = host_set.healthy_hosts_per_locality_.get()->get();
  std::vector<HostVector>& degraded_hosts_per_locality = host_set.degraded_hosts_per_locality_.get()->get();
  /*const */std::vector<HostVector>& excluded_hosts_per_locality = host_set.excluded_hosts_per_locality_.get()->get();
  
  healthy_hosts_per_locality.clear();
  degraded_hosts_per_locality.clear();
  excluded_hosts_per_locality.clear();

  healthy_hosts_per_locality = {{}, {}, {}};
  degraded_hosts_per_locality = {{}, {}, {}};
  excluded_hosts_per_locality = {{}, {}, {}};

  Fuzz::ProperSubsetSelector subset_selector(random_bytestring);

  std::vector<std::vector<uint8_t>> subsets = subset_selector.constructSubsets(
      3, {num_healthy_hosts, num_degraded_hosts, num_excluded_hosts}, host_set_size);

  // Healthy hosts are first subset
  for (uint8_t index : subsets.at(0)) {
    host_set.healthy_hosts_.push_back(host_set.hosts_[index]);
    ENVOY_LOG_MISC(trace, "Index of host made healthy: {}", index);
    //If the host is in a locality, we have to add it to healthy hosts per locality
    if (!(locality_indexes_.find(index) == locality_indexes_.end())) {
      //Chooses correct locality number from locality indexes map, and puts correct host there
      host_set.healthy_hosts_per_locality_.getHostVector()[locality_indexes_[index]].push_back_(host_set.hosts_[index]);
      ENVOY_LOG_MISC(trace, "Added healthy host at index {} in locality {}", index, locality_indexes_[index]);
    }
  }

  // Degraded hosts are second subset
  for (uint8_t index : subsets.at(1)) {
    host_set.degraded_hosts_.push_back(host_set.hosts_[index]);
    ENVOY_LOG_MISC(trace, "Index of host made degraded: {}", index);
    //If the host is in a locality, we have to add it to degraded hosts per locality
    if (!(locality_indexes_.find(index) == locality_indexes_.end())) {
      //Chooses correct locality number from locality indexes map, and puts correct host there
      host_set.degraded_hosts_per_locality_.getHostVector()[locality_indexes_[index]].push_back_(host_set.hosts_[index]);
      ENVOY_LOG_MISC(trace, "Added degraded host at index {} in locality {}", index, locality_indexes_[index]);
    }
  }

  // Excluded hosts are third subset
  for (uint8_t index : subsets.at(2)) {
    host_set.excluded_hosts_.push_back(host_set.hosts_[index]);
    ENVOY_LOG_MISC(trace, "Index of host made excluded: {}", index);
    //If the host is in a locality, we have to add it to excluded hosts per locality
    if (!(locality_indexes_.find(index) == locality_indexes_.end())) {
      //Chooses correct locality number from locality indexes map, and puts correct host there
      host_set.excluded_hosts_per_locality_.getHostVector()[locality_indexes_[index]].push_back_(host_set.hosts_[index]);
      ENVOY_LOG_MISC(trace, "Added excluded host at index {} in locality {}", index, locality_indexes_[index]);
    }
  }

  host_set.runCallbacks({}, {});
}

void LoadBalancerFuzzBase::prefetch() {
  // TODO: context, could generate it in proto action
  lb_->peekAnotherHost(nullptr);
}

void LoadBalancerFuzzBase::chooseHost() {
  // TODO: context, could generate it in proto action
  lb_->chooseHost(nullptr);
}

void LoadBalancerFuzzBase::replay(
    const Protobuf::RepeatedPtrField<test::common::upstream::LbAction>& actions) {
  constexpr auto max_actions = 64;
  for (int i = 0; i < std::min(max_actions, actions.size()); ++i) {
    const auto& event = actions.at(i);
    ENVOY_LOG_MISC(trace, "Action: {}", event.DebugString());
    switch (event.action_selector_case()) {
    case test::common::upstream::LbAction::kUpdateHealthFlags: {
      updateHealthFlagsForAHostSet(event.update_health_flags().host_index(),
                                   event.update_health_flags().num_healthy_hosts(),
                                   event.update_health_flags().num_degraded_hosts(),
                                   event.update_health_flags().num_excluded_hosts(),
                                   event.update_health_flags().random_bytestring());
      break;
    }
    case test::common::upstream::LbAction::kPrefetch:
      prefetch();
      break;
    case test::common::upstream::LbAction::kChooseHost:
      chooseHost();
      break;
    default:
      break;
    }
  }
}

} // namespace Upstream
} // namespace Envoy
