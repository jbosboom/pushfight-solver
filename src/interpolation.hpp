/* 
 * File:   interpolation.hpp
 * Author: jbosboom
 *
 * Created on April 28, 2020, 12:41 PM
 */

#ifndef INTERPOLATION_HPP
#define INTERPOLATION_HPP

#include <algorithm>

namespace interp {

template<typename Iterator, typename T>
Iterator upper_bound(Iterator first, Iterator last, const T& needle) {
	while (first != last) {
		if (needle < *first)
			return first;
		if (*(last-1) <= needle)
			return last;

		std::ptrdiff_t idx = ((double)(needle - *first) / (double)(*(last-1) - *first)) * std::distance(first, last);
		assert(0 <= idx && idx < std::distance(first, last));
		Iterator it = first;
		std::advance(it, idx);
		if (needle == *it)
			return it+1;
		if (needle < *it)
			last = it;
		else
			first = it+1;
	}
	return last;
}

}

#endif /* INTERPOLATION_HPP */

