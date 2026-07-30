#include <cppy3/cppy3.hpp>
#include <cppy3/cppy3_numpy.hpp>
#include <iostream>

// Bug #6 (plan Part 1): NDArray<Type>::wrap(Type*, int) -- the 1D overload --
// passes `(void*)&data` (the address of the *local pointer parameter*) to
// PyArray_SimpleNewFromData instead of `(void*)data` (the caller's buffer).
// The 2D overload at wrap(Type*, int, int) passes `(void*)data` correctly.
// The array ends up backed by the few stack bytes of the `data` parameter
// itself, not the caller's array -- reads return garbage/stale-stack data
// and, for n > 1, walk off the single pointer-sized stack slot entirely
// (an ASan stack-buffer-overflow).
int main()
{
  cppy3::Interpreter interpreter;
  cppy3::importNumpy();

  double cData[4] = {10.0, 20.0, 30.0, 40.0};

  cppy3::NDArray<double> nd;
  nd.wrap(cData, 4);

  bool mismatch = false;
  for (int i = 0; i < 4; i++)
  {
    std::cout << "bug06: nd(" << i << ")=" << nd(i) << " cData[" << i << "]=" << cData[i] << std::endl;
    if (nd(i) != cData[i])
      mismatch = true;
  }

  if (mismatch)
  {
    std::cerr << "bug06: CONFIRMED - wrap(Type*, int) does not alias the caller's buffer "
                 "(values read back do not match; ASan may also have flagged a stack-buffer-overflow above)"
              << std::endl;
    return 1;
  }
  std::cout << "bug06: not reproduced (already fixed?)" << std::endl;
  return 0;
}
