// reconstruct: rebuild the inbound order-entry stream from a Nasdaq TotalView-ITCH 5.0 file.
//
// ITCH shows the output of the matching engine. This tool turns it back into
// the requests that went in. The output file uses the same ITCH 5.0 framing
// and message layout as the input, so the same parser reads it. It contains
// only these message types:
//
//   R  stock directory, copied from the input for each output symbol
//   S  system event, copied from the input
//   H  stock trading action, copied from the input
//   A  add order: a limit order enters the book. Your engine must match it
//      when it crosses the book.
//   X  cancel part of a resting order
//   D  delete a resting order
//
// How the input maps to the output:
//   A, F   -> A with the ITCH order reference number.
//   X      -> X.
//   D      -> D.
//   U      -> D of the old order, then A of the new order.
//   E, C   -> fills of one aggressive order. All fills of one aggressive
//             order share one timestamp and one stock. The tool sums them
//             into one A on the opposite side of the resting orders. The
//             limit price is the worst resting price that the order hit.
//             If an A or F for the same stock, timestamp and side follows,
//             and its price reaches the fills, that message is the unfilled
//             remainder. The output A then uses that order reference number
//             and the total size. Otherwise the A gets a synthetic reference
//             number with the high bit set.
//   C at a price better than the display price of the resting order
//          -> X of the executed shares. Nasdaq "price to comply" orders show
//             one price and execute at another. A price-time book cannot
//             reproduce that fill, so the shares leave the book as a cancel.
//   P, Q, B -> ignored. Hidden orders never appear in the displayed book.
//   H      -> copied. During a halt or a pause Nasdaq accepts orders without
//             matching, so the displayed book can cross. The tool holds the
//             orders that enter during the halt. Fills of the reopen cross
//             reduce them silently, or become X/D for orders that rested
//             before the halt. When trading resumes and the book no longer
//             crosses, the tool writes the held orders as A messages in
//             priority order.
//
// The tool keeps its own price-time book. When a fill skips a resting order
// that had priority, the exchange did not let that order trade (self-trade
// prevention, for example). The tool writes a D for the skipped order before
// the aggressive A, so a price-time engine hits the same orders. A D or X at
// the same timestamp as a fill group is also written before the aggressive A.
//
// Usage:
//   ./reconstruct <itch file> [--out <file>] [--symbol SYM]... [--top N]
//                 [--verify] [--trace N] [--quiet]
//
// --symbol SYM  output only this symbol. Repeat the flag for more symbols.
// --top N       output the N symbols with the most add-order messages.
// --verify      replay the output through a reference price-time matcher and
//               compare its book with the displayed ITCH book.
// --trace N     with --verify, print the first N messages where the matcher
//               and ITCH differ.

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

static inline uint16_t be16(const uint8_t* p) { return uint16_t((uint16_t(p[0]) << 8) | p[1]); }
static inline uint32_t be32(const uint8_t* p) {
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
static inline uint64_t be48(const uint8_t* p) { return (uint64_t(be16(p)) << 32) | be32(p + 2); }
static inline uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

static inline void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
static inline void put32(uint8_t* p, uint32_t v) { put16(p, uint16_t(v >> 16)); put16(p + 2, uint16_t(v)); }
static inline void put48(uint8_t* p, uint64_t v) { put16(p, uint16_t(v >> 32)); put32(p + 2, uint32_t(v)); }
static inline void put64(uint8_t* p, uint64_t v) { put32(p, uint32_t(v >> 32)); put32(p + 4, uint32_t(v)); }

static constexpr uint64_t SYNTHETIC_BIT = 1ULL << 63;

// ---------------------------------------------------------------- price-time book
struct Node {
	uint64_t id;
	Node* prev;
	Node* next;
	uint32_t shares;
	uint32_t price;
	uint16_t locate;
	uint8_t side;
	uint8_t deferred; // entered during a halt, not yet written
};
struct Level {
	Node* head = nullptr;
	Node* tail = nullptr;
};
struct Sym {
	std::map<uint32_t, Level, std::greater<uint32_t>> bids;
	std::map<uint32_t, Level, std::less<uint32_t>> asks;
};

class Book {
	std::vector<Sym> syms_;
	std::unordered_map<uint64_t, Node*> index_;
	std::vector<Node*> free_;
	std::vector<std::unique_ptr<Node[]>> blocks_;

	Node* alloc() {
		if (free_.empty()) {
			const size_t n = 1 << 16;
			blocks_.emplace_back(new Node[n]);
			for (size_t i = 0; i < n; ++i) free_.push_back(&blocks_.back()[i]);
		}
		Node* p = free_.back();
		free_.pop_back();
		return p;
	}
	template <class Levels>
	void unlink(Levels& levels, Node* o) {
		auto it = levels.find(o->price);
		Level& lv = it->second;
		if (o->prev) o->prev->next = o->next; else lv.head = o->next;
		if (o->next) o->next->prev = o->prev; else lv.tail = o->prev;
		if (!lv.head) levels.erase(it);
	}
	template <class Levels>
	void append(Levels& levels, Node* o) {
		Level& lv = levels[o->price];
		// Nasdaq ranks orders by entry sequence, and the reference number
		// carries that sequence. An order can enter before it is displayed,
		// so with priority_by_id the node moves ahead of larger ids.
		Node* after = lv.tail;
		if (priority_by_id) while (after && after->id > o->id) after = after->prev;
		o->prev = after;
		o->next = after ? after->next : lv.head;
		if (o->next) o->next->prev = o; else lv.tail = o;
		if (after) after->next = o; else lv.head = o;
	}

public:
	bool priority_by_id = false;

	Book() : syms_(65536) { index_.reserve(1 << 24); }

	size_t size() const { return index_.size(); }
	Node* find(uint64_t id) {
		auto it = index_.find(id);
		return it == index_.end() ? nullptr : it->second;
	}
	const std::unordered_map<uint64_t, Node*>& all() const { return index_; }

	// place an order at the back of its level, no matching
	Node* rest(uint64_t id, uint16_t loc, uint8_t side, uint32_t shares, uint32_t price) {
		Node* o = alloc();
		o->id = id; o->shares = shares; o->price = price; o->locate = loc; o->side = side; o->deferred = 0;
		if (side == 'B') append(syms_[loc].bids, o); else append(syms_[loc].asks, o);
		index_[id] = o;
		return o;
	}
	void remove(Node* o) {
		if (o->side == 'B') unlink(syms_[o->locate].bids, o); else unlink(syms_[o->locate].asks, o);
		index_.erase(o->id);
		free_.push_back(o);
	}
	// reduce by `shares`, remove when empty
	void reduce(Node* o, uint32_t shares) {
		o->shares -= shares;
		if (o->shares == 0) remove(o);
	}
	uint32_t best(uint16_t loc, uint8_t side) {
		Sym& s = syms_[loc];
		if (side == 'B') return s.bids.empty() ? 0 : s.bids.begin()->first;
		return s.asks.empty() ? 0 : s.asks.begin()->first;
	}
	bool crossed(uint16_t loc) {
		uint32_t bb = best(loc, 'B'), ba = best(loc, 'S');
		return bb != 0 && ba != 0 && bb >= ba;
	}

	// Call fn(node) for every order on `side` of `loc` that has priority over
	// `target`, in price-time order. Stops at `target`. fn may remove the node.
	template <class Fn>
	void for_each_before(uint16_t loc, uint8_t side, Node* target, Fn fn) {
		Sym& s = syms_[loc];
		auto walk = [&](auto& levels) {
			for (auto it = levels.begin(); it != levels.end();) {
				uint32_t px = it->first;
				bool past_target = side == 'B' ? px < target->price : px > target->price;
				if (past_target) return;
				Node* o = it->second.head;
				++it; // fn may erase this level
				while (o) {
					if (o == target) return;
					Node* nx = o->next;
					fn(o);
					o = nx;
				}
			}
		};
		if (side == 'B') walk(s.bids); else walk(s.asks);
	}

	// price-time matching for an incoming order. Returns the matched shares.
	// fn(node, shares) is called for each fill.
	template <class Fn>
	uint32_t match(uint16_t loc, uint8_t side, uint32_t shares, uint32_t price, Fn fn) {
		Sym& s = syms_[loc];
		uint32_t left = shares;
		auto sweep = [&](auto& levels, auto crosses) {
			while (left > 0 && !levels.empty()) {
				auto it = levels.begin();
				if (!crosses(it->first)) return;
				Node* o = it->second.head;
				while (left > 0 && o) {
					Node* nx = o->next;
					uint32_t take = std::min(left, o->shares);
					left -= take;
					fn(o, take);
					reduce(o, take); // may erase the level
					o = nx;
				}
			}
		};
		if (side == 'B') sweep(s.asks, [&](uint32_t px) { return px <= price; });
		else sweep(s.bids, [&](uint32_t px) { return px >= price; });
		return shares - left;
	}
};

// ---------------------------------------------------------------- stats
struct Stats {
	uint64_t add_itch = 0;         // A and F that rested
	uint64_t add_replace = 0;      // U new orders
	uint64_t add_aggr_synth = 0;   // aggressive orders with a synthetic id
	uint64_t add_aggr_rem = 0;     // aggressive orders with a resting remainder
	uint64_t cancel_partial = 0;   // X
	uint64_t cancel_full = 0;      // D
	uint64_t cancel_replace = 0;   // U old orders
	uint64_t cancel_hidden_px = 0; // C fills at a hidden price, written as X
	uint64_t cancel_skipped = 0;   // orders a fill skipped, written as D
	uint64_t fill_shares = 0;      // E and C shares that became aggressive orders
	uint64_t hidden_px_shares = 0; // C shares written as cancels
	uint64_t fill_unknown_ref = 0; // E or C for an order the tool never saw
	uint64_t fill_over = 0;        // E or C with more shares than the order had
	uint64_t cancel_unknown_ref = 0;
	uint64_t cancel_over = 0;
	uint64_t replace_unknown_ref = 0;
	uint64_t group_split_by_cancel = 0; // a D or X hit an order inside an open group
	uint64_t add_held = 0;          // orders that entered during a halt
	uint64_t add_released = 0;      // held orders written after the reopen
	uint64_t held_fill_shares = 0;  // reopen cross shares on held orders
	uint64_t cancel_halt_cross = 0; // reopen cross fills on pre-halt orders, written as X/D
	uint64_t halts = 0;
	uint64_t skipped_then_filled = 0;   // a skipped order the exchange filled later (fill dropped)
	uint64_t skipped_then_cancelled = 0; // a skipped order the exchange cancelled later (expected)
	uint64_t skipped_better_level = 0;  // skipped orders at a better price than the fill
	uint64_t messages = 0;
};

// ---------------------------------------------------------------- verify engine
struct RefEngine {
	Book book;
	uint64_t matched_shares = 0;
	uint64_t cancel_missing = 0;
	uint64_t cancel_over = 0;
	uint64_t synthetic_rested = 0;
	uint64_t duplicate_id = 0;

	uint32_t add(uint64_t id, uint16_t loc, uint8_t side, uint32_t shares, uint32_t price) {
		if (book.find(id)) duplicate_id++;
		uint32_t got = book.match(loc, side, shares, price, [&](Node*, uint32_t take) { matched_shares += take; });
		if (got < shares) {
			if (id & SYNTHETIC_BIT) synthetic_rested++;
			book.rest(id, loc, side, shares - got, price);
		}
		return got;
	}
	bool cancel(uint64_t id, uint32_t shares) {
		Node* o = book.find(id);
		if (!o) { cancel_missing++; return false; }
		if (shares > o->shares) { cancel_over++; book.remove(o); return false; }
		book.reduce(o, shares);
		return true;
	}
};

// ---------------------------------------------------------------- ITCH writer
struct Writer {
	FILE* f = nullptr;
	std::vector<uint8_t> buf;
	uint64_t count = 0;
	std::vector<std::array<char, 8>> stock; // by locate
	RefEngine* ref = nullptr;
	int trace_left = 0;
	uint64_t mismatches = 0;
	std::vector<uint64_t> mismatch_by_loc = std::vector<uint64_t>(65536, 0);

	Writer() { stock.resize(65536); buf.reserve(1 << 20); }

	void open(const std::string& path) {
		f = fopen(path.c_str(), "wb");
		if (!f) { perror("open output"); exit(1); }
	}
	void flush() {
		if (!buf.empty()) fwrite(buf.data(), 1, buf.size(), f);
		buf.clear();
	}
	void close() { flush(); fclose(f); }

	// reserve one framed message and return a pointer to its body
	uint8_t* begin(uint16_t len, char type, uint16_t loc, uint64_t ts) {
		if (buf.size() + len + 2 > buf.capacity()) flush();
		size_t at = buf.size();
		buf.resize(at + 2 + len);
		uint8_t* p = buf.data() + at;
		put16(p, len);
		p += 2;
		p[0] = uint8_t(type);
		put16(p + 1, loc);
		put16(p + 3, 0);
		put48(p + 5, ts);
		count++;
		return p;
	}
	void copy(const uint8_t* m, uint16_t len) {
		if (buf.size() + len + 2 > buf.capacity()) flush();
		size_t at = buf.size();
		buf.resize(at + 2 + len);
		put16(buf.data() + at, len);
		memcpy(buf.data() + at + 2, m, len);
		count++;
	}
	void add(uint64_t ts, uint16_t loc, uint64_t id, uint8_t side, uint32_t shares, uint32_t price, uint32_t expected_match) {
		uint8_t* p = begin(36, 'A', loc, ts);
		put64(p + 11, id);
		p[19] = side;
		put32(p + 20, shares);
		memcpy(p + 24, stock[loc].data(), 8);
		put32(p + 32, price);
		if (!ref) return;
		uint32_t best = ref->book.best(loc, side == 'B' ? 'S' : 'B');
		uint32_t got = ref->add(id, loc, side, shares, price);
		if (got != expected_match) {
			mismatches++;
			mismatch_by_loc[loc]++;
			if (trace_left > 0) {
				trace_left--;
				fprintf(stderr, "MISMATCH ts=%llu ADD id=%llu%s side=%c shares=%u px=%u : matched %u, expected %u, best opposite %u\n",
				        (unsigned long long)ts, (unsigned long long)(id & ~SYNTHETIC_BIT),
				        (id & SYNTHETIC_BIT) ? "(synthetic)" : "", side, shares, price, got, expected_match, best);
			}
		}
	}
	void cancel(uint64_t ts, uint16_t loc, uint64_t id, uint32_t shares, bool full) {
		if (full) {
			uint8_t* p = begin(19, 'D', loc, ts);
			put64(p + 11, id);
		} else {
			uint8_t* p = begin(23, 'X', loc, ts);
			put64(p + 11, id);
			put32(p + 19, shares);
		}
		if (!ref) return;
		if (!ref->cancel(id, shares)) {
			mismatches++;
			mismatch_by_loc[loc]++;
			if (trace_left > 0) {
				trace_left--;
				fprintf(stderr, "MISMATCH ts=%llu %s id=%llu shares=%u : order missing or too small\n",
				        (unsigned long long)ts, full ? "DELETE" : "CANCEL", (unsigned long long)id, shares);
			}
		}
	}
};

// ---------------------------------------------------------------- reconstruction
struct Group {
	bool open = false;
	uint16_t locate = 0;
	uint64_t ts = 0;
	uint8_t rest_side = 0;
	uint64_t shares = 0;
	uint32_t worst = 0;
	std::vector<uint64_t> refs;    // orders the group hit
	std::vector<uint64_t> skipped; // orders with priority that a fill passed over
};

struct Reconstructor {
	Writer& w;
	Stats st;
	Book book; // the displayed ITCH book
	std::vector<uint8_t> included; // by locate
	Group g;
	uint64_t synth_next = SYNTHETIC_BIT | 1;
	std::vector<uint8_t> halted = std::vector<uint8_t>(65536, 0);  // orders enter without matching
	std::vector<uint8_t> resumed = std::vector<uint8_t>(65536, 0); // state T seen, book may still cross
	std::vector<std::vector<uint64_t>> held = std::vector<std::vector<uint64_t>>(65536);

	explicit Reconstructor(Writer& w_) : w(w_) { book.priority_by_id = true; }

	void on_trading_action(uint64_t ts, uint16_t loc, uint8_t state) {
		if (state == 'T') {
			if (halted[loc]) { resumed[loc] = 1; try_release(ts, loc); }
			return;
		}
		flush_group();
		if (!halted[loc]) st.halts++;
		halted[loc] = 1;
		resumed[loc] = 0;
	}

	// after the reopen, write the held orders once the book no longer crosses
	void try_release(uint64_t ts, uint16_t loc) {
		if (!halted[loc] || !resumed[loc] || book.crossed(loc)) return;
		std::vector<uint64_t>& ids = held[loc];
		std::sort(ids.begin(), ids.end());
		for (uint64_t id : ids) {
			Node* o = book.find(id);
			if (!o || !o->deferred) continue;
			o->deferred = 0;
			w.add(ts, loc, id, o->side, o->shares, o->price, 0);
			st.add_released++;
		}
		ids.clear();
		halted[loc] = 0;
		resumed[loc] = 0;
	}

	static uint32_t clamp32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(v); }

	// Orders that had priority over a fill but that the group never hit did
	// not trade on the exchange (self-trade prevention, for example). They
	// leave the book before the aggressive order arrives.
	void cancel_skipped() {
		for (uint64_t id : g.skipped) {
			if (std::find(g.refs.begin(), g.refs.end(), id) != g.refs.end()) continue;
			Node* o = book.find(id);
			if (!o) continue;
			w.cancel(g.ts, g.locate, id, o->shares, true);
			st.cancel_skipped++;
			book.remove(o);
		}
		g.skipped.clear();
	}

	void flush_group() {
		if (!g.open) return;
		g.open = false;
		cancel_skipped();
		if (g.shares == 0) return;
		uint8_t side = g.rest_side == 'B' ? 'S' : 'B';
		uint32_t shares = clamp32(g.shares);
		w.add(g.ts, g.locate, synth_next++, side, shares, g.worst, shares);
		st.add_aggr_synth++;
	}

	void on_add(uint64_t ts, uint16_t loc, uint64_t ref, uint8_t side, uint32_t shares, uint32_t price) {
		if (halted[loc]) {
			Node* o = book.rest(ref, loc, side, shares, price);
			o->deferred = 1;
			held[loc].push_back(ref);
			st.add_held++;
			return;
		}
		bool reaches = (side == 'B') ? price >= g.worst : price <= g.worst;
		if (g.open && g.locate == loc && g.ts == ts && g.rest_side != side && reaches) {
			// the unfilled remainder of the aggressive order
			g.open = false;
			cancel_skipped();
			w.add(ts, loc, ref, side, clamp32(g.shares + shares), price, clamp32(g.shares));
			st.add_aggr_rem++;
		} else {
			// A remainder whose display price does not reach the fills is a
			// "price to comply" order. It becomes a separate resting order.
			flush_group();
			w.add(ts, loc, ref, side, shares, price, 0);
			st.add_itch++;
		}
		book.rest(ref, loc, side, shares, price);
	}

	// exec_price is 0 for an E message
	void on_fill(uint64_t ts, uint16_t loc, uint64_t ref, uint32_t shares, uint32_t exec_price) {
		Node* o = book.find(ref);
		if (!o) { st.fill_unknown_ref++; return; }
		if (shares > o->shares) { st.fill_over++; shares = o->shares; }
		if (halted[loc]) {
			// a reopen cross fill
			if (o->deferred) { st.held_fill_shares += shares; book.reduce(o, shares); return; }
			w.cancel(ts, loc, ref, shares, shares == o->shares);
			st.cancel_halt_cross++;
			book.reduce(o, shares);
			return;
		}
		bool hidden_px = exec_price != 0 &&
		                 ((o->side == 'B' && exec_price > o->price) || (o->side == 'S' && exec_price < o->price));
		if (hidden_px) {
			// the order executed at a price its display price does not reach
			if (!cancel_shares(ts, loc, o, shares, false)) { st.skipped_then_filled++; return; }
			st.cancel_hidden_px++;
			st.hidden_px_shares += shares;
			return;
		}
		if (g.open && (g.locate != loc || g.ts != ts || g.rest_side != o->side)) {
			flush_group();
			o = book.find(ref); // the flush may have removed it as a skipped order
			if (!o) { st.skipped_then_filled++; return; }
		}
		if (!g.open) {
			g.open = true; g.locate = loc; g.ts = ts; g.rest_side = o->side;
			g.shares = 0; g.worst = o->price; g.refs.clear(); g.skipped.clear();
		}
		// fills of one sweep can arrive out of priority order, so a passed-over
		// order is only a candidate until the group closes
		book.for_each_before(loc, o->side, o, [&](Node* skip) {
			if (std::find(g.skipped.begin(), g.skipped.end(), skip->id) == g.skipped.end()) {
				if (skip->price != o->price) st.skipped_better_level++;
				g.skipped.push_back(skip->id);
			}
		});
		g.shares += shares;
		g.refs.push_back(ref);
		g.worst = (o->side == 'B') ? std::min(g.worst, o->price) : std::max(g.worst, o->price);
		st.fill_shares += shares;
		book.reduce(o, shares);
	}

	// returns false when the order is gone
	bool cancel_shares(uint64_t ts, uint16_t loc, Node* o, uint32_t shares, bool full) {
		uint64_t id = o->id;
		if (g.open) {
			bool hit = std::find(g.refs.begin(), g.refs.end(), id) != g.refs.end();
			if (g.ts != ts || g.locate != loc || hit) {
				if (hit) st.group_split_by_cancel++;
				flush_group();
				o = book.find(id); // the flush may have removed it as a skipped order
				if (!o) { st.skipped_then_cancelled++; return false; }
			}
		}
		if (full) shares = o->shares;
		if (shares > o->shares) { st.cancel_over++; shares = o->shares; }
		if (shares == o->shares) full = true;
		w.cancel(ts, loc, o->id, shares, full);
		book.reduce(o, shares);
		return true;
	}

	void on_cancel(uint64_t ts, uint16_t loc, uint64_t ref, uint32_t shares, bool full) {
		Node* o = book.find(ref);
		if (!o) { st.cancel_unknown_ref++; return; }
		if (o->deferred) { book.reduce(o, full ? o->shares : std::min(shares, o->shares)); return; }
		if (!cancel_shares(ts, loc, o, shares, full)) return;
		if (full) st.cancel_full++; else st.cancel_partial++;
	}

	void on_replace(uint64_t ts, uint16_t loc, uint64_t old_ref, uint64_t new_ref, uint32_t shares, uint32_t price) {
		flush_group();
		Node* o = book.find(old_ref);
		if (!o) {
			st.replace_unknown_ref++;
			return; // the side is unknown, so the new order cannot be placed
		}
		uint8_t side = o->side;
		if (!o->deferred) { w.cancel(ts, loc, old_ref, o->shares, true); st.cancel_replace++; }
		book.remove(o);
		if (halted[loc]) {
			Node* n = book.rest(new_ref, loc, side, shares, price);
			n->deferred = 1;
			held[loc].push_back(new_ref);
			st.add_held++;
			return;
		}
		w.add(ts, loc, new_ref, side, shares, price, 0);
		st.add_replace++;
		book.rest(new_ref, loc, side, shares, price);
	}

	void run(const uint8_t* buf, size_t n, bool quiet) {
		size_t off = 0;
		uint64_t next_report = 50'000'000;
		while (off + 2 <= n) {
			uint16_t len = be16(buf + off);
			off += 2;
			if (off + len > n) break;
			const uint8_t* m = buf + off;
			off += len;
			st.messages++;
			if (!quiet && st.messages >= next_report) {
				fprintf(stderr, "  %llu messages in, %llu out, %zu resting orders\n",
				        (unsigned long long)st.messages, (unsigned long long)w.count, book.size());
				next_report += 50'000'000;
			}
			char t = char(m[0]);
			uint16_t loc = be16(m + 1);
			uint64_t ts = be48(m + 5);
			if (t == 'S') { flush_group(); w.copy(m, len); continue; }
			if (t == 'R') {
				if (included[loc]) { memcpy(w.stock[loc].data(), m + 11, 8); w.copy(m, len); }
				continue;
			}
			if (!included[loc]) {
				// a fill group never spans another stock
				if (g.open && t != 'P') flush_group();
				continue;
			}
			switch (t) {
				case 'A': case 'F':
					on_add(ts, loc, be64(m + 11), m[19], be32(m + 20), be32(m + 32));
					break;
				case 'E':
					on_fill(ts, loc, be64(m + 11), be32(m + 19), 0);
					break;
				case 'C':
					on_fill(ts, loc, be64(m + 11), be32(m + 19), be32(m + 32));
					break;
				case 'X':
					on_cancel(ts, loc, be64(m + 11), be32(m + 19), false);
					break;
				case 'D':
					on_cancel(ts, loc, be64(m + 11), 0, true);
					break;
				case 'U':
					on_replace(ts, loc, be64(m + 11), be64(m + 19), be32(m + 27), be32(m + 31));
					break;
				case 'P':
					break; // hidden fills of the same sweep. Do not close the group.
				case 'H':
					w.copy(m, len);
					on_trading_action(ts, loc, m[19]);
					break;
				default:
					flush_group();
					break;
			}
			if (resumed[loc]) try_release(ts, loc);
		}
		flush_group();
	}
};

// count A and F messages per locate, for --top
static std::vector<uint64_t> count_adds(const uint8_t* buf, size_t n) {
	std::vector<uint64_t> c(65536, 0);
	size_t off = 0;
	while (off + 2 <= n) {
		uint16_t len = be16(buf + off);
		off += 2;
		if (off + len > n) break;
		const uint8_t* m = buf + off;
		off += len;
		if (m[0] == 'A' || m[0] == 'F') c[be16(m + 1)]++;
	}
	return c;
}

int main(int argc, char** argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: %s <itch file> [--out <file>] [--symbol SYM]... [--top N] [--verify] [--trace N] [--quiet]\n", argv[0]);
		return 2;
	}
	std::string in = argv[1], out;
	std::vector<std::string> symbols;
	int top = 0, trace = 0;
	bool verify = false, quiet = false;
	for (int i = 2; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--out" && i + 1 < argc) out = argv[++i];
		else if (a == "--symbol" && i + 1 < argc) { std::string s = argv[++i]; s.resize(8, ' '); symbols.push_back(s); }
		else if (a == "--top" && i + 1 < argc) top = atoi(argv[++i]);
		else if (a == "--verify") verify = true;
		else if (a == "--trace" && i + 1 < argc) { trace = atoi(argv[++i]); verify = true; }
		else if (a == "--quiet") quiet = true;
		else { fprintf(stderr, "unknown argument %s\n", a.c_str()); return 2; }
	}
	if (out.empty()) {
		out = in;
		size_t dot = out.rfind('.');
		if (dot != std::string::npos && out.find('/', dot) == std::string::npos) out.resize(dot);
		out += "-inbound.bin";
	}

	int fd = open(in.c_str(), O_RDONLY);
	if (fd < 0) { perror("open input"); return 1; }
	struct stat stt;
	fstat(fd, &stt);
	size_t n = size_t(stt.st_size);
	const uint8_t* buf = (const uint8_t*)mmap(nullptr, n, PROT_READ, MAP_PRIVATE, fd, 0);
	if (buf == MAP_FAILED) { perror("mmap"); return 1; }
	madvise((void*)buf, n, MADV_SEQUENTIAL);

	// which locates go to the output
	std::vector<uint8_t> included(65536, symbols.empty() && top == 0);
	if (top > 0) {
		if (!quiet) fprintf(stderr, "counting add orders per symbol for --top %d\n", top);
		std::vector<uint64_t> adds = count_adds(buf, n);
		std::vector<uint16_t> order;
		for (uint32_t l = 0; l < 65536; ++l) if (adds[l] > 0) order.push_back(uint16_t(l));
		std::sort(order.begin(), order.end(), [&](uint16_t a, uint16_t b) { return adds[a] > adds[b]; });
		for (int i = 0; i < top && i < int(order.size()); ++i) included[order[i]] = 1;
	}
	if (!symbols.empty()) {
		// map symbol names to locates with the R messages at the start of the file
		size_t off = 0;
		while (off + 2 <= n) {
			uint16_t len = be16(buf + off);
			off += 2;
			if (off + len > n) break;
			const uint8_t* m = buf + off;
			off += len;
			if (m[0] != 'R') { if (m[0] == 'A') break; continue; }
			for (auto& s : symbols) if (memcmp(m + 11, s.data(), 8) == 0) included[be16(m + 1)] = 1;
		}
	}

	Writer w;
	w.open(out);
	RefEngine ref;
	if (verify) { w.ref = &ref; w.trace_left = trace; }
	Reconstructor rc(w);
	rc.included = included;
	rc.run(buf, n, quiet);
	w.close();
	munmap((void*)buf, n);
	close(fd);

	const Stats& s = rc.st;
	int nsym = 0;
	for (auto v : included) nsym += v;
	printf("input messages          %llu\n", (unsigned long long)s.messages);
	printf("output messages         %llu  -> %s  (%d symbols)\n", (unsigned long long)w.count, out.c_str(), nsym);
	printf("  A from A/F            %llu\n", (unsigned long long)s.add_itch);
	printf("  A from U              %llu\n", (unsigned long long)s.add_replace);
	printf("  A aggressive, synth   %llu\n", (unsigned long long)s.add_aggr_synth);
	printf("  A aggressive, rest    %llu\n", (unsigned long long)s.add_aggr_rem);
	printf("  X from X              %llu\n", (unsigned long long)s.cancel_partial);
	printf("  D from D              %llu\n", (unsigned long long)s.cancel_full);
	printf("  D from U              %llu\n", (unsigned long long)s.cancel_replace);
	printf("  X from hidden-price C %llu  (%llu shares)\n", (unsigned long long)s.cancel_hidden_px, (unsigned long long)s.hidden_px_shares);
	printf("  D for skipped orders  %llu  (%llu at a better level)\n", (unsigned long long)s.cancel_skipped, (unsigned long long)s.skipped_better_level);
	printf("  X/D from reopen cross %llu\n", (unsigned long long)s.cancel_halt_cross);
	printf("halts                   %llu\n", (unsigned long long)s.halts);
	printf("  orders held in halts  %llu, released %llu, cross-filled shares %llu\n",
	       (unsigned long long)s.add_held, (unsigned long long)s.add_released, (unsigned long long)s.held_fill_shares);
	printf("aggressive fill shares  %llu\n", (unsigned long long)s.fill_shares);
	printf("anomalies\n");
	printf("  fill unknown ref      %llu\n", (unsigned long long)s.fill_unknown_ref);
	printf("  fill over size        %llu\n", (unsigned long long)s.fill_over);
	printf("  cancel unknown ref    %llu\n", (unsigned long long)s.cancel_unknown_ref);
	printf("  cancel over size      %llu\n", (unsigned long long)s.cancel_over);
	printf("  replace unknown ref   %llu\n", (unsigned long long)s.replace_unknown_ref);
	printf("  group split by cancel %llu\n", (unsigned long long)s.group_split_by_cancel);
	printf("  skipped, then filled  %llu\n", (unsigned long long)s.skipped_then_filled);
	printf("  skipped, then removed %llu\n", (unsigned long long)s.skipped_then_cancelled);
	printf("resting at end (ITCH)   %zu\n", rc.book.size());

	if (verify) {
		uint64_t missing = 0, extra = 0, size_diff = 0;
		for (auto& [id, o] : rc.book.all()) {
			Node* r = ref.book.find(id);
			if (!r) missing++;
			else if (r->shares != o->shares || r->price != o->price) size_diff++;
		}
		for (auto& [id, o] : ref.book.all()) if (!rc.book.find(id)) extra++;
		printf("verify\n");
		printf("  messages that diverged %llu of %llu\n", (unsigned long long)w.mismatches, (unsigned long long)w.count);
		printf("  matched shares        %llu  (ITCH aggressive fills %llu)\n",
		       (unsigned long long)ref.matched_shares, (unsigned long long)s.fill_shares);
		printf("  cancel id missing     %llu\n", (unsigned long long)ref.cancel_missing);
		printf("  cancel over size      %llu\n", (unsigned long long)ref.cancel_over);
		printf("  synthetic id rested   %llu\n", (unsigned long long)ref.synthetic_rested);
		printf("  duplicate id          %llu\n", (unsigned long long)ref.duplicate_id);
		printf("  end book: ref %zu orders, ITCH %zu orders\n", ref.book.size(), rc.book.size());
		printf("  end book: missing %llu, extra %llu, size/price diff %llu\n",
		       (unsigned long long)missing, (unsigned long long)extra, (unsigned long long)size_diff);
		std::vector<uint16_t> locs;
		for (uint32_t l = 0; l < 65536; ++l) if (w.mismatch_by_loc[l]) locs.push_back(uint16_t(l));
		std::sort(locs.begin(), locs.end(), [&](uint16_t a, uint16_t b) { return w.mismatch_by_loc[a] > w.mismatch_by_loc[b]; });
		printf("  symbols with the most divergence:");
		for (size_t i = 0; i < locs.size() && i < 10; ++i)
			printf(" %.8s=%llu", w.stock[locs[i]].data(), (unsigned long long)w.mismatch_by_loc[locs[i]]);
		printf("\n");
	}
	return 0;
}
