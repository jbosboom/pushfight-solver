/* 
 * File:   set_bits_range.h
 * Author: jbosboom
 *
 * Created on March 28, 2020, 6:02 PM
 */

#ifndef SET_BITS_RANGE_H
#define SET_BITS_RANGE_H

#include <limits>
#include <bit>

struct set_bits_range_sentinel {};

template<class Integer>
struct set_bits_range_iterator {
	Integer x;
	unsigned int idx = 0;
	set_bits_range_iterator(Integer x) : x(x) {
		this->operator++();
	}
	set_bits_range_iterator& operator++() {
		idx = std::countr_zero(x);
		x &= ~(1 << idx);
		return *this;
	}
	unsigned int operator*() const {
		return idx;
	}
	bool operator!=(set_bits_range_sentinel) const {
		return idx != std::numeric_limits<Integer>::digits;
	}
};

template<class T>
struct set_bits_range {
	T t;
	template<class X>
	set_bits_range(X x) : t(+x) {} //unary plus to promote to uint

	auto begin() const {
		return set_bits_range_iterator<T>(t);
	}
	set_bits_range_sentinel end() const {
		return {};
	}
};

set_bits_range(unsigned int) -> set_bits_range<unsigned int>;

#endif /* SET_BITS_RANGE_H */

