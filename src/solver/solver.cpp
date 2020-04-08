#include "precompiled.hpp"
#include "state.hpp"
#include "board.hpp"
#include "board-defs.inc"
#include "intervals.hpp"
#include "util.hpp"
#include <filesystem>

using namespace pushfight;
using std::vector;
using std::pair;
using namespace std::literals::string_view_literals;

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

void write_intervals(vector<vector<pair<unsigned long, unsigned long>>>&& intervals, std::filesystem::path filename) {
	FILE* f = std::fopen(filename.c_str(), "w+");
	for (vector<pair<unsigned long, unsigned long>> v : intervals) {
		std::size_t written = std::fwrite(v.data(), v.size() * sizeof(v.front()), 1, f);
		if (written != 1) {
			auto saved_errno = errno;
			throw std::runtime_error(fmt::format("error writing {}: failed to write some intervals; error {} ({})",
					filename.c_str(), strerror(saved_errno), saved_errno));
		}
		vector<pair<unsigned long, unsigned long>> free_memory(std::move(v));
	}
	if (std::fclose(f)) {
		auto saved_errno = errno;
		throw std::runtime_error(fmt::format("error writing {}: failed to close?; error {} ({})",
				filename.c_str(), strerror(saved_errno), saved_errno));
	}
}

int main(int argc, char* argv[]) { //genbuild {'entrypoint': True, 'ldflags': ''}
	std::optional<unsigned int> generation, slice;
	std::optional<std::filesystem::path> data_dir;
	for (int i = 1; i < argc; ++i)
		if (argv[i] == "--generation"sv)
			generation = from_string<unsigned int>(argv[++i]);
		else if (argv[i] == "--slice"sv)
			slice = from_string<unsigned int>(argv[++i]);
		else if (argv[i] == "--data-dir"sv || argv[i] == "--data"sv)
			data_dir = argv[++i];
		else {
			fmt::print(stderr, "unknown option: {}\n", argv[i]);
			return 1;
		}
	if (!generation || !slice || !data_dir) {
		fmt::print(stderr, "required options not passed\n");
		return 1;
	}
	if (!std::filesystem::is_directory(*data_dir)) {
		fmt::print(stderr, "data dir not a directory (or does not exist)\n");
		return 1;
	}


	if (*generation == 0) {
		std::filesystem::path win_file = *data_dir / fmt::format("win-{}-{}.bin", *generation, *slice),
				loss_file = *data_dir / fmt::format("loss-{}-{}.bin", *generation, *slice);
		if (std::filesystem::exists(win_file) || std::filesystem::exists(loss_file)) {
			fmt::print(stderr, "win or loss file exists; not overwriting\n");
			return 1;
		}

		InherentValueVisitor visitor;
		enumerate_anchored_states_threaded(0, traditional, visitor);
		fmt::print("Visited {} states, found {} wins ({:.3f}) and {} losses ({:.3f}), total {} ({:.3f}) resolved.\n",
				visitor.visited,
				visitor.wins, (double)visitor.wins / (double)visitor.visited,
				visitor.losses, (double)visitor.losses / (double)visitor.visited,
				visitor.wins + visitor.losses, (double)(visitor.wins + visitor.losses) / (double)visitor.visited);
		unsigned long total_win_intervals = 0, total_loss_intervals = 0;
		for (auto& v : visitor.win_intervals)
			total_win_intervals += v.size();
		for (auto& v : visitor.loss_intervals)
			total_loss_intervals += v.size();
		fmt::print("{} win intervals ({:.5f}) and {} loss intervals ({:.5f}).\n",
				total_win_intervals, (double)visitor.wins / (double)total_win_intervals,
				total_loss_intervals, (double)visitor.losses / (double)total_loss_intervals);

		//They should already be sorted, but be doubly-sure.
		std::sort(visitor.win_intervals.begin(), visitor.win_intervals.end());
		std::sort(visitor.loss_intervals.begin(), visitor.loss_intervals.end());

		write_intervals(std::move(visitor.win_intervals), win_file);
		write_intervals(std::move(visitor.loss_intervals), loss_file);
		return 0;
	} else {
		fmt::print("TODO implement later generations");
		return 1;
	}
}
