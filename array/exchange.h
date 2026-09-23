#pragma once
#include "orderbook.h"

class Exchange {
	protected:
	std::unordered_map<ORDER_ID, Order*> registry;
	std::unordered_map<TICKER, std::unique_ptr<Orderbook>> orderbooks;
	public:
	void popOrder(ORDER_ID order_id) {
		Order* order = registry[order_id];
		orderbooks[order->ticker]->unlinkOrder(registry[order_id]);
		registry.erase(order_id);
	}
};