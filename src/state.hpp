#ifndef STATE_HPP
#define STATE_HPP

#include "board.hpp"

namespace pushfight {

struct State {
	uint32_t enemy_pushers, enemy_pawns, allied_pushers, allied_pawns;
	uint32_t anchored_pieces; //usually one enemy pusher, but allow for variants
	uint32_t blockers() const {
		return enemy_pushers | enemy_pawns | allied_pushers | allied_pawns;
	}
};

struct StateVisitor {
	virtual void begin(const State& state) = 0;
	//return false to stop visiting
	virtual bool accept(const State& state, char removed_piece) = 0;
	virtual void end(const State& state) = 0;
};

void enumerate_anchored_states(const Board& board, StateVisitor& sv);

}//namespace pushfight

#endif /* STATE_HPP */

