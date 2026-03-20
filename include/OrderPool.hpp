#include "Types.hpp"
#include <cstddef>

namespace engine::book
{

    struct alignas(64) PoolOrder
    {
        types::OrderId orderId;
        types::Verb verb;
        types::ClientId clientId;
        types::Price price;
        types::Quantity qty;
        types::TimeInForce tif;
        std::uint32_t next;
        std::uint32_t prev;
    };

}
