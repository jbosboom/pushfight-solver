#include "precompiled.hpp"
#include "state.hpp"
#include "board.hpp"
#include "board-defs.inc"
#include "intervals.hpp"
#include "stopwatch.hpp"
#include "util.hpp"
#include <filesystem>
#include <fcntl.h>
#include <sys/mman.h> //for mmap

using namespace pushfight;
using std::vector;
using std::pair;
using namespace std::literals::string_view_literals;

struct IntervalVisitor : public ForkableStateVisitor {
	unsigned long wins = 0, losses = 0, visited = 0;
	bool is_win = false; //set true if we ever push off an enemy piece
	bool is_loss = true; //set false if we ever make a push that doesn't push off an allied piece
	vector<unsigned long> win_ranks, loss_ranks;
	vector<vector<pair<unsigned long, unsigned long>>> win_intervals, loss_intervals;
	bool begin(const State& state) override {
		is_win = false;
		is_loss = true;
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

	void prepare_for_merge() {
		//clean up any remainder
		if (win_ranks.size())
			win_intervals.push_back(maximal_intervals(win_ranks));
		if (loss_ranks.size())
			loss_intervals.push_back(maximal_intervals(loss_ranks));
	}

	void merge(std::unique_ptr<ForkableStateVisitor> p) override {
		IntervalVisitor& other = dynamic_cast<IntervalVisitor&>(*p);
		other.prepare_for_merge();

		wins += other.wins;
		losses += other.losses;
		visited += other.visited;
		win_intervals.insert(win_intervals.end(), std::move_iterator(other.win_intervals.begin()), std::move_iterator(other.win_intervals.end()));
		loss_intervals.insert(loss_intervals.end(), std::move_iterator(other.loss_intervals.begin()), std::move_iterator(other.loss_intervals.end()));
	}
};

/**
 * Computes the inherent value of a position (positions that are immediate wins
 * or losses, rather than positions they lead to).
 */
struct InherentValueVisitor : public IntervalVisitor {
	bool accept(const State& state, char removed_piece) override {
		if (removed_piece == 'E' || removed_piece == 'e') {
			is_win = true;
			return false;
		} else if (removed_piece != 'A' && removed_piece != 'a')
			is_loss = false;
		return true;
	}
	std::unique_ptr<ForkableStateVisitor> clone() const override {
		return std::make_unique<InherentValueVisitor>();
	}
};

enum GameValue {WIN, LOSS, UNKNOWN};
struct WinLossUnknownDatabase {
	struct Data {
		pair<unsigned long*, unsigned long*> start;
		pair<std::uint8_t*, std::uint8_t*> length;
		GameValue v;
	};
	vector<Data> data;
	WinLossUnknownDatabase(vector<std::filesystem::path> starts, vector<std::filesystem::path> lengths, vector<GameValue> values) {
		if (starts.size() != lengths.size() || lengths.size() != values.size())
			throw std::logic_error("length mismatch in WinLossUnknownDatabase");
		for (std::size_t i = 0; i < starts.size(); ++i) {
			auto ssz = std::filesystem::file_size(starts[i]);
			auto lsz = std::filesystem::file_size(lengths[i]);
			if (ssz == 0 && lsz == 0) continue;
			if (ssz == 0 || lsz == 0)
				throw std::logic_error(fmt::format("empty/nonempty mismatch between {} and {}",
						starts[i].c_str(), lengths[i].c_str()));

			int fd = open(starts[i].c_str(), O_RDONLY);
			void* sv = mmap(nullptr, ssz, PROT_READ, MAP_SHARED_VALIDATE, fd, 0);
			close(fd);

			fd = open(lengths[i].c_str(), O_RDONLY);
			void* lv = mmap(nullptr, lsz, PROT_READ, MAP_SHARED_VALIDATE, fd, 0);
			close(fd);

			Data d;
			d.start.first = reinterpret_cast<unsigned long*>(sv);
			d.start.second = d.start.first + ssz / sizeof(unsigned long);
			d.length.first = reinterpret_cast<std::uint8_t*>(lv);
			d.length.second = d.length.first + lsz / sizeof(std::uint8_t);
			d.v = values[i];
			data.push_back(d);
		}
	}

	GameValue query(unsigned long r) const {
		for (Data d : data) {
			auto p = std::upper_bound(d.start.first, d.start.second, r);
			if (p == d.start.first) continue;
			--p;
			auto offset = std::distance(d.start.first, p);
			auto q = d.length.first;
			std::advance(q, offset);
			if (*p <= r && r < (*p + *q)) return d.v;
		}
		return UNKNOWN;
	}
};

struct CompositeValueVisitor : public IntervalVisitor {
	const WinLossUnknownDatabase* wldb;
	CompositeValueVisitor(const WinLossUnknownDatabase* wldb) : wldb(wldb) {}

	bool begin(const State& state) override {
		auto r = rank(state, traditional);
		if (wldb->query(r) != UNKNOWN)
			return false;
		return IntervalVisitor::begin(state);
	}

	bool accept(const State& state, char removed_piece) override {
		if (removed_piece == 'E' || removed_piece == 'e')
			throw std::logic_error("visiting an inherently winning configuration?");
		if (removed_piece == 'A' || removed_piece == 'a')
			//can't rank this because we removed a piece, but it doesn't affect
			//whether this position is a win or a loss
			return true;
		auto value = wldb->query(rank(state, traditional));
		if (value != WIN)
			is_loss = false;
		if (value == LOSS) {
			is_win = true;
			return false;
		}
		return true;
	}

	std::unique_ptr<ForkableStateVisitor> clone() const override {
		return std::make_unique<CompositeValueVisitor>(wldb);
	}
};

void write_intervals(vector<vector<pair<unsigned long, unsigned long>>>&& intervals,
		std::filesystem::path start_filename, std::filesystem::path length_filename) {
	FILE* sf = std::fopen(start_filename.c_str(), "w+"),
			*lf = std::fopen(length_filename.c_str(), "w+");
	for (vector<pair<unsigned long, unsigned long>> v : intervals) {
		for (pair<unsigned long, unsigned long> i : v) {
			//If an interval's size is greater than 255 we split it into multiple intervals.
			for (unsigned long start = i.first; start < i.second;) {
				unsigned long length = std::min(255ul, i.second - start);
				std::size_t written = std::fwrite(&start, sizeof(start), 1, sf);
				if (written != 1) {
					auto saved_errno = errno;
					throw std::runtime_error(fmt::format("error writing {}: failed to write start; error {} ({})",
							start_filename.c_str(), strerror(saved_errno), saved_errno));
				}
				written = std::fwrite(&length, sizeof(std::uint8_t), 1, lf);
				if (written != 1) {
					auto saved_errno = errno;
					throw std::runtime_error(fmt::format("error writing {}: failed to write length; error {} ({})",
							start_filename.c_str(), strerror(saved_errno), saved_errno));
				}
				start += length;
			}
		}
		vector<pair<unsigned long, unsigned long>> free_memory(std::move(v));
	}
	if (std::fclose(sf)) {
		auto saved_errno = errno;
		throw std::runtime_error(fmt::format("error writing {}: failed to close start file; error {} ({})",
				start_filename.c_str(), strerror(saved_errno), saved_errno));
	}
	if (std::fclose(lf)) {
		auto saved_errno = errno;
		throw std::runtime_error(fmt::format("error writing {}: failed to close length file; error {} ({})",
				start_filename.c_str(), strerror(saved_errno), saved_errno));
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
	std::filesystem::path win_start_file = *data_dir / fmt::format("win-{}-{:02}.bin", *generation, *slice),
			win_length_file = *data_dir / fmt::format("win-{}-{:02}.len", *generation, *slice),
			loss_start_file = *data_dir / fmt::format("loss-{}-{:02}.bin", *generation, *slice),
			loss_length_file = *data_dir / fmt::format("loss-{}-{:02}.len", *generation, *slice);
	if (std::filesystem::exists(win_start_file) || std::filesystem::exists(loss_start_file) ||
			std::filesystem::exists(win_length_file) || std::filesystem::exists(loss_length_file)) {
		fmt::print(stderr, "win or loss files exist; not overwriting\n");
		return 1;
	}

	std::unique_ptr<IntervalVisitor> visitor_ptr;
	std::unique_ptr<WinLossUnknownDatabase> wldb;
	if (*generation == 0)
		visitor_ptr = std::make_unique<InherentValueVisitor>();
	else {
		vector<std::filesystem::path> starts, lengths;
		vector<GameValue> values;
		for (unsigned int g = 0; g < *generation; ++g) {
			std::filesystem::path ws = *data_dir / fmt::format("win-{}.bin", g),
					wl = *data_dir / fmt::format("win-{}.len", g),
					ls = *data_dir / fmt::format("loss-{}.bin", g),
					ll = *data_dir / fmt::format("loss-{}.len", g);
			for (std::filesystem::path p : {ws, wl, ls, ll})
				if (!std::filesystem::is_regular_file(p))
					throw std::runtime_error(fmt::format("expected {} to exist", p.c_str()));
			starts.push_back(ws);
			lengths.push_back(wl);
			values.push_back(WIN);
			starts.push_back(ls);
			lengths.push_back(ll);
			values.push_back(LOSS);
		}
		wldb = std::make_unique<WinLossUnknownDatabase>(std::move(starts), std::move(lengths), std::move(values));
		visitor_ptr = std::make_unique<CompositeValueVisitor>(wldb.get());
	}

	IntervalVisitor& visitor = *visitor_ptr;
	Stopwatch stopwatch = Stopwatch::process();
	enumerate_anchored_states_threaded(*slice, traditional, visitor);
	auto times = stopwatch.elapsed();
	fmt::print("Processed generation {} slice {}.\n", *generation, *slice);
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
	fmt::print("{} seconds ({}), {} cpu-seconds ({:.2f}), {:.2f} GiB, {} hard faults.\n",
			times.seconds(), times.hms(), times.cpuSeconds(), times.utilization(), times.highwaterGibibytes(), times.hardFaults());

	//They should already be sorted, but be doubly-sure.
	std::sort(visitor.win_intervals.begin(), visitor.win_intervals.end());
	std::sort(visitor.loss_intervals.begin(), visitor.loss_intervals.end());
	//IntervalVisitor assumes a given visitor object will either be the parent
	//being merged into or an actual visitor, not both.  The parent shouldn't
	//have any singleton ranks of its own because it never visits.  Ideally we
	//would change the design of ForkableStateVisitor to not inherit from
	//StateVisitor, but in lieu of that, at least fail noisily.
	if (visitor.win_ranks.size() || visitor.loss_ranks.size())
		throw std::logic_error("unmerged singleton ranks?");

	write_intervals(std::move(visitor.win_intervals), win_start_file, win_length_file);
	write_intervals(std::move(visitor.loss_intervals), loss_start_file, loss_length_file);
	return 0;
}
