#include "InstrumentConfig.hpp"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace engine
{

    static BookType parseBookType(const std::string &s, int lineNo)
    {
        if (s == "FastBook")
            return BookType::FastBook;
        throw std::runtime_error(
            "instruments.cfg line " + std::to_string(lineNo) +
            ": unknown book type '" + s + "' (valid: FastBook)");
    }

    static void validateRange(const book::PriceRange &r, int lineNo)
    {
        if (r.tickSize == 0)
            throw std::runtime_error(
                "instruments.cfg line " + std::to_string(lineNo) +
                ": tick_size must be > 0");
        if (r.minPrice >= r.maxPrice)
            throw std::runtime_error(
                "instruments.cfg line " + std::to_string(lineNo) +
                ": min_price must be < max_price");
        if ((r.maxPrice - r.minPrice) % r.tickSize != 0)
            throw std::runtime_error(
                "instruments.cfg line " + std::to_string(lineNo) +
                ": (max_price - min_price) must be divisible by tick_size");
    }

    std::vector<InstrumentConfig> loadInstrumentConfig(const std::string &path)
    {
        std::ifstream file(path);
        if (!file.is_open())
            throw std::runtime_error("cannot open config file: " + path);

        std::vector<InstrumentConfig> configs;
        std::string line;
        int lineNo = 0;

        while (std::getline(file, line))
        {
            ++lineNo;

            auto commentPos = line.find('#');
            if (commentPos != std::string::npos)
                line.erase(commentPos);

            std::istringstream iss(line);
            std::string token;
            if (!(iss >> token))
                continue;

            InstrumentConfig cfg{};

            try
            {
                cfg.instrumentId = static_cast<types::InstrumentId>(std::stoul(token));
            }
            catch (...)
            {
                throw std::runtime_error(
                    "instruments.cfg line " + std::to_string(lineNo) +
                    ": invalid instrument_id '" + token + "'");
            }

            std::string bookTypeStr;
            if (!(iss >> bookTypeStr))
                throw std::runtime_error(
                    "instruments.cfg line " + std::to_string(lineNo) +
                    ": missing book_type");
            cfg.bookType = parseBookType(bookTypeStr, lineNo);

            auto readPrice = [&](const char *fieldName) -> types::Price
            {
                std::string s;
                if (!(iss >> s))
                    throw std::runtime_error(
                        "instruments.cfg line " + std::to_string(lineNo) +
                        ": missing " + fieldName);
                try
                {
                    return static_cast<types::Price>(std::stoul(s));
                }
                catch (...)
                {
                    throw std::runtime_error(
                        "instruments.cfg line " + std::to_string(lineNo) +
                        ": invalid " + fieldName + " '" + s + "'");
                }
            };

            cfg.priceRange.minPrice = readPrice("min_price");
            cfg.priceRange.maxPrice = readPrice("max_price");
            cfg.priceRange.tickSize = readPrice("tick_size");

            validateRange(cfg.priceRange, lineNo);

            for (const auto &existing : configs)
            {
                if (existing.instrumentId == cfg.instrumentId)
                    throw std::runtime_error(
                        "instruments.cfg line " + std::to_string(lineNo) +
                        ": duplicate instrument_id " +
                        std::to_string(cfg.instrumentId));
            }

            configs.push_back(cfg);
        }

        if (configs.empty())
            throw std::runtime_error(
                "instruments.cfg: no instruments defined in " + path);

        return configs;
    }

}