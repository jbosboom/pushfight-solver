#ifndef UTIL_HPP
#define UTIL_HPP

#include <charconv>

template<typename T>
T from_string(std::string_view view) {
	T value;
	if (auto [ptr, ec] = std::from_chars(view.begin(), view.end(), value, 10); ec != std::errc() || ptr != view.end()) {
		//TODO: extract slowpath into [[gnu::cold]] function
		throw std::logic_error("from_string problem"); //TODO: appropriate exception and messages
	}
	return value;
}

#endif /* UTIL_HPP */

