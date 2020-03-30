#include "precompiled.hpp"
#include "state.hpp"
#include "board.hpp"
#include "board-defs.inc"

using namespace pushfight;

struct StateCounter : public StateVisitor {
	unsigned long began = 0, accepted = 0, ended = 0;
	void begin(const State& state) override {++began;}
	void accept(const State& state, char removed_piece) override {++accepted;}
	void end(const State& state) override {++ended;}
};

int main(int argc, char* argv[]) { //genbuild {'entrypoint': True, 'ldflags': ''}
	StateCounter counter;
	pushfight::enumerate_anchored_states(pushfight::traditional, counter);
	fmt::print("{} {} {}\n", counter.began, counter.accepted, counter.ended);
	return 0;
}
