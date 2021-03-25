#pragma once

#if __cplusplus < 201703L
//#include "optional.hpp"
//using std::experimental::nullopt;
//using std::experimental::optional;
//namespace std {
//using std::experimental::nullopt_t;
//}
#include<boost/optional.hpp>
using boost::optional;
#else
#include <optional>
using std::nullopt;
using std::optional;
#endif

