#include "src/utils/intrusive_ref_count.h"

#include <thread>  // NOLINT(build/c++11)
#include <vector>

#include "gtest/gtest.h"

namespace valkey_search {

namespace {
struct Tester : public IntrusiveRefCount {
  Tester(int _value, bool &_deleted) : value(_value), deleted(_deleted) {}
  ~Tester() { deleted = true; }
  int value;
  bool &deleted;
};

TEST(IntrusiveRefCountTest, SimpleRefCount) {
  bool deleted = false;
  {
    auto ptr = CreateUniquePtr(Tester, 10, deleted);
    EXPECT_EQ(ptr->value, 10);
    EXPECT_FALSE(deleted);
    ptr->IncrementRef();
    ptr->IncrementRef();
    ptr->IncrementRef();
    EXPECT_EQ(ptr->value, 10);
    EXPECT_FALSE(deleted);
    ptr->DecrementRef();
    ptr->DecrementRef();
    ptr->DecrementRef();
    EXPECT_EQ(ptr->value, 10);
    EXPECT_FALSE(deleted);
    EXPECT_EQ(ptr->value, 10);
  }
  EXPECT_TRUE(deleted);
}

DEFINE_UNIQUE_PTR_TYPE(Tester);

void FunctionToRunInThread(int thread_id, Tester* ptr) {
  for (int i = 0; i < 1000 * thread_id; ++i) {
    ptr->IncrementRef();
    ptr->DecrementRef();
  }
}

TEST(IntrusiveRefCountTest, Concurrent) {
  const int num_threads = 5;
  std::vector<std::thread> threads;
  bool deleted = false;
  {
    auto ptr = CreateUniquePtr(Tester, 10, deleted);
    for (int i = 0; i < num_threads; ++i) {
      threads.emplace_back(FunctionToRunInThread, i, ptr.get());
    }

    for (auto &thread : threads) {
      thread.join();
    }
  }
  EXPECT_TRUE(deleted);
}

}  // namespace
}  // namespace valkey_search
