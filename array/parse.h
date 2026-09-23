#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstdint>
#include "engine.h"
using namespace std;

#define FILENAME "../S120825-inbound-top100.bin"

class parser {
	private:
		Engine* exch;
		int file_descriptor;
		struct stat file_info;
		size_t size;
		void* file;
		const uint8_t* ptr;
	public:
	parser(Engine& e) : exch(&e) {
		file_descriptor = open(FILENAME, O_RDONLY);
		fstat(file_descriptor, &file_info);
		size = file_info.st_size;
		file = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, file_descriptor, 0);
		ptr = (const uint8_t*)file;
	}
	~parser() {
		munmap(file, size);
		close(file_descriptor);
	}
	uint8_t getU8() {
		unsigned char b = ptr[0];
		ptr += 1;
		return b;
	}
	uint16_t getU16() {
		uint16_t w = (ptr[0] << 8) | ptr[1];
		ptr += 2;
		return w;
	}
	uint32_t getU32() {
		uint32_t dw = (ptr[0] << 24) | (ptr[1] << 16) | (ptr[2] << 8) | (ptr[3]);
		ptr += 4;
		return dw;
	};
	uint64_t getU48() {
		uint64_t hw = ((uint64_t)ptr[0] << 48) | ((uint64_t)ptr[1] << 40) | ((uint64_t)ptr[2] << 32) | (ptr[3] << 24) | (ptr[4] << 16) | (ptr[5] << 8) | (ptr[6]);
		ptr += 6;
		return hw;
	};
	uint64_t getU64() {
		uint64_t qw = ((uint64_t)ptr[0] << 56) | ((uint64_t)ptr[1] << 48) | ((uint64_t)ptr[2] << 40) | ((uint64_t)ptr[3] << 32) | (ptr[4] << 24) | (ptr[5] << 16) | (ptr[6] << 8) | (ptr[7]);
		ptr += 8;
		return qw;
	};
	void getMessage() {
		//common header;
		uint16_t l = this->getU16();
		unsigned char t = this->getU8();
		uint16_t s = this->getU16();
		uint16_t n = this->getU16();
		uint64_t timestamp = this->getU48();
		switch (t) {
			case 'A' : { //add order
				ORDER_ID order_id = this->getU64();
				SIDE side = this->getU8();
				QUANTITY quantity = this->getU32();
				TICKER ticker = this->getU64();
				PRICE price = this->getU32(); //4 implied decimals
				exch->addOrder(order_id, side, quantity, ticker, price);
				break;
			};
			case 'X' : { //cancel (partial)
				ORDER_ID order_id = this->getU64();
				QUANTITY quantity = this->getU32();
				exch->cancelOrder(order_id, quantity);
				break;
			};
			case 'D' : { //delete
				ORDER_ID order_id = this->getU64();
				exch->deleteOrder(order_id);
				break;
			};
			case 'S' : { //system event
				unsigned char event = this->getU8();
				exch->systemEvent(event);
				break;
			};
			case 'R' : { //stock directory
				TICKER ticker = this->getU64();
				unsigned char market_category = this->getU8();
				unsigned char financial_status = this->getU8();
				uint32_t round_lot_size = this->getU32();
				unsigned char round_lots_only = this->getU8();
				unsigned char issue_classification = this->getU8();
				uint16_t issue_sub_type = this->getU16();
				unsigned char authenticity = this->getU8();
				unsigned char short_sale_threshold = this->getU8();
				unsigned char ipo_flag = this->getU8();
				unsigned char luld_tier = this->getU8();
				unsigned char etp_flag = this->getU8();
				uint32_t etp_leverage = this->getU32();
				unsigned char inverse = this->getU8();
				exch->addStock(ticker, market_category, financial_status, round_lot_size, round_lots_only, issue_classification, issue_sub_type, authenticity, short_sale_threshold, ipo_flag, luld_tier, etp_flag, etp_leverage, inverse);
				break;
			};
			case 'H' : { //stock trading action
				TICKER ticker = this->getU64();
				unsigned char state = this->getU8();
				unsigned char reserved = this->getU8();
				uint32_t reason = this->getU32();
				exch->tradingAction(ticker, state, reserved, reason);
				break;
			};
			default : { //unknown type, skip the body
				ptr += l - 11;
				break;
			};
		}
	}
};
