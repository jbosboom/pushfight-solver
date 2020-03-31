#include "precompiled.hpp"
#include "state.hpp"
#include "board.hpp"
#include "set_bits_range.hpp"

using std::uint32_t;

namespace pushfight {

struct SharedWorkspace {
	SharedWorkspace(const Board& b) : board(b) {
		neighbor_masks.reserve(b.squares());
		for (unsigned int s = 0; s < b.squares(); ++s)
			neighbor_masks.push_back(b.neighbors_mask(s));

		for (unsigned int i = 0; i < board.squares(); ++i) {
			board_choose_masks[1].push_back(1 << i);
			for (unsigned int j = i+1; j < board.squares(); ++j) {
				board_choose_masks[2].push_back((1 << i) | (1 << j));
				for (unsigned int k = j+1; k < board.squares(); ++k)
					board_choose_masks[3].push_back((1 << i) | (1 << j) | (1 << k));
			}
		}
	}
	const Board& board;
	std::vector<uint32_t> neighbor_masks;
	//board_choose_masks[i] is (squares choose i) masks for the position generator
	std::array<std::vector<uint32_t>, 4> board_choose_masks;

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

void do_all_pushes(const State source, const SharedWorkspace& swork, ThreadWorkspace& twork, StateVisitor& sv) {
	for (unsigned int start : set_bits_range(source.allied_pushers)) {
		if (!(swork.neighbor_masks[start] & (source.blockers() & ~source.anchored_pieces)))
			continue; //no non-anchored pieces to push, in any direction
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
			std::swap(next.allied_pushers, next.enemy_pushers);
			std::swap(next.allied_pawns, next.enemy_pawns);

			sv.accept(next, removed_piece);
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

void next_states(const State source, unsigned int move_number, const SharedWorkspace& swork, ThreadWorkspace& twork, StateVisitor& sv) {
	if (move_number == 0)
		sv.begin(source);

	if (swork.allowable_moves_mask & (1 << move_number))
		do_all_pushes(source, swork, twork, sv);

	if (move_number < swork.max_moves) {
		//Make a move.
		for (unsigned int from : set_bits_range(source.allied_pushers)) {
			uint32_t all_to = connected_empty_space(from, source.blockers(), swork);
			for (unsigned int to : set_bits_range(all_to)) {
				State next = source;
				next.allied_pushers &= ~(1 << from);
				next.allied_pushers |= (1 << to);
				next_states(source, move_number+1, swork, twork, sv);
			}
		}
		for (unsigned int from : set_bits_range(source.allied_pawns)) {
			uint32_t all_to = connected_empty_space(from, source.blockers(), swork);
			for (unsigned int to : set_bits_range(all_to)) {
				State next = source;
				next.allied_pawns &= ~(1 << from);
				next.allied_pawns |= (1 << to);
				next_states(source, move_number+1, swork, twork, sv);
			}
		}
	}

	if (move_number == 0)
		sv.end(source);
}



void enumerate_anchored_states(const Board& board, StateVisitor& sv) {
	SharedWorkspace swork(board);
	ThreadWorkspace twork;
	unsigned long count = 0;
//	for (unsigned int p = 0; p < swork.board.anchorable_squares(); ++p) {
	for (unsigned int p = 0; p < 1; ++p) {
		State state = {};
		state.enemy_pushers = 1 << p;
		state.anchored_pieces = state.enemy_pushers;

		for (unsigned int epu_mask : swork.board_choose_masks[swork.board.pushers() - 1]) {
			if (epu_mask & state.blockers()) continue;
			state.enemy_pushers |= epu_mask;
			assert(std::popcount(state.enemy_pushers) == swork.board.pushers());
			fmt::print("{}\n", epu_mask);

			for (unsigned int epa_mask : swork.board_choose_masks[swork.board.pawns()]) {
				if (epa_mask & state.blockers()) continue;
				state.enemy_pawns = epa_mask;

				for (unsigned int apu_mask : swork.board_choose_masks[swork.board.pushers()]) {
					if (apu_mask & state.blockers()) continue;
					state.allied_pushers = apu_mask;

					for (unsigned int apa_mask : swork.board_choose_masks[swork.board.pawns()]) {
						if (apa_mask & state.blockers()) continue;
						state.allied_pawns = apa_mask;
						++count;
						next_states(state, 0, swork, twork, sv);
						state.allied_pawns = 0;
					}

					state.allied_pushers = 0;
				}

				state.enemy_pawns = 0;
			}

			state.enemy_pushers &= ~epu_mask;
		}

		state.enemy_pushers = 0;
		state.anchored_pieces = 0;
	}
	fmt::print("{}\n", count);
}

} //namespace pushfight