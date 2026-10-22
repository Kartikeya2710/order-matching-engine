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

| ID                                                                                        | Decision                                                                      | Status       |
| ----------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------- | ------------ |
| [DD-001](#dd-001-the-gateway-checks-message-structure-the-engine-checks-values-and-state) | The gateway checks message structure; the engine checks values and book state | Keep         |
| [DD-002](#dd-002-one-struct-per-request-type-carried-in-a-stdvariant)                     | One struct per request type, carried in a `std::variant`                      | Changed      |
| [DD-003](#dd-003-price-levels-change-only-through-append-remove-and-reduce)               | Price levels change only through `append`, `remove` and `reduce`              | Keep (noted) |

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

**Where:** `ArrayBitMapLocator`, `PriceLevel`, `PoolSlot`, `OrderPool::acquire`, `OrderPool::release`, `OrderPool::slots`, `OrderBook`

**What**

The orders themselves live in the order pool's slots, and each level's queue is a doubly linked list threaded through those slots. So a single change to the book touches three things that must agree: the slot's queue links, the level's head, tail and totals, and the level's bit (in case of bitmap-based level locator).

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
   - No code other than `OrderPool`, when it acquires a slot, and `PriceLevel`, while the order rests, may write an order's fields.

**Design**

Four types, one job each:

1. **`OrderPool`**
   - Hands out and takes back slots.
   - `acquire(Order)` takes a free slot and writes the order into it. It is the only way to write an order, so a slot cannot be acquired without an order, written without being acquired, or rewritten while it is live.
   - `OrderBook` holds the pool as a private member, so it is the only code that can acquire a slot.
   - The pool's only addition for the levels is `slots()`, which returns all slots as a `std::span<PoolSlot>` for `PriceLevel` to change.

2. **`PoolSlot`**
   - An `Order` (the order's ID, side, client, price, `qty` and TIF) plus the slot indexes of its neighbours in its level's queue (`next` and `prev`).
   - Only `OrderPool` writes the whole slot in `acquire`
   - `PriceLevel` changes `qty`, `next` and `prev` while the order rests.
   - Copy assignment is deleted, so code holding `slots()` cannot overwrite a slot.

3. **`PriceLevel`**
   - One price's queue: `head`, `tail`, `totalQty` and `orderCount`.
   - Its `append`, `remove` and `reduce` change the links, the order's `qty` and the level's totals together.
   - In debug builds, `remove` asserts that the order's neighbours, or the level's `head` and `tail`, point back at it.

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

**Other designs**

- **Public `PoolSlot` fields, or a mutable `OrderPool::operator[]`:** fails need 5. Any code holding a slot could change a resting order's price or side, leaving it in the wrong level, or its `qty`, leaving `totalQty` out of sync.
- **Separate `acquire()` and `fill(slotIdx, Order)`:** a slot could be acquired without being filled, filled without being acquired, or rewritten while it is live. Asserts could catch only the first two, and would need state per slot to do it.
- **Two parts in `PoolSlot`, each with one friend** (`qty`, `next` and `prev` for `PriceLevel`; the rest for `OrderPool`): would also stop `PriceLevel` from writing an order's ID, side or price, but adds a type and longer call sites (`order.levelEntry.qty()`) to guard the code already trusted with the queue.
- **Pointer links:** Using pointers makes it more difficult to capture snapshots of the order book very efficiently. It's a widely adopted design.
- **Mutable access to levels, with the book setting and clearing bits itself:** fails need 1. Every call site has to remember the bit.
- **One `edit(side, price, change)` that runs a callback on the level and re-syncs the bit afterwards:** fails need 3. The callback can make any change, and the call sites are harder to read than three named operations.
- **Queue logic in `OrderPool`, or the pool owning the links:** fails need 4.
- **The locator stores a reference to the pool:** The locator must not be able to acquire or release slots.

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
