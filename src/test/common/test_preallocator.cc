#include "common/preallocator.h"

#include <list>
#include <map>
#include <set>
#include <vector>

#include <gtest/gtest.h>

struct Allocation {
  void* p;
  size_t n;
};
// comparison and stream ouput operators for ASSERT_EQ()
bool operator==(const Allocation& lhs, const Allocation& rhs) {
  return lhs.p == rhs.p && lhs.n == rhs.n;
}
std::ostream& operator<<(std::ostream& out, const Allocation& a) {
  return out << a.p << '[' << a.n << ']';
}
using Allocs = std::vector<Allocation>;


// allocator wrapper to record allocate/dellocate calls
template <typename T, typename Alloc = std::allocator<T>>
struct RecordingAllocator : public Alloc {
  // record the pointer and count for each allocation/deallocation
  Allocs& allocs;
  Allocs& deallocs;

  RecordingAllocator(Allocs& allocs, Allocs& deallocs)
    : allocs(allocs), deallocs(deallocs) {}

  RecordingAllocator(const RecordingAllocator&) = default;

  template <typename U>
  RecordingAllocator(const RecordingAllocator<U>& other)
    : Alloc(other), allocs(other.allocs), deallocs(other.deallocs) {}

  using typename Alloc::size_type;
  using typename Alloc::pointer;

  template <typename U>
  using base_other = typename Alloc::template rebind<U>::other;

  template <typename U>
  struct rebind { using other = RecordingAllocator<U, base_other<U>>; };

  pointer allocate(size_type n, typename base_other<void>::const_pointer hint=0)
  {
    auto p = Alloc::allocate(n, hint);
    allocs.push_back(Allocation{p, n});
    return p;
  }

  void deallocate(pointer p, size_type n)
  {
    deallocs.push_back(Allocation{p, n});
    Alloc::deallocate(p, n);
  }
};

// use RecordingAllocator as the base class for our preallocator to record the
// calls that it passes through
template <typename T, size_t N>
using Preallocator = ceph::preallocator<T, N, RecordingAllocator<T>>;


TEST(Preallocator, AllocateOverflow)
{
  Allocs allocs, deallocs;
  auto allocator = Preallocator<int, 1>{{allocs, deallocs}};
  auto p1 = allocator.allocate(1);
  ASSERT_TRUE(allocs.empty()); // allocation handled internally
  auto p2 = allocator.allocate(1);
  ASSERT_EQ(Allocs({{p2, 1}}), allocs); // overflows

  allocator.deallocate(p1, 1);
  ASSERT_TRUE(deallocs.empty());
  allocator.deallocate(p2, 1);
  ASSERT_EQ(Allocs({{p2, 1}}), deallocs);
}

TEST(Preallocator, AllocateUnalignedOverflow)
{
  Allocs allocs, deallocs;
  auto allocator = Preallocator<int, 2>{{allocs, deallocs}};
  auto p1 = allocator.allocate(1);
  ASSERT_TRUE(allocs.empty());
  // should overflow even though preallocator::index=1 and N=2
  auto p2 = allocator.allocate(2);
  ASSERT_EQ(Allocs({{p2, 2}}), allocs);

  allocator.deallocate(p1, 1);
  ASSERT_TRUE(deallocs.empty());
  allocator.deallocate(p2, 2);
  ASSERT_EQ(Allocs({{p2, 2}}), deallocs);
}

TEST(Preallocator, DeallocateFront)
{
  Allocs allocs, deallocs;
  auto allocator = Preallocator<int, 2>{{allocs, deallocs}};
  auto p1 = allocator.allocate(1);
  auto p2 = allocator.allocate(1);
  ASSERT_TRUE(allocs.empty());
  // because it wasn't deallocated from the back, this won't reclaim storage
  allocator.deallocate(p1, 1);
  ASSERT_TRUE(deallocs.empty());
  // so an allocation to replace it will overflow
  p1 = allocator.allocate(1);
  ASSERT_EQ(Allocs({{p1, 1}}), allocs);

  allocator.deallocate(p2, 1);
  ASSERT_TRUE(deallocs.empty());
  allocator.deallocate(p1, 1);
  ASSERT_EQ(Allocs({{p1, 1}}), deallocs);
}

TEST(Preallocator, DeallocateBack)
{
  Allocs allocs, deallocs;
  auto allocator = Preallocator<int, 2>{{allocs, deallocs}};
  auto p1 = allocator.allocate(1);
  auto p2 = allocator.allocate(1);
  ASSERT_TRUE(allocs.empty());
  // deallocate from the back, and verify that the storage is reclaimed
  allocator.deallocate(p2, 1);
  ASSERT_TRUE(deallocs.empty());
  // so an allocation to replace it won't overflow
  p2 = allocator.allocate(1);
  ASSERT_TRUE(allocs.empty());

  allocator.deallocate(p2, 1);
  allocator.deallocate(p1, 1);
  ASSERT_TRUE(deallocs.empty());
}

TEST(Preallocator, DeallocateReverse)
{
  Allocs allocs, deallocs;
  auto allocator = Preallocator<int, 4>{{allocs, deallocs}};
  auto p1 = allocator.allocate(1);
  auto p2 = allocator.allocate(1);
  auto p3 = allocator.allocate(1);
  auto p4 = allocator.allocate(1);
  ASSERT_TRUE(allocs.empty());
  // deallocate in reverse, and verify that the storage is reclaimed
  allocator.deallocate(p4, 1);
  allocator.deallocate(p3, 1);
  allocator.deallocate(p2, 1);
  allocator.deallocate(p1, 1);
  ASSERT_TRUE(deallocs.empty());
  // so all allocations to replace them won't overflow
  p1 = allocator.allocate(1);
  p2 = allocator.allocate(1);
  p3 = allocator.allocate(1);
  p4 = allocator.allocate(1);
  ASSERT_TRUE(allocs.empty());

  // but the next should overflow
  auto p5 = allocator.allocate(1);
  ASSERT_EQ(Allocs({{p5, 1}}), allocs);

  allocator.deallocate(p4, 1);
  allocator.deallocate(p3, 1);
  allocator.deallocate(p2, 1);
  allocator.deallocate(p1, 1);
  ASSERT_TRUE(deallocs.empty());

  allocator.deallocate(p5, 1);
  ASSERT_EQ(Allocs({{p5, 1}}), deallocs);
}

TEST(Preallocator, List)
{
  Allocs allocs, deallocs;
  // std::list<int> with preallocator
  auto c = std::list<int, Preallocator<int, 5>>{{{allocs, deallocs}}};

  c.push_back(1);
  c.push_back(2);
  c.push_back(3);
  c.push_back(4);
  c.push_back(5);
  ASSERT_TRUE(allocs.empty());
  c.push_back(6);
  ASSERT_EQ(1u, allocs.size()); // overflows

  auto expected = {1, 2, 3, 4, 5, 6};
  ASSERT_TRUE(std::equal(std::begin(expected), std::end(expected),
                         std::begin(c)));
}

TEST(Preallocator, Map)
{
  Allocs allocs, deallocs;
  // std::map<int, int> with preallocator
  using cmp = std::less<int>;
  using allocator_type = Preallocator<std::pair<const int, int>, 5>;
  auto c = std::map<int, int, cmp, allocator_type>{{{allocs, deallocs}}};

  c[1] = 1;
  c[2] = 4;
  c[3] = 9;
  c[4] = 16;
  c[5] = 25;
  ASSERT_TRUE(allocs.empty());
  c[6] = 36;
  ASSERT_EQ(1u, allocs.size()); // overflows

  auto expected = std::map<int, int>{
    {1, 1}, {2, 4}, {3, 9}, {4, 16}, {5, 25}, {6,36}};
  ASSERT_TRUE(std::equal(std::begin(expected), std::end(expected),
                         std::begin(c)));
}

TEST(Preallocator, Set)
{
  Allocs allocs, deallocs;
  // std::set<int> with preallocator
  using cmp = std::less<int>;
  auto c = std::set<int, cmp, Preallocator<int, 5>>{{{allocs, deallocs}}};

  c.insert(1);
  c.insert(2);
  c.insert(3);
  c.insert(4);
  c.insert(5);
  ASSERT_TRUE(allocs.empty());
  c.insert(6);
  ASSERT_EQ(1u, allocs.size()); // overflows

  auto expected = {1, 2, 3, 4, 5, 6};
  ASSERT_TRUE(std::equal(std::begin(expected), std::end(expected),
                         std::begin(c)));
}

TEST(Preallocator, Vector)
{
  Allocs allocs, deallocs;
  // std::vector<int> with preallocator
  auto c = std::vector<int, Preallocator<int, 5>>{{{allocs, deallocs}}};
  c.reserve(5);

  c.push_back(1);
  c.push_back(2);
  c.push_back(3);
  c.push_back(4);
  c.push_back(5);
  ASSERT_TRUE(allocs.empty());
  c.push_back(6);
  ASSERT_EQ(Allocs({{c.data(), 10}}), allocs); // overflows

  auto expected = {1, 2, 3, 4, 5, 6};
  ASSERT_TRUE(std::equal(std::begin(expected), std::end(expected),
                         std::begin(c)));
}
