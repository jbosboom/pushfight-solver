#include "precompiled.hpp"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN //genbuild {'entrypoint': True}
#include <doctest.h>
#include <random>

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


#include "interpolation.hpp"

TEST_CASE("Interpolation_UpperBound00") {
	vector<unsigned int> haystack = {1};
	vector<unsigned int> needles = {0, 2};
	for (auto n : needles) {
		CAPTURE(n);
		auto actual = interp::upper_bound(haystack.begin(), haystack.end(), n);
		auto expected = std::upper_bound(haystack.begin(), haystack.end(), n);
		CHECK_UNARY((actual == haystack.end()) == (expected == haystack.end()));
		if (actual != haystack.end() && expected != haystack.end())
			CHECK_EQ(*actual, *expected);
	}
}

TEST_CASE("Interpolation_UpperBound01") {
	vector<unsigned int> haystack = {1};
	vector<unsigned int> needles = {1};
	for (auto n : needles) {
		CAPTURE(n);
		auto actual = interp::upper_bound(haystack.begin(), haystack.end(), n);
		auto expected = std::upper_bound(haystack.begin(), haystack.end(), n);
		CHECK_UNARY((actual == haystack.end()) == (expected == haystack.end()));
		if (actual != haystack.end() && expected != haystack.end())
			CHECK_EQ(*actual, *expected);
	}
}

TEST_CASE("Interpolation_UpperBound02") {
	vector<unsigned int> haystack = {1, 2, 3};
	vector<unsigned int> needles = {0, 1, 2, 3, 4};
	for (auto n : needles) {
		CAPTURE(n);
		auto actual = interp::upper_bound(haystack.begin(), haystack.end(), n);
		auto expected = std::upper_bound(haystack.begin(), haystack.end(), n);
		CHECK_UNARY((actual == haystack.end()) == (expected == haystack.end()));
		if (actual != haystack.end() && expected != haystack.end())
			CHECK_EQ(*actual, *expected);
	}
}

TEST_CASE("Interpolation_UpperBound03") {
	std::mt19937 gen(0);
	std::uniform_int_distribution<unsigned long> dist(
			std::numeric_limits<unsigned long>::min(),
			std::numeric_limits<unsigned long>::max());
	vector<unsigned long> haystack;
	haystack.reserve(20000);
	for (unsigned int i = 0; i < 20000; ++i)
		haystack.push_back(dist(gen));
	std::sort(haystack.begin(), haystack.end());
	haystack.erase(std::unique(haystack.begin(), haystack.end()), haystack.end());
	vector<unsigned long> needles;
	for (unsigned int i = 0; i < 1000; ++i)
		needles.push_back(dist(gen));
	for (auto n : needles) {
		CAPTURE(n);
		auto actual = interp::upper_bound(haystack.begin(), haystack.end(), n);
		auto expected = std::upper_bound(haystack.begin(), haystack.end(), n);
		CHECK_UNARY((actual == haystack.end()) == (expected == haystack.end()));
		if (actual != haystack.end() && expected != haystack.end())
			CHECK_EQ(*actual, *expected);
	}
	for (auto n : haystack) {
		CAPTURE(n);
		auto actual = interp::upper_bound(haystack.begin(), haystack.end(), n);
		auto expected = std::upper_bound(haystack.begin(), haystack.end(), n);
		CHECK_UNARY((actual == haystack.end()) == (expected == haystack.end()));
		if (actual != haystack.end() && expected != haystack.end())
			CHECK_EQ(*actual, *expected);
	}
}