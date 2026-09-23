#pragma once
#include <cstdint>
#include <optional>
#include "exchange.h"

class Engine : Exchange {
	public:
	//order book
	void addOrder(ORDER_ID order_id, SIDE side, QUANTITY quantity, TICKER ticker, PRICE price);
	void cancelOrder(ORDER_ID order_id, QUANTITY quantity);
	void deleteOrder(ORDER_ID order_id);
	//session and symbol state
	void systemEvent(unsigned char event);
	void addStock(TICKER ticker, unsigned char market_category, unsigned char financial_status, uint32_t round_lot_size, unsigned char round_lots_only, unsigned char issue_classification, uint16_t issue_sub_type, unsigned char authenticity, unsigned char short_sale_threshold, unsigned char ipo_flag, unsigned char luld_tier, unsigned char etp_flag, uint32_t etp_leverage, unsigned char inverse);
	void tradingAction(TICKER ticker, unsigned char state, unsigned char reserved, uint32_t reason);
	void operationalHalt(TICKER ticker, unsigned char market_code, unsigned char halt_action);
	void getSummary(TICKER ticker);
};
