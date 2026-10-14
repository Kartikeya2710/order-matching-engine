# To Implement

Work agreed for the engine but not yet done. Items are added only on request; tick them off as they land.

- [ ] **Fix `OrderBook` API:** Use request-specific struct in place of a generic command
- [ ] **Checks for positive quantity:** The order book API must check that the quantity it receives everywhere is positive (> 0)
- [ ] **Send reject if pool is full:** An appropriate reject messgae must be sent if the pool is full
- [ ] **Check order ownership:** `modifyOrder()` and `cancelOrder()` must check whether the client owns the order
- [ ] **Engine-assigned order IDs:** Generate our own order ID for every new order and return it in the accept message. Modify and cancel requests refer to an order by this ID
- [ ] **Self-trade prevention:** An order should not match against a resting order belonging to the same client
- [ ] **Fix `ArrayBitMapLocator` API:** The current `ArrayBitMapLocator` API exposes methods such as `markEmpty()` and `markNonEmpty()` which do not generalise
- [ ] **Order book snapshot and recovery:** The state of the order book should be recoverable in case of some unexpected termination of the program.
- [ ] **Implement `FlexBook`:** A tree-based price level locator that does not constrain an instrument to a price range.
- [ ] **Level 1 and 2 market data:** Expose level 1 and 2 market data
- [x] **Fill-Or-Kill (FOK):** Implement FOK order type
