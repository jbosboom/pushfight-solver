#ifndef PUSHFIGHT_BOARD_HPP_INCLUDED
#define PUSHFIGHT_BOARD_HPP_INCLUDED

namespace pushfight {

//TODO: we may want a bit-check here (high or second-high bit set) instead of values
constexpr inline unsigned int VOID = std::numeric_limits<unsigned int>::max();
constexpr inline unsigned int RAIL = std::numeric_limits<unsigned int>::max()-1;

enum Dir {LEFT, UP, RIGHT, DOWN};

class Board {
public:
	Board(std::string_view name, unsigned int squares, unsigned int anchorables,
			unsigned int pushers, unsigned int pawns, const unsigned int* topology,
			const unsigned int* placement_first, unsigned int placement_first_len,
			const unsigned int* placement_second, unsigned int placement_second_len)
	: name_(name), squares_(squares), anchorables_(anchorables), pushers_(pushers),
			pawns_(pawns), topology_(topology), placement_first_(placement_first),
			placement_second_(placement_second), placement_first_len_(placement_first_len),
			placement_second_len_(placement_second_len) {}

	unsigned int pushers() const {return pushers_;}
	unsigned int pawns() const {return pawns_;}
	unsigned int squares() const {return squares_;}

	unsigned int neighbor(unsigned int square, Dir dir) const {
		return topology_[square*4 + static_cast<unsigned int>(dir)];
	}
	std::array<unsigned int, 4> neighbors(unsigned int square) const {
		return {
			neighbor(square, LEFT),
			neighbor(square, UP),
			neighbor(square, RIGHT),
			neighbor(square, DOWN)
		};
	}
	std::uint32_t neighbors_mask(unsigned int square) const {
		std::uint32_t m = 0;
		for (unsigned int n : neighbors(square))
			if (n != VOID && n != RAIL)
				m |= (1 << n);
		return m;
	}
private:
	std::string_view name_;
	unsigned int squares_, anchorables_, pushers_, pawns_;
	const unsigned int* topology_;
	//TODO: should be some kind of array_view/span
	const unsigned int* placement_first_, *placement_second_;
	unsigned int placement_first_len_, placement_second_len_;
};

}

#endif /* PUSHFIGHT_BOARD_HPP_INCLUDED */

