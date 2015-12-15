#pragma once

#include <vector>
#include "common/preallocator.h"

namespace ceph {

// a std::vector wrapper that uses a preallocator as its allocator
template <typename T, size_t N,
          typename Alloc = std::allocator<T>,
          typename Prealloc = preallocator<T, N, Alloc>,
          typename Vector = std::vector<T, Prealloc>>
class prealloc_vector : public Vector {
 public:
  using typename Vector::size_type;
  using typename Vector::value_type;
  using typename Vector::allocator_type;

  // override all of the vector constructors to call reserve(N) first
  explicit prealloc_vector(const allocator_type& alloc = allocator_type())
    : Vector(alloc) {
    this->reserve(N);
  }
  explicit prealloc_vector(size_type n) {
    this->reserve(N);
    resize(n);
  }
  prealloc_vector(size_type n, const value_type& val,
                  const allocator_type& alloc = allocator_type())
    : Vector(alloc) {
    this->reserve(N);
    resize(n, val);
  }
  template <class InputIterator>
  prealloc_vector(InputIterator first, InputIterator last,
                  const allocator_type& alloc = allocator_type())
    : Vector(alloc) {
    this->reserve(N);
    insert(this->end(), first, last);
  }
  prealloc_vector(std::initializer_list<value_type> il,
                  const allocator_type& alloc = allocator_type())
    : Vector(alloc) {
    this->reserve(N);
    insert(this->end(), il);
  }

  // copy and move operations are disabled. both involve a copy of the
  // allocator, and the preallocator can never do the right thing for copy
  prealloc_vector(const prealloc_vector& x) = delete;
  prealloc_vector(const prealloc_vector& x,
                  const allocator_type& alloc) = delete;
  prealloc_vector& operator=(const prealloc_vector& x) = delete;

  prealloc_vector(prealloc_vector&& x) = delete;
  prealloc_vector(prealloc_vector&& x,
                  const allocator_type& alloc) = delete;
  prealloc_vector& operator=(prealloc_vector&& x) = delete;
};

} // namespace ceph
