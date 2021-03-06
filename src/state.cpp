#include "precompiled.hpp"
#include "state.hpp"
#include "board.hpp"
#include "set_bits_range.hpp"
#include <thread>
#include <future>

using std::uint32_t;
using std::vector;
using std::unique_ptr;

namespace pushfight {

//adapted from http://www.talkchess.com/forum3/viewtopic.php?t=48220&start=2
template<typename Integer>
Integer pext0(Integer val, Integer mask) {
	Integer res = 0;
	for (unsigned int i = 0; mask; ++i) {
		if (val & mask & -mask)
			res |= static_cast<Integer>(1) << i;
		mask &= mask - 1;
	}
	return res;
}

//adapted from http://www.talkchess.com/forum3/viewtopic.php?t=48220&start=1
template<typename Integer>
Integer pext1(Integer val, Integer mask) {
	Integer res = 0;
	int i = 0;

	val &= mask;
	while (val) {
		Integer p = val & -val;
		Integer q = mask & -mask;
		while (q != p) {
			i++;
			mask &= mask - 1;
			q = mask & -mask;
		}
		mask &= mask - 1;
		res |= static_cast<Integer> (1) << i;
		val &= val - 1;
	}

	return res;
}

//adapted from http://www.talkchess.com/forum3/viewtopic.php?t=48220&start=1#p723470
template<typename Integer>
Integer pext2(Integer val, Integer mask) {
	Integer res = 0;
	for (Integer src = val & mask; src; src &= src - 1) {
		res |= 1 << std::popcount(((src & -src) - 1) & mask);
	}
	return res;
}

unsigned long rank(State state, const Board& board) {
	if (state.allied_pawns & state.allied_pushers ||
			state.allied_pawns & state.enemy_pawns ||
			state.allied_pawns & state.enemy_pushers ||
			state.allied_pushers & state.enemy_pawns ||
			state.allied_pushers & state.enemy_pushers ||
			state.enemy_pawns & state.enemy_pushers) {
		throw std::logic_error(fmt::format("multiple pieces on square: {:b} {:b} {:b} {:b} {:b}",
				state.allied_pawns, state.allied_pushers, state.enemy_pawns, state.enemy_pushers, state.anchored_pieces));
	}
	if (std::popcount(state.blockers()) != 2 * (board.pawns() + board.pushers()))
		throw std::logic_error(fmt::format("too few pieces: {:b} {:b} {:b} {:b} {:b}",
				state.allied_pawns, state.allied_pushers, state.enemy_pawns, state.enemy_pushers, state.anchored_pieces));
	if (!(state.anchored_pieces & state.enemy_pushers))
		throw std::logic_error(fmt::format("enemy pusher not anchored: {:b} {:b} {:b} {:b} {:b}",
				state.allied_pawns, state.allied_pushers, state.enemy_pawns, state.enemy_pushers, state.anchored_pieces));

	unsigned long result = 0;
	//The bits that haven't been used yet.
	unsigned int pext_mask = (1 << board.squares()) - 1;
	//The number of squares that haven't been used; should be popcount(pext_mask).
	unsigned int squares = board.squares();

	//This is assuming there is exactly one anchored piece, an enemy pusher.
	int anchored_pusher_idx = std::countr_zero(state.anchored_pieces);
	result += anchored_pusher_idx;
	pext_mask &= ~state.anchored_pieces;
	--squares;
	//In the past, we special-cased the first multiply to be by board.anchorable_squares()
	//instead of squares.  That results in slices overlapping in rank numbers.

	auto enemy_pushers = state.enemy_pushers & ~state.anchored_pieces;
	enemy_pushers = pext2(enemy_pushers, pext_mask);
	pext_mask &= ~state.enemy_pushers;
	while (enemy_pushers) {
		int low_bit = std::countr_zero(enemy_pushers);
		result *= squares;
		result += low_bit;
		--squares;
		enemy_pushers >>= low_bit + 1;
	}

	auto enemy_pawns = pext2(state.enemy_pawns, pext_mask);
	pext_mask &= ~state.enemy_pawns;
	while (enemy_pawns) {
		int low_bit = std::countr_zero(enemy_pawns);
		result *= squares;
		result += low_bit;
		--squares;
		enemy_pawns >>= low_bit + 1;
	}

	auto allied_pushers = pext2(state.allied_pushers, pext_mask);
	pext_mask &= ~state.allied_pushers;
	while (allied_pushers) {
		int low_bit = std::countr_zero(allied_pushers);
		result *= squares;
		result += low_bit;
		--squares;
		allied_pushers >>= low_bit + 1;
	}

	auto allied_pawns = pext2(state.allied_pawns, pext_mask);
	pext_mask &= ~state.allied_pawns;
	while (allied_pawns) {
		int low_bit = std::countr_zero(allied_pawns);
		result *= squares;
		result += low_bit;
		--squares;
		allied_pawns >>= low_bit + 1;
	}

	return result;
}



struct SharedWorkspace {
	SharedWorkspace(const Board& b) : board(b), max_moves(board.max_moves()),
			allowable_moves_mask(board.allowed_moves_mask()) {
		assert(b.squares() <= neighbor_masks.size());
		for (unsigned int s = 0; s < b.squares(); ++s)
			neighbor_masks[s] = b.neighbors_mask(s);

		for (unsigned int i = 0; i < board.squares(); ++i) {
			board_choose_masks[1].push_back(1 << i);
			for (unsigned int j = i+1; j < board.squares(); ++j) {
				board_choose_masks[2].push_back((1 << i) | (1 << j));
				for (unsigned int k = j+1; k < board.squares(); ++k)
					board_choose_masks[3].push_back((1 << i) | (1 << j) | (1 << k));
			}
		}
		//The masks are already in the proper order for enumerating the states
		//in rank order, so don't sort them.

		for (Dir d : {LEFT, UP, RIGHT, DOWN}) {
			uint32_t v = 0, r = 0;
			for (unsigned int s = 0; s < b.squares(); ++s)
				if (b.neighbor(s, d) == VOID)
					v |= 1 << s;
				else if (b.neighbor(s, d) == RAIL)
					r |= 1 << s;
			adjacent_to_void[d] = v;
			adjacent_to_rail[d] = r;
		}

		placement0_mask = 0;
		for (const unsigned int* i = board.placement0_begin(); i != board.placement0_end(); ++i)
			placement0_mask |= 1 << *i;
		placement1_mask = 0;
		for (const unsigned int* i = board.placement1_begin(); i != board.placement1_end(); ++i)
			placement1_mask |= 1 << *i;
		//To rotate 180 degrees, find the x and y distances to the center of
		//mass and add their negations to the center of mass.
		double avg_row = 0.0, avg_col = 0.0;
		for (unsigned int s = 0; s < board.squares(); ++s) {
			auto coord = board.coord_for_square(s);
			avg_row += coord.first;
			avg_col += coord.second;
		}
		avg_row /= board.squares();
		avg_col /= board.squares();
		for (unsigned int s = 0; s < board.squares(); ++s) {
			auto coord = board.coord_for_square(s);
			double dr = coord.first - avg_row, dc = coord.second - avg_col;
			double nr = avg_row + (-dr), nc = avg_col + (-dc);
			canonicalize_180[s] = board.square_for_coord(std::round(nr), std::round(nc));
		}
	}

	State canonicalize(State state) const {
		if (state.anchored_pieces & placement1_mask) {
			State canonical = {};
			for (int s : set_bits_range(state.anchored_pieces))
				canonical.anchored_pieces |= 1 << canonicalize_180[s];
			for (int s : set_bits_range(state.enemy_pushers))
				canonical.enemy_pushers |= 1 << canonicalize_180[s];
			for (int s : set_bits_range(state.enemy_pawns))
				canonical.enemy_pawns |= 1 << canonicalize_180[s];
			for (int s : set_bits_range(state.allied_pushers))
				canonical.allied_pushers |= 1 << canonicalize_180[s];
			for (int s : set_bits_range(state.allied_pawns))
				canonical.allied_pawns |= 1 << canonicalize_180[s];
			state = canonical;
		}
		return state;
	}

	const Board& board;
	std::array<uint32_t, 26> neighbor_masks;
	//for each direction, a mask of the squares immediately adjacent to VOID or RAIL
	std::array<uint32_t, 4> adjacent_to_void, adjacent_to_rail;
	//board_choose_masks[i] is (squares choose i) masks for the position generator
	std::array<std::vector<uint32_t>, 4> board_choose_masks;
	unsigned int max_moves;
	unsigned int allowable_moves_mask;
	std::array<uint32_t, 26> canonicalize_180;
	unsigned int placement0_mask, placement1_mask;
};

char remove_piece(State& state, unsigned int index) {
	//Surprisingly, this is faster than a branchless solution that composes the
	//bits to form an index into an array of char.
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

template<typename Integer>
bool move_bit(Integer& x, unsigned int from, unsigned int to) {
	auto bit = (x & (1u << from)) >> from;
	x &= ~(1u << from);
	x |= (bit << to);
	return bit;
}

void move_piece(State& state, unsigned int from, unsigned int to) {
	//We don't know which mask it's in, but it's only in one, so it's safe to do
	//the move in all masks.  They are parallel dependency chains so this isn't
	//4x as expensive as knowing.
	move_bit(state.allied_pushers, from, to);
	move_bit(state.allied_pawns, from, to);
	move_bit(state.enemy_pushers, from, to);
	move_bit(state.enemy_pawns, from, to);
}

//returns true iff we should continue visiting
bool do_all_pushes(const State source, const SharedWorkspace& swork, StateVisitor& sv) {
	std::array<unsigned int, 10> chain;
	unsigned int chain_length = 0;
	for (unsigned int start : set_bits_range(source.allied_pushers)) {
		if (!(swork.neighbor_masks[start] & (source.blockers() & ~source.anchored_pieces)))
			continue; //no non-anchored pieces to push, in any direction
		for (Dir dir : {LEFT, UP, RIGHT, DOWN}) {
			chain_length = 0;
			chain[chain_length++] = start;
			State succ = source;
			char removed_piece = ' ';

			while (true) {
				unsigned int next = swork.board.neighbor(chain[chain_length-1], dir);
				if (swork.adjacent_to_void[dir] & (1 << chain[chain_length-1])) {
					removed_piece = remove_piece(succ, chain[chain_length-1]);
					break;
				} else if (swork.adjacent_to_rail[dir] & (1 << chain[chain_length-1])) {
					//TODO: if we have a torus, do we allow a circular push?
					chain_length = 0;
					break; //push not possible
				} else if (source.anchored_pieces & (1 << next)) {
					chain_length = 0;
					break;
				} else {
					chain[chain_length++] = next;
					if (!(source.blockers() & (1 << next))) //if we just added an empty square
						break;
				}
			}
			//"Pushing nothing" results in chain length 2 (the pusher and an
			//empty square).  This also handles impossible pushes (explicitly
			//set to 0).
			if (chain_length < 2)
				continue;

			for (auto i = chain_length-1; i-- > 0;)
				move_piece(succ, chain[i], chain[i+1]);
			//TODO: if we have multiple anchored pieces, how do we update?
			succ.anchored_pieces = 1u << chain[1]; //anchor where the pusher moved to
			std::swap(succ.allied_pushers, succ.enemy_pushers);
			std::swap(succ.allied_pawns, succ.enemy_pawns);

			succ = swork.canonicalize(succ);

			if (!sv.accept(succ, removed_piece))
				return false;
		}
	}
	return true;
}

uint32_t connected_empty_space(unsigned int source, uint32_t blockers, const SharedWorkspace& work) {
	uint32_t result = (work.neighbor_masks[source] & ~blockers) | (1 << source);
	uint32_t expanded = 1 << source;
//	fmt::print("{:b} {:b}\n", result, expanded);

	while (expanded != result) {
		uint32_t old_result = result, unexpanded = result & ~expanded;
		for (unsigned int bit : set_bits_range(unexpanded)) {
			assert(bit < work.board.squares());
			result |= work.neighbor_masks[bit] & ~blockers;
//			fmt::print("{} {:b} {:b}\n", bit, result, expanded);
		}
		expanded = old_result;
	}
	//avoid no-op move to where we started from
	result &= ~(1 << source);
	assert(!(result & blockers)); //shouldn't ever have blockers in empty space
	return result;
}

//returns true iff we should continue visiting
bool next_states(const State source, unsigned int move_number, const SharedWorkspace& swork, StateVisitor& sv) {
	bool returning_early = false;
	if (move_number == 0)
		if (!sv.begin(source))
			return false; //do not call sv.end

	if (swork.allowable_moves_mask & (1 << move_number))
		if (!do_all_pushes(source, swork, sv)) {
			returning_early = true;
			goto end;
		}

	if (move_number < swork.max_moves) {
		//Make a move.
		for (unsigned int from : set_bits_range(source.allied_pushers)) {
			uint32_t all_to = connected_empty_space(from, source.blockers(), swork);
			for (unsigned int to : set_bits_range(all_to)) {
				State next = source;
				next.allied_pushers &= ~(1 << from);
				next.allied_pushers |= (1 << to);
				if (!next_states(next, move_number+1, swork, sv)) {
					returning_early = true;
					goto end;
				}
			}
		}
		for (unsigned int from : set_bits_range(source.allied_pawns)) {
			uint32_t all_to = connected_empty_space(from, source.blockers(), swork);
			for (unsigned int to : set_bits_range(all_to)) {
				State next = source;
				next.allied_pawns &= ~(1 << from);
				next.allied_pawns |= (1 << to);
				if (!next_states(next, move_number+1, swork, sv)) {
					returning_early = true;
					goto end;
				}
			}
		}
	}

	end:
	if (move_number == 0)
		sv.end(source);
	return !returning_early;
}



void enumerate_anchored_states(const Board& board, StateVisitor& sv) {
	SharedWorkspace swork(board);
	unsigned long count = 0;
	for (unsigned int p = 0; p < swork.board.anchorable_squares(); ++p) {
		State state = {};
		state.enemy_pushers = 1 << p;
		state.anchored_pieces = state.enemy_pushers;

		for (unsigned int epu_mask : swork.board_choose_masks[swork.board.pushers() - 1]) {
			if (epu_mask & state.blockers()) continue;
			state.enemy_pushers |= epu_mask;
			assert(std::popcount(state.enemy_pushers) == swork.board.pushers());

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
						next_states(state, 0, swork, sv);
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

void enumerate_anchored_states_threaded(unsigned int slice, const Board& board, ForkableStateVisitor& sv) {
	assert(slice < board.anchorable_squares());
	SharedWorkspace swork(board);
	State base_state = {};
	base_state.enemy_pushers = 1 << slice;
	base_state.anchored_pieces = base_state.enemy_pushers;

	auto work_function = [&](std::size_t index) -> unique_ptr<ForkableStateVisitor> {
		unsigned int epu_mask = swork.board_choose_masks[swork.board.pushers() - 1][index];
		if (epu_mask & base_state.blockers()) return nullptr;
		unique_ptr<ForkableStateVisitor> result = sv.clone();
		State state = base_state;
		state.enemy_pushers |= epu_mask;
		assert(std::popcount(state.enemy_pushers) == swork.board.pushers());

		for (unsigned int epa_mask : swork.board_choose_masks[swork.board.pawns()]) {
			if (epa_mask & state.blockers()) continue;
			state.enemy_pawns = epa_mask;

			for (unsigned int apu_mask : swork.board_choose_masks[swork.board.pushers()]) {
				if (apu_mask & state.blockers()) continue;
				state.allied_pushers = apu_mask;

				for (unsigned int apa_mask : swork.board_choose_masks[swork.board.pawns()]) {
					if (apa_mask & state.blockers()) continue;
					state.allied_pawns = apa_mask;
					next_states(state, 0, swork, *result);
					state.allied_pawns = 0;
				}

				state.allied_pushers = 0;
			}

			state.enemy_pawns = 0;
		}
		return result;
	};

	auto task_count = swork.board_choose_masks[swork.board.pushers() - 1].size();
	std::mutex merge_mutex;
	std::atomic<std::size_t> index_dispenser(0);
	vector<std::future<void>> futures;
	std::size_t num_threads = std::thread::hardware_concurrency();
	for (std::size_t i = 0; i < num_threads && i < task_count; ++i)
		futures.push_back(std::async(std::launch::async, [&]() {
			for (std::size_t index = index_dispenser++; index < task_count; index = index_dispenser++) {
				auto result = work_function(index);
				if (result) {
					std::lock_guard lock(merge_mutex);
					sv.merge(std::move(result));
				}
			}
		}));
	for (std::size_t i = 0; i < futures.size(); ++i) {
		futures[i].wait();
		futures[i].get(); //rethrow any exception from the thread
	}
}

void enumerate_anchored_states_subslice(unsigned int slice, unsigned int subslice, const Board& board, ForkableStateVisitor& sv) {
	assert(slice < board.anchorable_squares());
	SharedWorkspace swork(board);
	State base_state = {};
	base_state.enemy_pushers = 1 << slice;
	base_state.anchored_pieces = base_state.enemy_pushers;

	auto work_function = [&](std::size_t index) -> unique_ptr<ForkableStateVisitor> {
		unsigned int epu_mask = swork.board_choose_masks[swork.board.pushers() - 1][index];
		if (epu_mask & base_state.blockers()) return nullptr;
		unique_ptr<ForkableStateVisitor> result = sv.clone();
		State state = base_state;
		state.enemy_pushers |= epu_mask;
		assert(std::popcount(state.enemy_pushers) == swork.board.pushers());

		for (unsigned int epa_mask : swork.board_choose_masks[swork.board.pawns()]) {
			if (epa_mask & state.blockers()) continue;
			state.enemy_pawns = epa_mask;

			for (unsigned int apu_mask : swork.board_choose_masks[swork.board.pushers()]) {
				if (apu_mask & state.blockers()) continue;
				state.allied_pushers = apu_mask;

				for (unsigned int apa_mask : swork.board_choose_masks[swork.board.pawns()]) {
					if (apa_mask & state.blockers()) continue;
					state.allied_pawns = apa_mask;
					next_states(state, 0, swork, *result);
					state.allied_pawns = 0;
				}

				state.allied_pushers = 0;
			}

			state.enemy_pawns = 0;
		}
		return result;
	};
	auto result = work_function(subslice);
	if (result)
		sv.merge(std::move(result));
}

void opening_procedure(const Board& board, ForkableStateVisitor& sv) {
	SharedWorkspace swork(board);
	vector<State> allied_halfstates, enemy_halfstates;
	{
		State state = {};
		for (unsigned int pu_mask : swork.board_choose_masks[swork.board.pushers()]) {
			state.allied_pushers = state.enemy_pushers = pu_mask;
			for (unsigned int pa_mask : swork.board_choose_masks[swork.board.pawns()]) {
				if (pa_mask & pu_mask) continue;
				state.allied_pawns = state.enemy_pawns = pa_mask;

				if ((state.blockers() & swork.placement0_mask) == state.blockers())
					allied_halfstates.push_back(state);
				if ((state.blockers() & swork.placement1_mask) == state.blockers())
					enemy_halfstates.push_back(state);
			}
		}
	}

	auto work_function = [&](std::size_t index) -> unique_ptr<ForkableStateVisitor> {
		unique_ptr<ForkableStateVisitor> result = sv.clone();
		State allied_halfstate = allied_halfstates[index];
		for (State enemy_halfstate : enemy_halfstates) {
			State state = allied_halfstate;
			state.enemy_pushers = enemy_halfstate.enemy_pushers;
			state.enemy_pawns = enemy_halfstate.enemy_pawns;
			next_states(state, 0, swork, *result);
		}
		return result;
	};

	auto task_count = allied_halfstates.size();
	std::mutex merge_mutex;
	std::atomic<std::size_t> index_dispenser(0);
	vector<std::future<void>> futures;
	std::size_t num_threads = std::thread::hardware_concurrency();
	for (std::size_t i = 0; i < num_threads && i < task_count; ++i)
		futures.push_back(std::async(std::launch::async, [&]() {
			for (std::size_t index = index_dispenser++; index < task_count; index = index_dispenser++) {
				auto result = work_function(index);
				if (result) {
					std::lock_guard lock(merge_mutex);
					sv.merge(std::move(result));
				}
			}
		}));
	for (std::size_t i = 0; i < futures.size(); ++i) {
		futures[i].wait();
		futures[i].get(); //rethrow any exception from the thread
	}
}

} //namespace pushfight