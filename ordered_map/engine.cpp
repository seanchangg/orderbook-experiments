#include "engine.h"
#include <algorithm>
#include <random>

using PRICE = uint32_t;
using QUANTITY = uint32_t;
using ORDER_ID = uint64_t;
using TICKER = uint64_t;
using SIDE = unsigned char;

void Engine::addOrder(ORDER_ID order_id, SIDE side, QUANTITY quantity, TICKER ticker, PRICE price) {
	registry[order_id] = new Order(order_id, side, quantity, ticker, price);
	if (orderbooks.find(ticker) != orderbooks.end()) {
		orderbooks[ticker]->addOrder(registry[order_id], registry);	
	} else {
		orderbooks[ticker] = std::make_unique<Orderbook>();
		orderbooks[ticker]->addOrder(registry[order_id], registry);
	}
};

void Engine::cancelOrder(ORDER_ID order_id, QUANTITY quantity) {
	auto it = registry.find(order_id);
	if (it == registry.end()) return;
	registry[order_id]->quantity -= quantity;
	//std::cout << "Canceled: " << quantity << " shares of " << order_id << "\n";
}
void Engine::deleteOrder(ORDER_ID order_id) {
	auto it = registry.find(order_id);
	if (it == registry.end()) return;
	popOrder(order_id);
	//std::cout << "Deleted Order: " << order_id << "\n";
}

void Engine::systemEvent(unsigned char event){};
void Engine::addStock(TICKER ticker, unsigned char market_category, unsigned char financial_status, uint32_t round_lot_size, unsigned char round_lots_only, unsigned char issue_classification, uint16_t issue_sub_type, unsigned char authenticity, unsigned char short_sale_threshold, unsigned char ipo_flag, unsigned char luld_tier, unsigned char etp_flag, uint32_t etp_leverage, unsigned char inverse) {};
void Engine::tradingAction(TICKER ticker, unsigned char state, unsigned char reserved, uint32_t reason){};
void Engine::operationalHalt(TICKER ticker, unsigned char market_code, unsigned char halt_action){};
void Engine::getSummary(TICKER ticker) {
	//std::cout << ticker_to_string(ticker) << " Summary:\n";
	orderbooks[ticker]->getSummary();
}