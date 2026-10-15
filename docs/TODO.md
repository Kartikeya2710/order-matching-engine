# To Implement

Work agreed for the engine but not yet done. Items are added only on request; tick them off as they land.

- [ ] **Checks for positive quantity:** The order book API must check that the quantity it receives everywhere is positive (> 0)
- [ ] **Send reject if pool is full:** An appropriate reject messgae must be sent if the pool is full
- [ ] **Check order ownership:** `modifyOrder()` and `cancelOrder()` must check whether the client owns the order
- [ ] **Engine-assigned order IDs:** Generate our own order ID for every new order and return it in the accept message. Modify and cancel requests refer to an order by this ID
- [ ] **Self-trade prevention:** An order should not match against a resting order belonging to the same client
- [ ] **Fix `ArrayBitMapLocator` API:** The current `ArrayBitMapLocator` API exposes methods such as `markEmpty()` and `markNonEmpty()` which do not generalise
- [ ] **Order book snapshot and recovery:** The state of the order book should be recoverable in case of some unexpected termination of the program.
- [ ] **Implement `FlexBook`:** A tree-based price level locator that does not constrain an instrument to a price range.
- [ ] **Level 1 and 2 market data:** Expose level 1 and 2 market data
- [ ] **Reject message for order pool exhaustion:** Emit suitable reject message if an order can't be accommodated in the order pool
- [ ] **Match modified orders on price change:** `modifyOrder()` re-adds an order at its new price without matching it, so a modify can leave the book crossed
- [ ] **Keep priority on no-op modify:** A modify with the same price and quantity cancels and re-adds the order, losing its time priority for no change
- [ ] **Consistent modify events:** A quantity reduction emits no event, while a price change or quantity increase emits `OrderAccepted`
- [ ] **Reprice orders in place on modify:** A price change or quantity increase releases and re-acquires the same pool slot and erases and re-inserts the same index entry. Move the order between levels in one helper that leaves the book consistent, instead of separate unlink and link helpers. Depends on fixing the `ArrayBitMapLocator` API
- [ ] **Order book invariant checks:** A test-only `checkInvariants()` that asserts after every operation that each indexed order sits in exactly one level, that a level is non-empty exactly when the locator lists it, and that each level's `totalQty` and `orderCount` match its orders
- [x] **Fill-Or-Kill (FOK):** Implement FOK order type
- [x] **Fix `OrderBook` API:** Use request-specific struct in place of a generic command
