#include <memory>
#include <random>
#include <utility>
#include <vector>

#include "envoy/common/random_generator.h"

#include "common/common/assert.h"
#include "common/common/logger.h"

namespace Envoy {
namespace Random {

class PsuedoRandomGenerator64 : public RandomGenerator {
public:
  PsuedoRandomGenerator64() = default;
  ~PsuedoRandomGenerator64() override = default;

  void initializeSeed(uint64_t seed) { prng_ = std::make_unique<std::mt19937_64>(seed); }

  // RandomGenerator
  uint64_t random() override {
    // Makes sure initializeSeed() was already called
    ASSERT(prng_ != nullptr);
    uint64_t to_return = (*prng_)();
    ENVOY_LOG_MISC(trace, "random() returned: {}", to_return);
    return to_return;
  }
  std::string uuid() override { return ""; }
  std::unique_ptr<std::mt19937_64> prng_;
};

} // namespace Random

namespace Fuzz {
class ProperSubsetSelector {
public:
  ProperSubsetSelector(const std::string& random_bytestring)
      : random_bytestring_(random_bytestring) {}

  /**
   * This function does proper subset selection on a certain number of elements. It returns a vector
   * of vectors of bytes. Each vector of bytes represents the indexes of a single subset. The
   * "randomness" of the subset that the class will use is determined by a bytestring passed into
   * the class. Example: call into function with a vector {3, 5} representing subset sizes, and 15
   * as number_of_elements. This function would return something such as {{3, 14, 7}, {2, 1, 13, 8,
   * 6}}
   */

  std::vector<std::vector<uint8_t>>
  constructSubsets(const std::vector<uint32_t>& number_of_elements_in_each_subset,
                   uint32_t number_of_elements) {
    num_elements_left_ = number_of_elements;
    std::vector<uint8_t> index_vector;
    index_vector.reserve(number_of_elements);
    for (uint32_t i = 0; i < number_of_elements; i++) {
      index_vector.push_back(i);
    }
    std::vector<std::vector<uint8_t>> subsets;
    subsets.reserve(number_of_elements_in_each_subset.size());
    for (uint32_t i : number_of_elements_in_each_subset) {
      subsets.push_back(constructSubset(i, index_vector));
    }
    return subsets;
  }

private:
  // Builds a single subset by pulling indexes off index_vector_
  std::vector<uint8_t> constructSubset(uint32_t number_of_elements_in_subset,
                                       std::vector<uint8_t>& index_vector) {
    std::vector<uint8_t> subset;

    for (uint32_t i = 0; i < number_of_elements_in_subset && !(num_elements_left_ == 0); i++) {
      // Index of bytestring will wrap around if it "overflows" past the random bytestring's length.
      uint64_t index_of_index_vector =
          random_bytestring_[index_of_random_bytestring_ % random_bytestring_.length()] %
          num_elements_left_;
      uint64_t index = index_vector.at(index_of_index_vector);
      subset.push_back(index);
      // Move the index chosen to the end of the vector - will not be chosen again
      std::swap(index_vector[index_of_index_vector], index_vector[num_elements_left_ - 1]);
      --num_elements_left_;

      ++index_of_random_bytestring_;
    }

    return subset;
  }

  // This bytestring will be iterated through representing randomness in order to choose
  // subsets
  const std::string random_bytestring_;
  uint32_t index_of_random_bytestring_ = 0;

  // Used to make subset construction linear time complexity with std::swap - chosen indexes will be
  // swapped to end of vector, and won't be chosen again due to modding against this integer
  uint32_t num_elements_left_;
};

// Goal for now, scale this up to 32 bits across, from that you can figure out how to make this
// generic Only logical difference: what part of the bytestring to pull off of at each iteration
// Current implementation only takes 8 bits and iterates index by 1, this will iterate by 4 and take
// 32 bits at each position

// What to do - once switched to generic? all the fours to generic length
class ProperSubsetSelector32 {
public:
  ProperSubsetSelector32(const std::string& random_bytestring)
      : random_bytestring_(random_bytestring) {
    ASSERT(random_bytestring_.length() >= 4); // Must be at least 4 bytes wide for random call
    // Pull off the last few bits to make random_bytestring_ a multiple of 4 - this will make
    // iteration through the bytestring a lot easier and cleaner
    while (random_bytestring_.length() % 4 != 0) {
      random_bytestring_.pop_back();
    }
  }

  /**
   * This function does proper subset selection on a certain number of elements. It returns a vector
   * of vectors of bytes. Each vector of bytes represents the indexes of a single subset. The
   * "randomness" of the subset that the class will use is determined by a bytestring passed into
   * the class. Example: call into function with a vector {3, 5} representing subset sizes, and 15
   * as number_of_elements. This function would return something such as {{3, 14, 7}, {2, 1, 13, 8,
   * 6}}
   */

  std::vector<std::vector<uint32_t>>
  constructSubsets(const std::vector<uint32_t>& number_of_elements_in_each_subset,
                   uint32_t number_of_elements) {
    num_elements_left_ = number_of_elements;
    std::vector<uint32_t> index_vector;
    index_vector.reserve(number_of_elements);
    for (uint32_t i = 0; i < number_of_elements; i++) {
      index_vector.push_back(i);
    }
    std::vector<std::vector<uint32_t>> subsets;
    subsets.reserve(number_of_elements_in_each_subset.size());
    for (uint32_t i : number_of_elements_in_each_subset) {
      subsets.push_back(constructSubset(i, index_vector));
    }
    return subsets;
  }

private:
  // Builds a single subset by pulling indexes off index_vector_
  std::vector<uint32_t> constructSubset(uint32_t number_of_elements_in_subset,
                                        std::vector<uint32_t>& index_vector) {
    std::vector<uint32_t> subset;

    for (uint32_t i = 0; i < number_of_elements_in_subset && !(num_elements_left_ == 0); i++) {
      // Index of bytestring will wrap around if it "overflows" past the random bytestring's length.
      uint32_t* bytes_from_bytestring = reinterpret_cast<uint32_t*>(
          &random_bytestring_ + (index_of_random_bytestring_ % random_bytestring_.length()));
      uint32_t index_of_index_vector = *bytes_from_bytestring % num_elements_left_;
      uint32_t index = index_vector.at(index_of_index_vector);
      subset.push_back(index);
      // Move the index chosen to the end of the vector - will not be chosen again
      std::swap(index_vector[index_of_index_vector], index_vector[num_elements_left_ - 1]);
      --num_elements_left_;

      index_of_random_bytestring_ += 4;
    }

    return subset;
  }

  // This bytestring will be iterated through representing randomness in order to choose
  // subsets
  std::string random_bytestring_;
  uint32_t index_of_random_bytestring_ =
      0; // This will iterate by 4 every time bytestring is iterated

  // Used to make subset construction linear time complexity with std::swap - chosen indexes will be
  // swapped to end of vector, and won't be chosen again due to modding against this integer
  uint32_t num_elements_left_; // TODO: Make this on stack and pass down as reference?
};

} // namespace Fuzz
} // namespace Envoy
