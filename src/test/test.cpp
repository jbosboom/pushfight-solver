#include "precompiled.hpp"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN //genbuild {'entrypoint': True}
#include <doctest.h>

using std::vector;


#include "set_bits_range.hpp"

TEST_CASE("SetBitsRange_Zero") {
	auto r = set_bits_range(0b0);
	vector<unsigned int> actual;
	for (auto x : r)
		actual.push_back(x);
	vector<unsigned int> expected;
	CHECK_EQ(actual, expected);
}

TEST_CASE("SetBitsRange_Nonzero00") {
	auto r = set_bits_range(0b1);
	vector<unsigned int> actual;
	for (auto x : r)
		actual.push_back(x);
	vector<unsigned int> expected = {0};
	CHECK_EQ(actual, expected);
}

TEST_CASE("SetBitsRange_Nonzero01") {
	auto r = set_bits_range(0b10);
	vector<unsigned int> actual;
	for (auto x : r)
		actual.push_back(x);
	vector<unsigned int> expected = {1};
	CHECK_EQ(actual, expected);
}

TEST_CASE("SetBitsRange_Nonzero02") {
	auto r = set_bits_range(0b11);
	vector<unsigned int> actual;
	for (auto x : r)
		actual.push_back(x);
	vector<unsigned int> expected = {0, 1};
	CHECK_EQ(actual, expected);
}