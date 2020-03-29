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
	SharedWorkspace(const Board& b) : board(b) {
		neighbor_masks.reserve(b.squares());
		for (unsigned int s = 0; s < b.squares(); ++s)
			neighbor_masks.push_back(b.neighbors_mask(s));
	}
	const Board& board;
	std::vector<uint32_t> neighbor_masks;

	//TODO these should be in a Game object for non-Board rules customization
	unsigned int max_moves = 2;
	unsigned int allowable_moves_mask = 0b111; //0, 1 and 2 are all fine
};

struct ThreadWorkspace {
	std::vector<unsigned int> chain;
};

char remove_piece(State& state, unsigned int index) {
	if (state.allied_pushers & (1 << index)) {
		state.allied_pushers &= ~(1 << index);
		return 'A';
	} else if (state.allied_pawns & (1 << index)) {
		state.allied_pawns &= ~(1 << index);
		return 'a';
	} else if (state.enemy_pushers & (1 << index)) {
		state.enemy_pushers &= ~(1 << index);
		return 'E';
	} else if (state.enemy_pawns & (1 << index)) {
		state.enemy_pawns &= ~(1 << index);
		return 'e';
	} else
		throw std::logic_error("remove_piece: piece not present in any mask?");

}

void move_piece(State& state, unsigned int from, unsigned int to) {
	//TODO make this branchless by extracting one bit, clearing it, and storing it back
	if (state.allied_pushers & (1 << from)) {
		state.allied_pushers &= ~(1 << from);
		state.allied_pushers |= (1 << to);
	} else if (state.allied_pawns & (1 << from)) {
		state.allied_pawns &= ~(1 << from);
		state.allied_pawns |= (1 << to);
	} else if (state.enemy_pushers & (1 << from)) {
		state.enemy_pushers &= ~(1 << from);
		state.enemy_pushers |= (1 << to);
	} else if (state.enemy_pawns & (1 << from)) {
		state.enemy_pawns &= ~(1 << from);
		state.enemy_pawns |= (1 << to);
	} else
		throw std::logic_error("move_piece: piece not present in any mask?");
}

void do_all_pushes(const State source, const SharedWorkspace& swork, ThreadWorkspace& twork) {
	for (unsigned int start : set_bits_range(source.allied_pushers)) {
		for (Dir dir : {LEFT, UP, RIGHT, DOWN}) {
			twork.chain.clear();
			twork.chain.push_back(start);

			while (true) {
				unsigned int next = swork.board.neighbor(twork.chain.back(), dir);
				if (next == RAIL || source.anchored_pieces & (1 << next)) {
					//TODO: if we have a torus, do we allow a circular push?
					twork.chain.clear();
					break; //push not possible
				}
				if (next == VOID) {
					twork.chain.push_back(next);
					break;
				}
				if (source.blockers() & (1 << next))
					twork.chain.push_back(next);
				else
					break;
			}
			if (twork.chain.size() < 2)
				continue;

			State next = source;
			char removed_piece = ' ';
			if (twork.chain.back() == VOID) {
				if (twork.chain.size() == 2)
					continue; //can't push nothing into the void
				twork.chain.pop_back(); //pops void, leaves removed piece at back()
				removed_piece = remove_piece(next, twork.chain.back());
			}

			assert(twork.chain.size() >= 2);
			for (auto i = twork.chain.size()-1; i-- > 0;)
				move_piece(next, twork.chain[i], twork.chain[i+1]);
			//TODO: if we have multiple anchored pieces, how do we update?
			next.anchored_pieces = 1u << twork.chain[1]; //anchor where the pusher moved to

			//visitor.visit(next, removed_piece);
		}
	}
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

void next_states(const State source, unsigned int move_number, const SharedWorkspace& swork, ThreadWorkspace& twork) {
	//to allow detecting positions with no moves we need a begin/end pair on the acceptor.
	//if (move_number == 0)
	//	visitor.begin(source);
	if (swork.allowable_moves_mask & (1 << move_number))
		do_all_pushes(source, swork, twork);

	if (move_number < swork.max_moves) {
		//Make a move.
		for (unsigned int from : set_bits_range(source.allied_pushers)) {
			uint32_t all_to = connected_empty_space(from, source.blockers(), swork);
			for (unsigned int to : set_bits_range(all_to)) {
				State next = source;
				next.allied_pushers &= ~(1 << from);
				next.allied_pushers |= (1 << to);
				next_states(source, move_number+1, swork, twork);
			}
		}
		for (unsigned int from : set_bits_range(source.allied_pawns)) {
			uint32_t all_to = connected_empty_space(from, source.blockers(), swork);
			for (unsigned int to : set_bits_range(all_to)) {
				State next = source;
				next.allied_pawns &= ~(1 << from);
				next.allied_pawns |= (1 << to);
				next_states(source, move_number+1, swork, twork);
			}
		}
	}
	//if (move_number == 0)
	//	visitor.end(source);
}

} //namespace pushfight