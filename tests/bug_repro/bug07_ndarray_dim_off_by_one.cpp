#include <cppy3/cppy3.hpp>
#include <cppy3/cppy3_numpy.hpp>
#include <iostream>

// Bug #7 (plan Part 1): NDArray::dim1()/dim2() call dim(1)/dim(2), but
// dim(n) is 0-indexed (it forwards straight to PyArray_DIM(_ndarray, n)).
// For a 2D array, dim1() therefore returns the *second* extent, and dim2()
// asserts `PyArray_NDIM(_ndarray) > 2` which is false for a 2D array (NDIM
// is 2), i.e. it reads past the array's own dimension count.
//
// NOTE: this deliberately calls create(size_t, size_t) directly rather than
// going through the NDArray(int, int) constructor. That constructor is
// itself ambiguous for two integer arguments -- overload resolution prefers
// the exact `int` match on create(int n, bool fillZeros)'s first parameter,
// so NDArray(3, 5) silently calls the *1D* create(3, /*fillZeros=*/(bool)5)
// instead of the 2D create(size_t, size_t) overload. That is a separate,
// adjacent footgun worth its own line item, but this file's job is only to
// isolate the dim1()/dim2() indexing bug against a genuine 2D array.
int main()
{
  cppy3::Interpreter interpreter;
  cppy3::importNumpy();

  const size_t rows = 3, cols = 5;
  cppy3::NDArray<double> nd;
  nd.create(rows, cols); // unambiguously the 2D overload

  const int d1 = nd.dim1();
  std::cout << "bug07: dim1()=" << d1 << " (expected rows=" << rows << ")" << std::endl;

  if (static_cast<size_t>(d1) != rows)
  {
    std::cerr << "bug07: CONFIRMED - dim1() returns dim(1) instead of dim(0); got " << d1
               << " (the column count) instead of " << rows << " (the row count)" << std::endl;
    return 1;
  }
  std::cout << "bug07: not reproduced (already fixed?)" << std::endl;
  return 0;
}
