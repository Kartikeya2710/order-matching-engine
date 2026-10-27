# Design Decisions

A running log of the design decisions in this codebase: what was chosen, why, what else could have been done, and whether the choice still holds up.

The [README](../README.md) describes _what_ the engine does. This document explains _why_ it is built the way it is.

## How to read this

Decisions are grouped by component and numbered in the order they were reviewed (`DD-001`, `DD-002`, ...). Numbers are permanent; a decision that is later reversed keeps its number and gets its status updated.

Each entry carries one of these statuses:

| Status           | Meaning                                                             |
| ---------------- | ------------------------------------------------------------------- |
| **Keep**         | The current choice is the right one for this engine's goals.        |
| **Keep (noted)** | Fine for now, but has a known limit or a condition where it breaks. |
| **Revisit**      | A better alternative exists; the change is worth making.            |
| **Changed**      | Was revisited and the code now does something different.            |

## Index

| ID                                                                                               | Decision                                                                        | Status       |
| ------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------- | ------------ |
| [DD-001](#dd-001-the-gateway-checks-message-structure-the-engine-checks-values-and-state)        | The gateway checks message structure; the engine checks values and book state   | Keep         |
| [DD-002](#dd-002-one-struct-per-request-type-carried-in-a-stdvariant)                            | One struct per request type, carried in a `std::variant`                        | Changed      |
| [DD-003](#dd-003-price-levels-change-only-through-append-remove-and-reduce)                      | Price levels change only through `append`, `remove` and `reduce`                | Keep (noted) |
| [DD-004](#dd-004-a-modify-that-changes-nothing-succeeds-and-keeps-the-orders-place)              | A modify that changes nothing succeeds and keeps the order's place              | Keep         |
| [DD-005](#dd-005-a-modify-that-moves-an-order-keeps-its-slot-and-matches-first-if-it-crosses)    | A modify that moves an order keeps its slot and matches first if it crosses     | Keep         |
| [DD-006](#dd-006-the-order-book-checks-every-price-a-request-brings-in-the-locator-only-asserts) | The order book checks every price a request brings in; the locator only asserts | Keep         |

## Request intake

### DD-001: The gateway checks message structure; the engine checks values and book state

**Status:** Keep

**Where:** `MatchingCore::submit`, the engine's entry point

**What**

Requests reach the engine through the gateway into `MatchingCore::submit`. The gateway takes an OMS client's messages off the wire, decodes them, and hands the engine typed requests. Thus, the code assumes certain guarantees from the gateway and does not re-check certain things:

1. **Type and fields match**

   Real protocols check incoming messages for a fixed message layout per type: FIX `35=D` / `35=F` / `35=G`, NASDAQ OUCH `'O'` / `'X'` / `'U'`.
   - Every request is exactly one known type (new order, cancel or modify)
   - Every request is the exact length for its type
   - Every request has the correct fields for its type

2. **Enum values are valid**
   - `Verb`, `OrderType` and `TimeInForce` only ever hold one of their declared values.

3. **Numbers fit their types**
   - Numeric fields were range-checked during decoding and never truncated into `uint32_t`.

4. **`clientId` is authenticated**
   - The gateway sets it from the client's logged-in session; the client cannot choose it. Ownership checks (for example, only an order's owner may cancel it) rely on this.

5. **Order is preserved**
   - Requests for one instrument reach the engine in the order the gateway accepted them. The per-instrument SPSC queue preserves that order within the engine too.

**What the engine still checks**

Anything that needs the book's state or the instrument's config. Each check is a few branches, and the gateway cannot do it:

1. **The instrument exists**

   `MatchingCore::submit` returns `false` for an unknown instrument

2. **Value rules:**
   - Quantity > 0
   - Price within the instrument's range and on its tick grid

3. **State rules:**
   - The order ID is known in case of modifies and cancels
   - The requester owns the order
   - The order pool has room

The exact list and the reject codes are a separate decision.

**What happens if a guarantee is broken**

That is a gateway bug, and the engine's behaviour is unspecified. It does not crash, but it does something silently wrong. For example:

- A `Verb` of 7 is treated as `Sell`, because the price level locator tests only for `Buy`.
- An unknown `TimeInForce` behaves like GTC.
- An unknown `OrderType` is matched as a limit order.

To catch such bugs in tests without slowing release builds, the engine's entry point should `assert` guarantees 1 and 2 in debug builds only.

### DD-002: One struct per request type, carried in a `std::variant`

**Status:** Changed

**Where:** `Command`, the `OrderBook` request handlers, the worker's dispatch

**What**

New orders, cancels and modifies each need a different set of fields, and all three travel through the same per-instrument queue. The request type has to meet four needs:

1. **A request carries only its own type's fields**
   - A handler must not be able to read a field the request does not have. For example, a modify must not be able to change an order's side or owner.

2. **Every request type has a handler**
   - A request type without a handler should be a compile error, not a request that is silently dropped.

3. **It fits one cache line**
   - It must be at most 64 bytes and trivially copyable, so the SPSC ring buffer can copy it into a slot.

4. **All request types share one queue**
   - Otherwise DD-001's guarantee 5 (order is preserved) breaks.

The design is one struct per request type, carried in a `std::variant`:

```cpp
struct NewOrder    { OrderId orderId; ClientId clientId; Verb side; OrderType orderType;
                     TimeInForce tif; Price limitPrice; Quantity qty; };
struct CancelOrder { OrderId orderId; ClientId clientId; };
struct ModifyOrder { OrderId orderId; ClientId clientId; Price newLimitPrice; Quantity newQty; };

struct alignas(64) Command
{
    InstrumentId instrumentId;
    std::variant<NewOrder, CancelOrder, ModifyOrder> request;
};
```

- **Need 1:** the variant's tag is set by which struct is built, so it cannot disagree with the fields. This is what makes DD-001's guarantee 1 hold inside the engine.
- **Need 2:** the worker dispatches with `std::visit`, one handler per request type. With libstdc++ it compiles to a `switch` on the variant's index; libc++ dispatches through a table of function pointers, which costs one indirect call per request.
- **Need 3:** `NewOrder` is 20 bytes. The variant is 24 (largest member + tag + padding), and `Command` is 64. `static_assert(sizeof(Command) <= 64)` guards the size.
- **Need 4:** one `Command` type holds all three requests.

**Other designs**

- **One struct with every field, plus a type field:** fails need 1. The type and the fields are set independently, so a request can carry another type's fields and a handler can read them.
- **One struct per request type in a hand-written tagged union:** fails need 1. The tag is set by hand, and reading the wrong union member is undefined behaviour.
- **One queue per request type:** fails need 4. A cancel could be processed before the new order it refers to.

## Price levels

### DD-003: Price levels change only through `append`, `remove` and `reduce`

**Status:** Keep

**Where:** `ArrayBitMapLocator`, `PriceLevel`, `PoolSlot`, `LevelHandleReader`, `OrderPool::acquire`, `OrderPool::release`, `OrderPool::reprice`, `OrderPool::slots`, `OrderBook`

**What**

The orders themselves live in the order pool's slots, and each level's queue is a doubly linked list threaded through those slots. So a single change to the book touches three things that must agree: the slot's queue links and level handle, the level's head, tail and totals, and the level's bit (in case of bitmap-based level locator).

**Needs**

1. **The bitmap always matches the levels**
   - A level's bitmap must be in sync with the level at all times.
   - The `OrderBook` should not be responsible of updating the bitmap everytime an operation is performed on a level.

2. **The level locator keeps its orders' links in sync**
   - The `next` and `prev` links of an order in `OrderPool` are owned and maintained by the level, i.e., the `PriceLevel`.
   - Thus, a `PriceLevel` must be responsible for updating its orders' links on each operation

3. **Every change to a level is named**
   - The `OrderBook` sees exactly what it does to a level, and can't make a change outside this fixed set.
   - Thus, the price level locator only exposes `append()`, `remove()` and `reduce()`.

4. **The pool stays a plain slab allocator**
   - The `OrderPool` only deals with handing out, taking back and filling slots.
   - The `next` and `prev` links live inside the pool's slots but the operation is performed via the `PriceLevel`s. This is the very defintion of an _intrusive list_

5. **Only the pool and the level write an order**
   - No code other than `OrderPool`, when it acquires or reprices a slot, and `PriceLevel`, while the order rests, may write an order's fields.

6. **A resting order's level is found without its price**
   - `remove` and `reduce` reach the order's level straight from its slot. They do not work it out again from the price, which costs a division (without optimizations) in an array-based locator and a search in a tree-based one.
   - The pool does not depend on which locator is used, so a tree-based locator can use the same pool.
   - Only the locator may read where an order rests. The `OrderBook` must not read it, so it never holds a level index.

**Design**

Five types, one job each:

1. **`OrderPool`**
   - Hands out and takes back slots.
   - `acquire(Order)` takes a free slot and writes the order into it. It is the only way to write a whole order, so a slot cannot be acquired without an order, or written without being acquired.
   - `reprice(slotIdx, price, qty)` writes a new price and quantity into an order that a modify has taken out of its level (DD-005).
   - `OrderBook` holds the pool as a private member, so it is the only code that can acquire a slot.
   - The pool's only addition for the levels is `slots()`, which returns all slots as a `std::span<PoolSlot>` for `PriceLevel` to change.

2. **`PoolSlot`**
   - An `Order` (the order's ID, side, client, price, `qty` and TIF) plus the slot indexes of its neighbours in its level's queue (`next` and `prev`).
   - It also holds a level handle: a `uint32_t` that says which level the order rests in. Its meaning belongs to the locator. In `ArrayBitMapLocator` it is the level's index in `bids_` or `asks_`. A tree-based locator would keep its levels in an indexed array and use that index, so the slot and the pool stay the same for both (need 6).
   - Only `OrderPool` writes the whole slot in `acquire`, and the price and `qty` in `reprice`
   - `PriceLevel` changes `qty`, `next` and `prev` while the order rests. It writes the level handle when it appends the order and sets it back to `NULL_IDX` when it removes the order.
   - The handle has no public getter. A locator reads it through `LevelHandleReader`.
   - Copy assignment is deleted, so code holding `slots()` cannot overwrite a slot.

3. **`PriceLevel`**
   - One price's queue: `head`, `tail`, `totalQty` and `orderCount`.
   - Its `append`, `remove` and `reduce` change the links, the order's `qty` and the level's totals together.
   - `append(slotIdx, levelHandle, slots)` also writes the handle the locator passes in. `remove` sets it back to `NULL_IDX`, so a removed order has no level.
   - In debug builds, `remove` asserts that the order's neighbours, or the level's `head` and `tail`, point back at it, and that the order still has a level handle.

4. **`ArrayBitMapLocator`**
   - Owns every level on both sides and the two bitmaps.
   - The levels are private and `level()` returns `const`, so the locator's three operations are the only path to a mutable level:

     ```cpp
     const PriceLevel& level(Verb side, Price price) const;   // read-only, for walking a queue

     void append(uint32_t slotIdx, std::span<PoolSlot> slots);  // rest an order at the back of its level
     void remove(uint32_t slotIdx, std::span<PoolSlot> slots);  // take an order out of its level
     void reduce(uint32_t slotIdx, Quantity by, std::span<PoolSlot> slots);  // take quantity off an order
     ```

   - The locator offers no generic access to a level. Every change the book makes to a level is one of these three, so the set is closed:

     | Book event                                    | Operation          |
     | --------------------------------------------- | ------------------ |
     | A new order rests                             | `append`           |
     | A resting order fills fully, or is cancelled  | `remove`           |
     | A partial fill, or a modify that lowers `qty` | `reduce`           |
     | A modify that changes price or raises `qty`   | `remove`, `append` |

     Between that `remove` and `append`, `OrderPool::reprice` writes the order's new price and `qty`.

   - `append` works out the level's index from the order's price once and passes it to `PriceLevel::append` as the handle. `remove` and `reduce` read the handle instead of the price, which meets need 6. In debug builds they assert that the handle matches the price.

5. **`LevelHandleReader`**
   - Lets a locator read a slot's level handle, and nothing else. It is a friend of `PoolSlot`, and its only member, `levelHandleOf(slot)`, is `protected` and `static`.
   - It names no locator, so `PoolSlot` needs one friend declaration however many locators there are (need 6).
   - `ArrayBitMapLocator` inherits from it privately, so the handle does not show through the locator's public interface. `OrderBook` does not inherit from it (need 6). This is a convention: the compiler would let any class inherit from it.

**Other designs**

- **Public `PoolSlot` fields, or a mutable `OrderPool::operator[]`:** fails need 5. Any code holding a slot could change a resting order's price or side, leaving it in the wrong level, or its `qty`, leaving `totalQty` out of sync.
- **Separate `acquire()` and `fill(slotIdx, Order)`:** a slot could be acquired without being filled, filled without being acquired, or rewritten while it is live. Asserts could catch only the first two, and would need state per slot to do it.
- **Two parts in `PoolSlot`, each with one friend** (`qty`, `next` and `prev` for `PriceLevel`; the rest for `OrderPool`): would also stop `PriceLevel` from writing an order's ID, side or price, but adds a type and longer call sites (`order.levelEntry.qty()`) to guard the code already trusted with the queue.
- **Pointer links:** Using pointers makes it more difficult to capture snapshots of the order book very efficiently. It's a widely adopted design.
- **Mutable access to levels, with the book setting and clearing bits itself:** fails need 1. Every call site has to remember the bit.
- **One `edit(side, price, change)` that runs a callback on the level and re-syncs the bit afterwards:** fails need 3. The callback can make any change, and the call sites are harder to read than three named operations.
- **Queue logic in `OrderPool`, or the pool owning the links:** fails need 4.
- **The locator stores a reference to the pool:** The locator must not be able to acquire or release slots.
- **The locator finds the level from the price on every `remove` and `reduce`:** fails need 6. Every cancel and fill pays a division (or simpler operations if the inverted value is stored), or a tree search in a tree-based locator.
- **The locator keeps its own table of handles, indexed by slot:** meets need 6 without touching the pool, but reads one more cache line on every `remove` and `reduce`.

### DD-006: The order book checks every price a request brings in; the locator only asserts

**Status:** Keep

**Where:** `OrderBook::addOrder`, `OrderBook::modifyOrder`, `ArrayBitMapLocator`, `validateRange`, `loadInstrumentConfig`

**What**

The locator turns a price into an index into its arrays. A price below `minPrice` or above `maxPrice` gives an index outside them, and a price off the tick grid gives the level below. Exactly one piece of code must make sure such a price never reaches the locator.

**Needs**

1. **One owner for the check**
   - Every price the locator receives has been checked once, by code that is named.

2. **A bad price is rejected with a reason**
   - The client is told, and the book is not touched.

**Design**

- Only two places bring a price from outside into the book: `addOrder` (the limit price) and `modifyOrder` (the new price). Each calls `isValidPrice()` before the price is matched or stored, and rejects the request if it fails. This meets needs 1 and 2.
- Every other price the locator receives comes from the locator itself (`bestBid`, `bestAsk`, `nextBid`, `nextAsk`) or from a pool slot, which holds only prices checked on the way in.
- A market order's price never reaches the locator.
- The locator states the rule once, on `isValidPrice`, and asserts it: `priceToIndex` asserts `isValidPrice(price)`, and every helper that takes an index asserts `idx < numLevels_`. Release builds check nothing and debug builds tests stop at the first bad price.
- `nextBid(minPrice)` and `nextAsk(maxPrice)` find no level and return `NO_PRICE`. A price outside the range is a bug for both, and both assert.

**Other designs**

- **The locator checks every call:** This would lead to repetitive work and `append`, `remove` and `reduce` would need a way to report a failure.
- **A checked price type that only the locator can build:** the compiler would enforce need 1, but `Order` would need a second price type, since a market order carries a price that is not a level.
- **Prices in ticks throughout the engine, converted by the gateway:** an off-tick price could not be expressed at all, but the tick check would move across DD-001's boundary, and every event's price would need converting back.

## Order modification

### DD-004: A modify that changes nothing succeeds and keeps the order's place

**Status:** Keep

**Where:** `OrderBook::modifyOrder`

**What**

A modify carries the order's new price and quantity. Both can equal what the order already has. For example, a cancel/replace message (FIX `35=G`, OUCH `'U'`) carries the whole order, so an OMS can resend it unchanged.

**Needs**

1. **A modify that changes nothing leaves the book as it was**
   - The order keeps its place in its level's queue.
   - Nothing in the book changes, so the book's subscribers see no remove and re-add.

2. **The client is not told it made an error**
   - The request is valid, so it is accepted like any other modify, not rejected.

3. **An order loses its place only when its price changes or its quantity rises**
   - One rule for time priority, so a client can tell from its own request whether its order will move.

**Design**

- `modifyOrder()` runs its usual checks first (order ID known, price valid, quantity > 0), so a modify that changes nothing is checked like any other.
- If the new price equals the order's price and the new quantity equals its remaining quantity, `modifyOrder()` returns without touching the book. This meets needs 1 and 3.
- The request is not rejected. Its outcome is that of any accepted modify, which meets need 2. How a modify reports its outcome is a separate decision.
- Eurex T7 behaves the same way: an order's priority resets only when its price changes, it switches between market and limit, or its quantity rises.

**Other designs**

- **Treat it as a new order, so it loses its place** (Cboe, Nasdaq OUCH): fails needs 1 and 3. Cboe chose this to discourage messages that change nothing.
- **Reject it** (BitMEX): fails need 2. The client has to handle an error for a request that leaves the book exactly as it wants.

### DD-005: A modify that moves an order keeps its slot and matches first if it crosses

**Status:** Keep

**Where:** `OrderBook::modifyOrder`, `OrderBook::moveOrder`, `OrderBook::matchAggressor`, `OrderPool::reprice`

**What**

A modify that changes an order's price or raises its quantity sends the order to the back of a level, often at a new price. The new price can reach the other side of the book. For example, an ask rests at 101 and a bid at 100, and the bid is modified to 102.

**Needs**

1. **The book is never crossed**
   - A modify whose new price reaches the other side trades like an incoming order, and only what is left rests.
   - This is how FIX `35=G` and OUCH `'U'` work: a replace that crosses executes at once.

2. **A moving order stays the same order in the book**
   - Its pool slot and its order index entry stay; the order is not freed and acquired again.
   - Its side, client and TIF stay; a modify cannot change them.

3. **The order is in one place at a time**
   - While it trades at its new price, it is no longer in its old level.

4. **Only the pool and the level write an order** (DD-003, need 5)

**Design**

- `modifyOrder()` hands a price change or a quantity rise to `moveOrder(slotIdx, newPrice, newQty)`.
- `moveOrder()` first takes the order out of its level with `remove`, which meets need 3.
- It then matches the order at its new price and quantity through `matchAggressor()`, the same code a new order uses, which meets need 1. If the new price does not reach the other side, the match loop stops at its first price check.
- If nothing is left, it erases the index entry and releases the slot. Otherwise `OrderPool::reprice()` writes the new price and the remaining quantity into the same slot, and `append` puts it at the back of its new level. This meets needs 2 and 4.
- `newQty` is the order's new remaining quantity, and fills during the move come off it.
- A modify emits only the fills it causes. How a modify reports its outcome is a separate decision.

**Other designs**

- **Free the slot and rest the order as a new one:** fails need 2.

<!--
Entry template

### DD-NNN: <the decision, stated as a choice>

**Status:** Keep | Keep (noted) | Revisit | Changed

**Where:** the types or functions involved, by name

**What**
The needs the decision must meet, as numbered points with short bullets, then the design and how it meets each need.
Describe the design on its own terms; do not critique an earlier version of the code.

**Other designs** (optional)
Each one, and which need it fails.
-->
