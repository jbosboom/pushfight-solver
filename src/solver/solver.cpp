#include "precompiled.hpp"
#include "state.hpp"
#include "board.hpp"
#include "board-defs.inc"

using namespace pushfight;

struct InherentValueVisitor : public StateVisitor {
	unsigned long wins = 0, losses = 0, visited = 0;
	bool is_win = false; //set true if we ever push off an enemy piece
	bool is_loss = true; //set false if we ever make a push that doesn't push off an allied piece
	void begin(const State& state) override {
		is_win = false;
		is_loss = true;
	}
	void accept(const State& state, char removed_piece) override {
		if (removed_piece == 'E' || removed_piece == 'e')
			is_win = true;
		else if (removed_piece != 'A' && removed_piece != 'a')
			is_loss = false;
	}
	void end(const State& state) override {
		++visited;
		if (is_win)
			++wins;
		if (is_loss)
			++losses;
	}
};

int main(int argc, char* argv[]) { //genbuild {'entrypoint': True, 'ldflags': ''}
	InherentValueVisitor visitor;
	enumerate_anchored_states(traditional, visitor);
	fmt::print("{} {} {}\n", visitor.visited, visitor.wins, visitor.losses);
	return 0;
}
