#pragma once
#include <string>
#include <vector>
#include <stdexcept>
#include "Types.hpp"
#include "ArrayBitMapLocator.hpp"

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

    std::vector<InstrumentConfig> loadInstrumentConfig(const std::string &path);

}