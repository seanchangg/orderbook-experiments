#pragma once
#include <iostream>
#include <map>
#include <unordered_map>
#include <string>
#include <list>
#include <memory>
#include <optional>

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
	Order dummy; // sentinel, self-linked by Order's default constructor
	void appendOrder(Order* o) {
		o->prev = dummy.prev;
		o->prev->next = o;
		o->next = &dummy;
		dummy.prev = o;
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

class Orderbook { 
	public:
	std::map<PRICE, PriceLevel, std::greater<PRICE>> bid;
	std::map<PRICE, PriceLevel, std::less<PRICE>> ask;
	void addOrder(Order* o, std::unordered_map<ORDER_ID, Order*>& registry) {
		if (o->side == 'S') {
			while (o->quantity > 0) {
				if (!bid.empty() && bid.begin()->first >= o->price) {
					Order* match = bid.begin()->second.dummy.next;
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
				} else {
					ask[o->price].appendOrder(o);
					//std::cout << "Added Order: " << o->quantity << " shares of " << ticker_to_string(o->ticker) << " at " <<  price_to_double(o->price) << " | " << side_to_char(o->side) << "\n";
					break;
				}
			}
		} else {
			while (o->quantity > 0) {
				if (!ask.empty() && ask.begin()->first <= o->price) {
					Order* match = ask.begin()->second.dummy.next;
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
				} else {
					bid[o->price].appendOrder(o);
					//std::cout << "Added Order: " << o->quantity << " shares of " << ticker_to_string(o->ticker) << " at " << price_to_double(o->price) << " | " << side_to_char(o->side) << "\n";
					break;
				}
			}
		}
	}
	void getSummary() {
		//std::cout << "ASKS:\n";
		for (auto& level : ask) {
			//std::cout << price_to_double(level.first) << ":\n";
			Order* o = level.second.dummy.next;
			while (o != &level.second.dummy) {
				//std::cout << "ID: " << o->id << " | PRICE: " << price_to_double(o->price) << " | QUANTITY: " << o->quantity << "\n";
				o = o->next;
			}
		}
		//std::cout << "BIDS:\n";
		for (auto& level : bid) {
			//std::cout << price_to_double(level.first) << ":\n";
			Order* o = level.second.dummy.next;
			while (o != &level.second.dummy) {
				//std::cout << "ID: " << o->id << " | PRICE: " << price_to_double(o->price) << " | QUANTITY: " << o->quantity << "\n";
				o = o->next;
			}
		}
	}
	void unlinkOrder(Order* order) {
		order->next ->prev = order->prev;
		order->prev->next = order->next;
		if (order->side == 'S') {
			auto it = ask.find(order->price);
			if (it != ask.end()) {
				if (ask[order->price].dummy.next == &ask[order->price].dummy) {
					ask.erase(order->price);
				}
			}
		} else {
			auto it = bid.find(order->price);
			if (it != bid.end()) {
				if (bid[order->price].dummy.next == &bid[order->price].dummy) {
					bid.erase(order->price);
				}
			}
		}
	delete order;
	}
};