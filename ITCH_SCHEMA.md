# Nasdaq TotalView-ITCH 5.0 Schema

Reference for the C++ parser in this project. The full specification is here:

https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf

Sample data files are here:

https://emi.nasdaq.com/ITCH/

## General rules

- All integers are big-endian and unsigned.
- Alpha fields are ASCII, left-justified, and padded with spaces.
- Price(4) is a 4-byte integer with 4 implied decimal places. Divide by 10000.
- Price(8) is an 8-byte integer with 8 implied decimal places. Only the MWCB message uses it.
- The timestamp is a 6-byte integer. It is nanoseconds since midnight, Eastern Time.
- The file has no packet headers. Each message has a 2-byte big-endian length prefix, then the message body.

## File framing

```
[len:2][message:len][len:2][message:len]...
```

## Common header (first 11 bytes of every message)

| Offset | Size | Field |
|---|---|---|
| 0 | 1 | Message Type (ASCII char) |
| 1 | 2 | Stock Locate |
| 3 | 2 | Tracking Number |
| 5 | 6 | Timestamp (ns since midnight) |

## Order book messages

### 'A' Add Order, 36 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | Order Reference Number |
| 19 | 1 | Buy/Sell Indicator ('B' or 'S') |
| 20 | 4 | Shares |
| 24 | 8 | Stock |
| 32 | 4 | Price(4) |

### 'F' Add Order with MPID, 40 bytes

Same as 'A' plus:

| Offset | Size | Field |
|---|---|---|
| 36 | 4 | Attribution (MPID) |

### 'E' Order Executed, 31 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | Order Reference Number |
| 19 | 4 | Executed Shares |
| 23 | 8 | Match Number |

### 'C' Order Executed With Price, 36 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | Order Reference Number |
| 19 | 4 | Executed Shares |
| 23 | 8 | Match Number |
| 31 | 1 | Printable ('Y' or 'N') |
| 32 | 4 | Execution Price(4) |

### 'X' Order Cancel, 23 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | Order Reference Number |
| 19 | 4 | Cancelled Shares |

### 'D' Order Delete, 19 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | Order Reference Number |

### 'U' Order Replace, 35 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | Original Order Reference Number |
| 19 | 8 | New Order Reference Number |
| 27 | 4 | Shares |
| 31 | 4 | Price(4) |

### 'P' Trade (non-cross), 44 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | Order Reference Number |
| 19 | 1 | Buy/Sell Indicator |
| 20 | 4 | Shares |
| 24 | 8 | Stock |
| 32 | 4 | Price(4) |
| 36 | 8 | Match Number |

### 'Q' Cross Trade, 40 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | Shares |
| 19 | 8 | Stock |
| 27 | 4 | Cross Price(4) |
| 31 | 8 | Match Number |
| 39 | 1 | Cross Type |

### 'B' Broken Trade, 19 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | Match Number |

## Reference and control messages

### 'S' System Event, 12 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 1 | Event Code |

Event codes: O start of messages, S start of system hours, Q start of market hours, M end of market hours, E end of system hours, C end of messages.

### 'R' Stock Directory, 39 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | Stock |
| 19 | 1 | Market Category |
| 20 | 1 | Financial Status Indicator |
| 21 | 4 | Round Lot Size |
| 25 | 1 | Round Lots Only |
| 26 | 1 | Issue Classification |
| 27 | 2 | Issue Sub-Type |
| 29 | 1 | Authenticity |
| 30 | 1 | Short Sale Threshold Indicator |
| 31 | 1 | IPO Flag |
| 32 | 1 | LULD Reference Price Tier |
| 33 | 1 | ETP Flag |
| 34 | 4 | ETP Leverage Factor |
| 38 | 1 | Inverse Indicator |

### 'H' Stock Trading Action, 25 bytes

| Offset | Size | Field |
|---|---|---|
| 11 | 8 | Stock |
| 19 | 1 | Trading State |
| 20 | 1 | Reserved |
| 21 | 4 | Reason |

Trading states: H halted, P paused, Q quotation only, T trading.

### Other messages

| Type | Name | Size | Fields |
|---|---|---|---|
| 'Y' | Reg SHO Restriction | 20 | Stock at 11, Reg SHO Action at 19 |
| 'L' | Market Participant Position | 26 | MPID at 11, Stock at 15, three 1-byte flags at 23, 24, 25 |
| 'V' | MWCB Decline Level | 35 | Three Price(8) values at 11, 19, 27 |
| 'W' | MWCB Status | 12 | Breached Level at 11 |
| 'K' | IPO Quoting Period Update | 28 | Stock at 11, Release Time at 19, Qualifier at 23, IPO Price(4) at 24 |
| 'J' | LULD Auction Collar | 35 | Stock at 11, four 4-byte fields at 19, 23, 27, 31 |
| 'h' | Operational Halt | 21 | Stock at 11, Market Code at 19, Halt Action at 20 |
| 'I' | Net Order Imbalance Indicator | 50 | Paired Shares at 11, Imbalance Shares at 19, Direction at 27, Stock at 28, Far Price at 36, Near Price at 40, Reference Price at 44, Cross Type at 48, Price Variation at 49 |
| 'N' | Retail Price Improvement Indicator | 20 | Stock at 11, Interest Flag at 19 |
| 'O' | Direct Listing with Capital Raise | 48 | Stock at 11, Eligibility at 19, Price(4) at 20, 24, 28, 8-byte time at 32, Price(4) at 40, 44 |

## Notes for the order book

- The Order Reference Number is unique for the whole day. Use it as the key in the order map.
- An 'E' or 'C' message does not carry the stock or price. Look the order up by reference number.
- An 'X' message reduces shares. A 'D' message removes the order.
- A 'U' message deletes the old order and adds a new one. The new order keeps the side and the stock of the old order. It loses its time priority.
- The Stock Locate field is a per-day integer for the symbol. It is a good array index. The 'R' messages at the start of the file map it to the ticker.

## C++ decode pattern

Do not cast the buffer to a packed struct. Alignment and byte order will cause errors. Read each field with a small helper:

```cpp
#include <cstdint>
#include <cstring>

inline uint16_t be16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | p[1];
}
inline uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  p[3];
}
inline uint64_t be48(const uint8_t* p) {
    return (uint64_t(be16(p)) << 32) | be32(p + 2);
}
inline uint64_t be64(const uint8_t* p) {
    return (uint64_t(be32(p)) << 32) | be32(p + 4);
}

struct AddOrder {
    uint16_t locate;
    uint64_t ts_ns;
    uint64_t order_ref;
    char     side;
    uint32_t shares;
    char     stock[8];
    uint32_t price;   // divide by 10000 for dollars
};

inline AddOrder parse_add(const uint8_t* m) {
    AddOrder a;
    a.locate    = be16(m + 1);
    a.ts_ns     = be48(m + 5);
    a.order_ref = be64(m + 11);
    a.side      = char(m[19]);
    a.shares    = be32(m + 20);
    std::memcpy(a.stock, m + 24, 8);
    a.price     = be32(m + 32);
    return a;
}
```

The read loop:

```cpp
// buf points at the unpacked file, n is its length
size_t off = 0;
while (off + 2 <= n) {
    uint16_t len = be16(buf + off);
    off += 2;
    if (off + len > n) break;
    const uint8_t* m = buf + off;
    switch (m[0]) {
        case 'A': case 'F': on_add(parse_add(m));       break;
        case 'E': on_exec(m);                            break;
        case 'C': on_exec_price(m);                      break;
        case 'X': on_cancel(m);                          break;
        case 'D': on_delete(m);                          break;
        case 'U': on_replace(m);                         break;
        case 'P': case 'Q': case 'B': /* trades */       break;
        default:  /* reference messages, skip */         break;
    }
    off += len;
}
```

For a latency project, map the whole file with mmap and skip the copy. Keep the price as an integer in the book. Convert to a double only for output.

## Inbound order files

`reconstruct` turns an ITCH file into the stream of order-entry requests that
produced it. The output uses the same framing and message layout as this
document, so `parse.h` reads it without change. It contains only:

| Type | Meaning in the inbound file |
|---|---|
| 'R' | Stock directory for each output symbol |
| 'S' | System event, copied from the input |
| 'A' | A limit order enters the book. Your engine must match it when it crosses. |
| 'X' | Cancel part of a resting order |
| 'D' | Delete a resting order |

Notes:

- An 'A' with the high bit set in the Order Reference Number is an aggressive
  order that filled completely. ITCH never shows the id of such an order, so
  the tool assigns one.
- A 'U' in the input becomes a 'D' of the old order and an 'A' of the new
  order in the output.
- `./reconstruct S120825-v50.bin --top 100 --verify` writes the 100 most
  active symbols and replays the output through a reference matcher. See the
  comment at the top of `reconstruct.cpp` for the full mapping.
