#include "precompiled.hpp"
#include "state.hpp"
#include "board.hpp"
#include "board-defs.inc"
#include "intervals.hpp"

using namespace pushfight;
using std::vector;
using std::pair;

/**
 * Computes the inherent value of a position (positions that are immediate wins
 * or losses, rather than positions they lead to).
 */
struct InherentValueVisitor : public ForkableStateVisitor {
	unsigned long wins = 0, losses = 0, visited = 0;
	bool is_win = false; //set true if we ever push off an enemy piece
	bool is_loss = true; //set false if we ever make a push that doesn't push off an allied piece
	vector<unsigned long> win_ranks, loss_ranks;
	vector<vector<pair<unsigned long, unsigned long>>> win_intervals, loss_intervals;
	void begin(const State& state) override {
		is_win = false;
		is_loss = true;
	}
	bool accept(const State& state, char removed_piece) override {
		if (removed_piece == 'E' || removed_piece == 'e') {
			is_win = true;
			return false;
		} else if (removed_piece != 'A' && removed_piece != 'a')
			is_loss = false;
		return true;
	}
	void end(const State& state) override {
		++visited;
		if (is_win) {
			++wins;
			//TODO: board should be passed in from elsewhere
			auto r = rank(state, traditional);
			if (win_ranks.size() * sizeof(win_ranks.front()) >= 16*1024*1024 &&
					r != win_ranks.back() + 1) {
				win_intervals.push_back(maximal_intervals(win_ranks));
				win_ranks.clear();
			}
			win_ranks.push_back(r);
		} else if (is_loss) {
			++losses;
			auto r = rank(state, traditional);
			if (loss_ranks.size() * sizeof(loss_ranks.front()) >= 16*1024*1024 &&
					r != loss_ranks.back() + 1) {
				loss_intervals.push_back(maximal_intervals(loss_ranks));
				loss_ranks.clear();
			}
			loss_ranks.push_back(r);
		}
	}

	std::unique_ptr<ForkableStateVisitor> clone() const override {
		return std::make_unique<InherentValueVisitor>();
	}
	void merge(std::unique_ptr<ForkableStateVisitor> p) override {
		InherentValueVisitor& other = dynamic_cast<InherentValueVisitor&>(*p);
		//clean up any remainder
		if (win_ranks.size())
			win_intervals.push_back(maximal_intervals(win_ranks));
		if (loss_ranks.size())
			loss_intervals.push_back(maximal_intervals(loss_ranks));

		wins += other.wins;
		losses += other.losses;
		visited += other.visited;
		win_intervals.insert(win_intervals.end(), std::move_iterator(other.win_intervals.begin()), std::move_iterator(other.win_intervals.end()));
		loss_intervals.insert(loss_intervals.end(), std::move_iterator(other.loss_intervals.begin()), std::move_iterator(other.loss_intervals.end()));
	}
};

int main(int argc, char* argv[]) { //genbuild {'entrypoint': True, 'ldflags': ''}
	InherentValueVisitor visitor;
	enumerate_anchored_states_threaded(0, traditional, visitor);
	fmt::print("{} {} {}\n", visitor.visited, visitor.wins, visitor.losses);
	unsigned long total_win_intervals = 0, total_loss_intervals = 0;
	for (auto& v : visitor.win_intervals)
		total_win_intervals += v.size();
	for (auto& v : visitor.loss_intervals)
		total_loss_intervals += v.size();
	fmt::print("{} {}\n", total_win_intervals, total_loss_intervals);
	return 0;
}
