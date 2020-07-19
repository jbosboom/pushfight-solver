#ifndef STATE_HPP
#define STATE_HPP

#include <memory>
#include "board.hpp"

namespace pushfight {

struct State {
	uint32_t enemy_pushers, enemy_pawns, allied_pushers, allied_pawns;
	uint32_t anchored_pieces; //usually one enemy pusher, but allow for variants
	uint32_t blockers() const {
		return enemy_pushers | enemy_pawns | allied_pushers | allied_pawns;
	}
};

unsigned long rank(State state, const Board& board);

struct StateVisitor {
	virtual bool begin(const State& state) = 0;
	//return false to stop visiting
	virtual bool accept(const State& state, char removed_piece) = 0;
	virtual void end(const State& state) = 0;
	virtual ~StateVisitor() = default;
};

struct ForkableStateVisitor : public StateVisitor {
	virtual std::unique_ptr<ForkableStateVisitor> clone() const = 0;
	virtual void merge(std::unique_ptr<ForkableStateVisitor> other) = 0;
};

void enumerate_anchored_states(const Board& board, StateVisitor& sv);
void enumerate_anchored_states_threaded(unsigned int slice, const Board& board, ForkableStateVisitor& sv);
void enumerate_anchored_states_subslice(unsigned int slice, unsigned int subslice, const Board& board, ForkableStateVisitor& sv);
void opening_procedure(const Board& board, ForkableStateVisitor& sv);

}//namespace pushfight

#endif /* STATE_HPP */

