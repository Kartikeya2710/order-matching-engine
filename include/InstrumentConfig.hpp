#pragma once
#include "ArrayBitMapLocator.hpp"
#include "Types.hpp"
#include <string>
#include <vector>

namespace engine
{

    enum class BookType : uint8_t
    {
        FastBook = 0,
    };

    struct InstrumentConfig
    {
        types::InstrumentId instrumentId;
        BookType bookType;
        book::PriceRange priceRange;
    };

    std::vector<InstrumentConfig> loadInstrumentConfig(const std::string& path);

} // namespace engine