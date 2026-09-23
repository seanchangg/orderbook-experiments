#pragma once
#include <iostream>
#include <string>
#include <unordered_map>
#include <memory>
#include <optional>
#include <vector>
#include <ranges>
#include <algorithm>

using PRICE = uint32_t;
using QUANTITY = uint32_t;
using ORDER_ID = uint64_t;
using TICKER = uint64_t;
using SIDE = unsigned char;


  
struct Order {
	ORDER_ID id;
	TICKER ticker;
	PRICE price;
	QUANTITY quantity;
	SIDE side; //0 = buy, 1 = sell
	Order* prev;
	Order* next;
	Order() : prev(this), next(this) {};
	Order(ORDER_ID order_id, SIDE side, QUANTITY quantity, TICKER ticker, PRICE price) : id(order_id), side(side), quantity(quantity), ticker(ticker), price(price), prev(this), next(this) {}
};

struct PriceLevel {
	PRICE price;
	std::unique_ptr<Order> dummy; // sentinel on the heap, self-linked by Order's default constructor
	explicit PriceLevel(PRICE p) : price(p), dummy(std::make_unique<Order>()) {};
	bool empty() const { return dummy->next == dummy.get(); }
	void appendOrder(Order* o) {
		o->prev = dummy->prev;
		o->prev->next = o;
		o->next = dummy.get();
		dummy->prev = o;
	}
};

static std::string ticker_to_string(TICKER t) {
      std::string s(8, ' ');
      for (int i = 7; i >= 0; --i) { s[i] = char(t & 0xFF); t >>= 8; }
      while (!s.empty() && s.back() == ' ') s.pop_back();
      return s;
}

static double price_to_double(PRICE p) {
	return static_cast<double>(p) / 10000.0;
}

static char side_to_char(SIDE s) {
	return (s == 'S') ? 'S' : 'B';
}

static auto bid_comp =[] (const PriceLevel& price_level, PRICE price) {return price_level.price < price;};
static auto ask_comp = [] (const PriceLevel& price_level, PRICE price) {return price_level.price > price;}; //should return to first price <= bid price?

class Orderbook { 
	public:
	std::vector<PriceLevel> bid;
	std::vector<PriceLevel> ask;
	void addOrder(Order* o, std::unordered_map<ORDER_ID, Order*>& registry) {
		if (o->side == 'S') {
			while (o->quantity > 0 && !bid.empty() && bid.back().price >= o->price) {
				Order* match = bid.back().dummy->next;
				QUANTITY matched_quantity = match->quantity;
				//std::cout << "Sold " << std::min(o->quantity, matched_quantity) << " shares of " << ticker_to_string(o->ticker) << " at " << price_to_double(o->price) << "\n";
				QUANTITY fulfilled_units = std::min(o->quantity, matched_quantity);
				o->quantity -= fulfilled_units;
				if (fulfilled_units >= matched_quantity) {
					registry.erase(match->id);
					unlinkOrder(match);
				} else {
					match->quantity -= fulfilled_units;
				}
			}
			if (o->quantity == 0) {
				registry.erase(o->id);
				delete o;
				return;
			}
			auto ask_rit = std::ranges::find_if(ask.rbegin(), ask.rend(), [&] (PriceLevel& l) {return l.price <= o->price;});
			auto it = ask_rit.base();
			if (ask_rit != ask.rend() && ask_rit ->price == o->price) {
				ask_rit->appendOrder(o);
			} else {
				it = ask.insert(it, PriceLevel(o->price));
				it->appendOrder(o);
			}
			//std::cout << "Added Order: " << o->quantity << " shares of " << ticker_to_string(o->ticker) << " at " <<  price_to_double(o->price) << " | " << side_to_char(o->side) << "\n";
		} else {
			while (o->quantity > 0 && !ask.empty() && ask.back().price <= o->price) {
				Order* match = ask.back().dummy->next;
				QUANTITY matched_quantity = match->quantity;
				//std::cout << "Bought " << std::min(o->quantity, matched_quantity) << " shares of " << ticker_to_string(o->ticker) << " at " << price_to_double(o->price) << "\n";
				QUANTITY fulfilled_units = std::min(o->quantity, matched_quantity);
				o->quantity -= fulfilled_units;
				if (fulfilled_units >= matched_quantity) {
					registry.erase(match->id);
					unlinkOrder(match);
				} else {
					match->quantity -= fulfilled_units;
				}
			}
			if (o->quantity == 0) {
				registry.erase(o->id);
				delete o;
				return;
			}
			auto bid_rit = std::ranges::find_if(bid.rbegin(), bid.rend(), [&] (PriceLevel& l) {return l.price >= o->price;});
			auto it = bid_rit.base();
			if (bid_rit != bid.rend() && bid_rit ->price == o->price) {
				bid_rit->appendOrder(o);
			} else {
				it = bid.insert(it, PriceLevel(o->price));
				it->appendOrder(o);
			}
			//std::cout << "Added Order: " << o->quantity << " shares of " << ticker_to_string(o->ticker) << " at " << price_to_double(o->price) << " | " << side_to_char(o->side) << "\n";
		}
	}
	void getSummary() {
		//std::cout << "ASKS:\n";
		for (auto& level : ask) {
			//std::cout << price_to_double(level.price) << ":\n";
			Order* o = level.dummy->next;
			while (o != level.dummy.get()) {
				//std::cout << "ID: " << o->id << " | PRICE: " << price_to_double(o->price) << " | QUANTITY: " << o->quantity << "\n";
				o = o->next;
			}
		}
		//std::cout << "BIDS:\n";
		for (auto& level : bid) {
			//std::cout << price_to_double(level.price) << ":\n";
			Order* o = level.dummy->next;
			while (o != level.dummy.get()) {
				//std::cout << "ID: " << o->id << " | PRICE: " << price_to_double(o->price) << " | QUANTITY: " << o->quantity << "\n";
				o = o->next;
			}
		}
	}
	void unlinkOrder(Order* o) {
		o->next ->prev = o->prev;
		o->prev->next = o->next;
		if (o->side == 'S') {
			auto ask_rit = std::ranges::find_if(ask.rbegin(), ask.rend(), [&] (PriceLevel& l) {return l.price <= o->price;});
			if (ask_rit != ask.rend() && ask_rit->price == o->price) {
				if (ask_rit->empty()) {
					auto it = next(ask_rit).base();
					ask.erase(it);
				}
			}
		} else {
			auto bid_rit = std::ranges::find_if(bid.rbegin(), bid.rend(), [&] (PriceLevel& l) {return l.price >= o->price;});
			if (bid_rit != bid.rend() && bid_rit->price == o->price) {
				if (bid_rit->empty()) {
					auto it = next(bid_rit).base();
					bid.erase(it);
				}
			}
		}
	delete o;
	}
};