#pragma once

#include <memory>
#include <boost/integer.hpp>
#include "include/assert.h"

namespace ceph {

// a std container-compatible allocator that satisfies the first N allocations
// from a preallocated array
template <typename T, size_t N, typename Alloc = std::allocator<T>>
class preallocator : public Alloc {
 public:
  // derive types from the default allocator
  using typename Alloc::size_type;
  using typename Alloc::pointer;

  // rebind converts T to other types. this allows std::list<T> to allocate
  // list_node<T> instead of T, for example. we implement our rebind in terms of
  // the base allocator's
  template <typename U>
  using base_other = typename Alloc::template rebind<U>::other;

  template <typename U>
  struct rebind { using other = preallocator<U, N, base_other<U>>; };

 private:
  // use aligned_storage to avoid ctors/dtors
  using storage_t = typename std::aligned_storage<sizeof(T), alignof(T)>::type;
  // inline storage for N elements
  storage_t storage[N];

  // use the smallest uint type that can hold N
  using index_t = typename boost::uint_value_t<N>::least;
  // index of the next inline entry to allocate
  index_t index {0};

  // return a pointer to storage entry i
  pointer storage_at(index_t i) {
    return reinterpret_cast<pointer>(&storage[i]);
  }

 public:
  preallocator(const Alloc& alloc = Alloc()) noexcept
    : Alloc(alloc) {}

  // copying this allocator cannot work, because one allocator cannot possibly
  // deallocate inline storage returned by another. however, allocator copies
  // are built into the constructors of all std library containers that support
  // them, so we can't just disable the copy constructor. the best we can do
  // here is assert that the given allocator is unused (index == 0). but this
  // means that the compiler won't enforce our non-copyable/non-moveable
  // requirements on containers that use this allocator, so it's recommended
  // that the allocator is only used by container wrapper classes that
  // explicitly disable copy and move
  preallocator(const preallocator<T, N>& other)
    : Alloc(other) {
    assert(other.index == 0);
  }

  // converting copy constructor for rebind
  template <typename U>
  preallocator(const preallocator<U, N>& other)
    : Alloc(other) {
    assert(other.index == 0);
  }

  pointer allocate(size_type n, typename base_other<void>::const_pointer hint=0)
  {
    // use inline storage if we can satisfy the entire request
    if (index + n <= N) {
      auto p = storage_at(index);
      index += n;
      return p;
    }
    // fall back to the default allocator
    return Alloc::allocate(n, hint);
  }

  void deallocate(pointer p, size_type n)
  {
    auto end = storage_at(index);
    if (p + n == end) {
      // only handle the simple case, and reclaim storage at the end
      index -= n;
    } else if (storage_at(0) <= p && p < end) {
      // discard entries that aren't at the end
    } else {
      // return it to the default allocator
      Alloc::deallocate(p, n);
    }
  }
};

} // namespace ceph
