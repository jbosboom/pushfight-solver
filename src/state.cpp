#include "precompiled.hpp"
#include "board.hpp"
#include "set_bits_range.hpp"

using std::uint32_t;

namespace pushfight {

struct State {
	uint32_t enemy_pushers, enemy_pawns, allied_pushers, allied_pawns;
	uint32_t anchored_pieces; //usually one enemy pusher, but allow for variants
	uint32_t blockers() const {
		return enemy_pushers | enemy_pawns | allied_pushers | allied_pawns;
	}
};

struct SharedWorkspace {
	SharedWorkspace(const Board& b) {
		neighbor_masks.reserve(b.squares());
		for (unsigned int s = 0; s < b.squares(); ++s)
			neighbor_masks.push_back(b.neighbors_mask(s));
	}
	std::vector<uint32_t> neighbor_masks;

	//TODO these should be in a Game object for non-Board rules customization
	unsigned int max_moves = 2;
	unsigned int allowable_moves_mask = 0b111; //0, 1 and 2 are all fine
};

void do_all_pushes(State source, const SharedWorkspace& work) {
	//TODO implement pushing
}

uint32_t connected_empty_space(unsigned int source, uint32_t blockers, const SharedWorkspace& work) {
	uint32_t result = work.neighbor_masks[source] & blockers;
	uint32_t expanded = 1 << source;

	while (expanded != result)
		for (unsigned int bit : set_bits_range(result & ~expanded)) {
			result |= work.neighbor_masks[bit] & blockers;
			expanded |= 1 << bit;
		}

	//avoid no-op move to where we started from
	result &= ~(1 << source);
	return result;
}

void next_states(State source, unsigned int move_number, const SharedWorkspace& work) {
	if (work.allowable_moves_mask & (1 << move_number))
		do_all_pushes(source, work);

	if (move_number < work.max_moves) {
		//Make a move.
		for (unsigned int from : set_bits_range(source.allied_pushers)) {
			uint32_t all_to = connected_empty_space(from, source.blockers(), work);
			for (unsigned int to : set_bits_range(all_to)) {
				State next = source;
				next.allied_pushers &= ~(1 << from);
				next.allied_pushers |= (1 << to);
				next_states(source, move_number+1, work);
			}
		}
		for (unsigned int from : set_bits_range(source.allied_pawns)) {
			uint32_t all_to = connected_empty_space(from, source.blockers(), work);
			for (unsigned int to : set_bits_range(all_to)) {
				State next = source;
				next.allied_pawns &= ~(1 << from);
				next.allied_pawns |= (1 << to);
				next_states(source, move_number+1, work);
			}
		}
	}
}

} //namespace pushfight