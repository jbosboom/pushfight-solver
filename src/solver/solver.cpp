#include "precompiled.hpp"
#include "state.hpp"
#include "board.hpp"
#include "board-defs.inc"
#include "intervals.hpp"
#include "interpolation.hpp"
#include "stopwatch.hpp"
#include "util.hpp"
#include "hopscotch/hopscotch_set.h"
#include "hopscotch/hopscotch_map.h"
#include "ska_sort.hpp"
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
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

			//Disable readahead.
			madvise(sv, ssz, MADV_RANDOM);
			madvise(lv, lsz, MADV_RANDOM);

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
	tsl::hopscotch_set<unsigned long> already_processed;
	CompositeValueVisitor(const WinLossUnknownDatabase* wldb) : wldb(wldb) {}

	bool begin(const State& state) override {
		already_processed.clear();
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
		auto r = rank(state, traditional);
		if (!already_processed.insert(r).second)
			return true;
		auto value = wldb->query(r);
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

	if (std::fflush(sf)) {
		auto saved_errno = errno;
		throw std::runtime_error(fmt::format("error writing {}: failed to flush start file; error {} ({})",
				start_filename.c_str(), strerror(saved_errno), saved_errno));
	}
	if (std::fflush(lf)) {
		auto saved_errno = errno;
		throw std::runtime_error(fmt::format("error writing {}: failed to flush length file; error {} ({})",
				start_filename.c_str(), strerror(saved_errno), saved_errno));
	}

	if (fsync(fileno(sf))) {
		auto saved_errno = errno;
		throw std::runtime_error(fmt::format("error writing {}: failed to sync start file; error {} ({})",
				start_filename.c_str(), strerror(saved_errno), saved_errno));
	}
	if (fsync(fileno(lf))) {
		auto saved_errno = errno;
		throw std::runtime_error(fmt::format("error writing {}: failed to sync length file; error {} ({})",
				start_filename.c_str(), strerror(saved_errno), saved_errno));
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

struct splitmix64 {
	//from near bottom of https://nullprogram.com/blog/2018/07/31/
	std::size_t operator()(unsigned long x) const {
		x ^= x >> 30;
		x *= 0xbf58476d1ce4e5b9UL;
		x ^= x >> 27;
		x *= 0x94d049bb133111ebUL;
		x ^= x >> 31;
		return x;
	}
};

struct OutcountingPair {
	unsigned long r;
	std::uint16_t count;
} __attribute__((packed));

//This could be more general.
auto gallop_to_next_first(vector<pair<unsigned long, unsigned long>>::iterator first,
		vector<pair<unsigned long, unsigned long>>::iterator last) {
	auto old = first->first;
	auto size = static_cast<std::size_t>(std::distance(first, last));
	if (size < 32 || first[32].first != old) {
		while (first != last && first->first == old)
			++first;
		return first;
	}
	std::size_t gallop = 64;
	while (gallop < size && first[gallop].first == old)
		gallop *= 2;
	return std::upper_bound(first+(gallop/2), first+std::min(size, gallop), old,
			[](auto& a, auto& b){return a < b.first;});
}

struct OutcountingVisitor : public ForkableStateVisitor {
	vector<vector<pair<unsigned long, unsigned long>>> win_intervals, loss_intervals;
	unsigned long wins = 0, losses = 0, visited = 0;
	vector<pair<unsigned long, unsigned long>> succ_to_pred;
	tsl::hopscotch_map<unsigned long, std::uint16_t, splitmix64> outcounts;
	const WinLossUnknownDatabase* wldb;
	tsl::hopscotch_set<unsigned long, splitmix64> successors;
	unsigned long current_rank = 0;
	OutcountingVisitor(const WinLossUnknownDatabase* wldb) : wldb(wldb) {
		succ_to_pred.reserve(64*1024*1024);
	}

	bool begin(const State& state) override {
		current_rank = rank(state, traditional);
		if (wldb->query(current_rank) != UNKNOWN)
			return false;
		successors.clear();
		return true;
	}

	bool accept(const State& state, char removed_piece) override {
		if (removed_piece == 'E' || removed_piece == 'e')
			throw std::logic_error("visiting an inherently winning configuration?");
		if (removed_piece == 'A' || removed_piece == 'a')
			//can't rank this because we removed a piece, but it doesn't affect
			//whether this position is a win or a loss
			return true;
		auto r = rank(state, traditional);
		successors.insert(r);
		return true;
	}

	void end(const State& state) override {
		++visited;
		if (successors.size() > std::numeric_limits<std::uint16_t>::max())
			throw std::logic_error(fmt::format("too many successors for {}: {}", current_rank, successors.size()));
		outcounts[current_rank] = (std::uint16_t)successors.size();
		for (auto succ : successors)
			succ_to_pred.push_back({succ, current_rank});
		if (succ_to_pred.size() > succ_to_pred.capacity() - 25000)
			flush();
	}

	void flush() {
		vector<unsigned long> win_ranks;

		ska_sort(succ_to_pred.begin(), succ_to_pred.end(), [](auto& a){return a.first;});
		for (auto it = succ_to_pred.begin(); it != succ_to_pred.end();) {
			auto succ = it->first;
			auto value = wldb->query(succ);
			if (value == LOSS)
				do {
					win_ranks.push_back(it->second);
				} while ((++it) != succ_to_pred.end() && it->first == succ);
			else if (value == WIN)
				do {
					--outcounts[it->second];
				} while ((++it) != succ_to_pred.end() && it->first == succ);
			else
				it = gallop_to_next_first(it, succ_to_pred.end());
		}

		vector<unsigned long> loss_ranks;
		for (const std::pair<unsigned long, std::uint16_t>& p : outcounts)
			if (!p.second)
				loss_ranks.push_back(p.first);

		std::sort(win_ranks.begin(), win_ranks.end());
		win_ranks.erase(std::unique(win_ranks.begin(), win_ranks.end()), win_ranks.end());
		wins += win_ranks.size();
		win_intervals.push_back(maximal_intervals(win_ranks));
		std::sort(loss_ranks.begin(), loss_ranks.end());
		loss_ranks.erase(std::unique(loss_ranks.begin(), loss_ranks.end()), loss_ranks.end());
		losses += loss_ranks.size();
		loss_intervals.push_back(maximal_intervals(loss_ranks));

		succ_to_pred.clear();
		outcounts.clear();
	}

	std::unique_ptr<ForkableStateVisitor> clone() const override {
		return std::make_unique<OutcountingVisitor>(wldb);
	}

	void merge(std::unique_ptr<ForkableStateVisitor> p) override {
		OutcountingVisitor* other = dynamic_cast<OutcountingVisitor*>(p.get());
		if (!other->succ_to_pred.empty())
			other->flush();

		wins += other->wins;
		losses += other->losses;
		visited += other->visited;
		win_intervals.insert(win_intervals.end(), std::move_iterator(other->win_intervals.begin()), std::move_iterator(other->win_intervals.end()));
		loss_intervals.insert(loss_intervals.end(), std::move_iterator(other->loss_intervals.begin()), std::move_iterator(other->loss_intervals.end()));
	}
};

struct OpeningProcedureVisitor : public ForkableStateVisitor {
	const WinLossUnknownDatabase* wldb;
	tsl::hopscotch_set<unsigned long> already_processed;
	bool is_win = false; //set true if we ever push off an enemy piece
	bool is_loss = true; //set false if we ever make a push that doesn't push off an allied piece
	vector<State> winning_openings, losing_openings, drawn_openings;
	OpeningProcedureVisitor(const WinLossUnknownDatabase* wldb) : wldb(wldb) {}

	bool begin(const State& state) override {
		already_processed.clear();
		is_win = false;
		is_loss = true;
		return true;
	}

	bool accept(const State& state, char removed_piece) override {
		if (removed_piece == 'E' || removed_piece == 'e') {
			is_loss = false;
			is_win = true;
			return false;
		}
		if (removed_piece == 'A' || removed_piece == 'a')
			//can't rank this because we removed a piece, but it doesn't affect
			//whether this position is a win or a loss
			return true;
		auto r = rank(state, traditional);
		if (!already_processed.insert(r).second)
			return true;
		auto value = wldb->query(r);
		if (value != WIN)
			is_loss = false;
		if (value == LOSS) {
			is_win = true;
			return false;
		}
		return true;
	}

	void end(const State& state) override {
		if (is_win)
			winning_openings.push_back(state);
		else if (is_loss)
			losing_openings.push_back(state);
		else
			drawn_openings.push_back(state);
	}

	std::unique_ptr<ForkableStateVisitor> clone() const override {
		return std::make_unique<OpeningProcedureVisitor>(wldb);
	}

	void merge(std::unique_ptr<ForkableStateVisitor> p) override {
		OpeningProcedureVisitor* other = dynamic_cast<OpeningProcedureVisitor*>(p.get());

		winning_openings.insert(winning_openings.end(), other->winning_openings.begin(), other->winning_openings.end());
		losing_openings.insert(losing_openings.end(), other->losing_openings.begin(), other->losing_openings.end());
		drawn_openings.insert(drawn_openings.end(), other->drawn_openings.begin(), other->drawn_openings.end());
	}
};

void write_openings(std::filesystem::path data_dir, const OpeningProcedureVisitor& v) {
	std::filesystem::path opening_dir = data_dir / "openings";
	std::filesystem::create_directory(opening_dir);

	//As written, we'll only write a file if we have openings to write into it;
	//no empty files will be created.
	auto it = v.winning_openings.begin();
	while (it != v.winning_openings.end()) {
		State allied_halfstate = *it;
		std::filesystem::path opening_file = opening_dir /
				fmt::format("{}-{}-win.txt", allied_halfstate.allied_pushers, allied_halfstate.allied_pawns);
		FILE* f = std::fopen(opening_file.c_str(), "w+");
		while (it != v.winning_openings.end() &&
				it->allied_pushers == allied_halfstate.allied_pushers &&
				it->allied_pawns == allied_halfstate.allied_pawns) {
			fmt::print(f, "{} {}\n", it->enemy_pushers, it->enemy_pawns);
			++it;
		}
		std::fclose(f);
	}

	it = v.losing_openings.begin();
	while (it != v.losing_openings.end()) {
		State allied_halfstate = *it;
		std::filesystem::path opening_file = opening_dir /
				fmt::format("{}-{}-loss.txt", allied_halfstate.allied_pushers, allied_halfstate.allied_pawns);
		FILE* f = std::fopen(opening_file.c_str(), "w+");
		while (it != v.losing_openings.end() &&
				it->allied_pushers == allied_halfstate.allied_pushers &&
				it->allied_pawns == allied_halfstate.allied_pawns) {
			fmt::print(f, "{} {}\n", it->enemy_pushers, it->enemy_pawns);
			++it;
		}
		std::fclose(f);
	}

	it = v.drawn_openings.begin();
	while (it != v.drawn_openings.end()) {
		State allied_halfstate = *it;
		std::filesystem::path opening_file = opening_dir /
				fmt::format("{}-{}-draw.txt", allied_halfstate.allied_pushers, allied_halfstate.allied_pawns);
		FILE* f = std::fopen(opening_file.c_str(), "w+");
		while (it != v.drawn_openings.end() &&
				it->allied_pushers == allied_halfstate.allied_pushers &&
				it->allied_pawns == allied_halfstate.allied_pawns) {
			fmt::print(f, "{} {}\n", it->enemy_pushers, it->enemy_pawns);
			++it;
		}
		std::fclose(f);
	}
}

int main(int argc, char* argv[]) { //genbuild {'entrypoint': True, 'ldflags': ''}
	std::optional<unsigned int> generation, slice, subslice;
	std::optional<std::filesystem::path> data_dir;
	bool do_opening_procedure = false;
	for (int i = 1; i < argc; ++i)
		if (argv[i] == "--generation"sv)
			generation = from_string<unsigned int>(argv[++i]);
		else if (argv[i] == "--slice"sv)
			slice = from_string<unsigned int>(argv[++i]);
		else if (argv[i] == "--subslice"sv)
			subslice = from_string<unsigned int>(argv[++i]);
		else if (argv[i] == "--data-dir"sv || argv[i] == "--data"sv)
			data_dir = argv[++i];
		else if (argv[i] == "--opening"sv || argv[i] == "--openings"sv)
			do_opening_procedure = true;
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
	
	if (do_opening_procedure) {
		vector<std::filesystem::path> starts, lengths;
		vector<GameValue> values;
		for (unsigned int g = 0; ; ++g) {
			std::filesystem::path ws = *data_dir / fmt::format("win-{}.bin", g),
					wl = *data_dir / fmt::format("win-{}.len", g),
					ls = *data_dir / fmt::format("loss-{}.bin", g),
					ll = *data_dir / fmt::format("loss-{}.len", g);
			int present_count = 0;
			for (std::filesystem::path p : {ws, wl, ls, ll})
				if (!std::filesystem::is_regular_file(p))
					++present_count;
			if (present_count == 0)
				break;
			else if (present_count != 4)
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
		std::unique_ptr<WinLossUnknownDatabase> wldb;
		wldb = std::make_unique<WinLossUnknownDatabase>(std::move(starts), std::move(lengths), std::move(values));
		OpeningProcedureVisitor visitor(wldb.get());

		Stopwatch stopwatch = Stopwatch::process();
		opening_procedure(traditional, visitor);
		auto times = stopwatch.elapsed();

		fmt::print("Processed {} openings ({} won, {} lost, {} drawn).\n",
				visitor.winning_openings.size() + visitor.losing_openings.size() + visitor.drawn_openings.size(),
				visitor.winning_openings.size(), visitor.losing_openings.size(), visitor.drawn_openings.size());
		fmt::print("{} seconds ({}), {} cpu-seconds ({:.2f}), {:.2f} GiB, {} hard faults.\n",
				times.seconds(), times.hms(), times.cpuSeconds(), times.utilization(), times.highwaterGibibytes(), times.hardFaults());

		write_openings(*data_dir, visitor);
	} else if (*generation == 0) {
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
		visitor_ptr = std::make_unique<InherentValueVisitor>();
		IntervalVisitor& visitor = *visitor_ptr;

		Stopwatch stopwatch = Stopwatch::process();
		enumerate_anchored_states_threaded(*slice, traditional, *visitor_ptr);
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

		//We no longer merge in order, so we need to sort.
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
	} else {
		//Check if the final outputs exist.
		std::filesystem::path win_start_file = *data_dir / fmt::format("win-{}-{:02}-{:03}.bin", *generation, *slice, *subslice),
			win_length_file = *data_dir / fmt::format("win-{}-{:02}-{:03}.len", *generation, *slice, *subslice),
			loss_start_file = *data_dir / fmt::format("loss-{}-{:02}-{:03}.bin", *generation, *slice, *subslice),
			loss_length_file = *data_dir / fmt::format("loss-{}-{:02}-{:03}.len", *generation, *slice, *subslice);
		if (std::filesystem::exists(win_start_file) || std::filesystem::exists(loss_start_file) ||
				std::filesystem::exists(win_length_file) || std::filesystem::exists(loss_length_file)) {
			fmt::print(stderr, "win or loss files exist; not overwriting\n");
			return 1;
		}
		//We go ahead and overwrite these temp files.
		std::filesystem::path win_start_temp_file = *data_dir / "tmp" / fmt::format("win-{}-{:02}-{:03}.bin", *generation, *slice, *subslice),
			win_length_temp_file = *data_dir / "tmp" / fmt::format("win-{}-{:02}-{:03}.len", *generation, *slice, *subslice),
			loss_start_temp_file = *data_dir / "tmp" / fmt::format("loss-{}-{:02}-{:03}.bin", *generation, *slice, *subslice),
			loss_length_temp_file = *data_dir / "tmp" / fmt::format("loss-{}-{:02}-{:03}.len", *generation, *slice, *subslice);

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
		std::unique_ptr<WinLossUnknownDatabase> wldb;
		wldb = std::make_unique<WinLossUnknownDatabase>(std::move(starts), std::move(lengths), std::move(values));

		OutcountingVisitor visitor(wldb.get());
		Stopwatch stopwatch = Stopwatch::process();
		enumerate_anchored_states_subslice(*slice, *subslice, traditional, visitor);
		auto times = stopwatch.elapsed();

		fmt::print("Processed generation {} slice {} subslice {}.\n", *generation, *slice, *subslice);
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

		//We no longer merge in order, so we need to sort.
		std::sort(visitor.win_intervals.begin(), visitor.win_intervals.end());
		std::sort(visitor.loss_intervals.begin(), visitor.loss_intervals.end());

		write_intervals(std::move(visitor.win_intervals), win_start_temp_file, win_length_temp_file);
		write_intervals(std::move(visitor.loss_intervals), loss_start_temp_file, loss_length_temp_file);
		//We're screwed if we crash after partially but not completely renaming these...
		//I guess we can manually check when concatenating them that we have the
		//same number of each type of file.
		std::filesystem::rename(win_start_temp_file, win_start_file);
		std::filesystem::rename(win_length_temp_file, win_length_file);
		std::filesystem::rename(loss_start_temp_file, loss_start_file);
		std::filesystem::rename(loss_length_temp_file, loss_length_file);
	}

	
}
