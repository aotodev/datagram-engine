/**
 * @file fuzz_demux.cpp
 * @brief Fuzzer for the payload key projection and the flow table.
 *
 * Two attacker-facing surfaces. `by_payload_id` reads bytes at a fixed offset
 * out of a datagram, so a short or empty payload must yield no key rather than
 * a read past the end. The flow table is not attacker-facing in the same way,
 * but its probe chains and backward-shift deletion are the kind of index
 * arithmetic that is wrong in one corner and correct everywhere else, so it is
 * driven differentially against a reference map.
 *
 * The input is an opcode stream: one byte of operation, one of key, and the
 * projection is run over whatever follows.
 */
#include <cstddef>
#include <cstdint>
#include <cstring>

import std;
import libmem;
import dgram;

namespace {

constexpr std::size_t slots{64};
using key_type = dgram::byte_key<4>;
using table_type = dgram::flow_table<key_type, std::uint32_t, slots>;

key_type key_of(const std::uint8_t id) noexcept {
    const std::array<std::byte, 1> raw{static_cast<std::byte>(id)};
    return key_type{raw};
}

/** The projection, over a payload sized exactly to the input. */
void project(const std::span<const std::byte> payload) noexcept {
    const dgram::metadata<dgram::no_features> meta{};
    const dgram::endpoint from{};
    const dgram::arrival<dgram::no_features> a{payload, from, meta, std::chrono::nanoseconds{0}};

    constexpr dgram::by_payload_id<3, 8> projection{};
    if (const auto key{projection(a)}) {
        if (key->bytes().size() != 8) {
            std::abort(); // a key that exists must be the requested length
        }
        if (payload.size() < 11) {
            std::abort(); // and must only exist when the bytes were there
        }
        (void)hash_value(*key);
    } else if (payload.size() >= 11) {
        std::abort(); // a long enough payload must always yield a key
    }
}

/** Drive the table against a reference and check they agree at every step. */
void exercise_table(const std::span<const std::byte> ops) noexcept {
    libmem::arena arena{table_type::footprint()};
    auto table{table_type::carve(arena)};
    if (!table) {
        return;
    }
    std::map<std::uint8_t, std::uint32_t> reference{};

    for (std::size_t i{}; i + 1 < ops.size(); i += 2) {
        const auto op{static_cast<std::uint8_t>(ops[i])};
        const auto id{static_cast<std::uint8_t>(ops[i + 1])};
        const auto key{key_of(id)};

        switch (op % 3) {
        case 0: {
            const bool would_fit{reference.size() < table_type::max_size || reference.contains(id)};
            auto* stored{table->insert(key, id)};
            if (would_fit) {
                if (stored == nullptr) {
                    std::abort(); // room was available, so insert must succeed
                }
                reference[id] = id;
            } else if (stored != nullptr) {
                std::abort(); // a full table must refuse
            }
            break;
        }
        case 1: {
            const bool erased{table->erase(key)};
            if (erased != (reference.erase(id) == 1)) {
                std::abort();
            }
            break;
        }
        default: {
            const auto* found{std::as_const(*table).find(key)};
            const auto expected{reference.find(id)};
            if ((found != nullptr) != (expected != reference.end())) {
                std::abort(); // lookups must agree with the reference
            }
            if (found != nullptr && *found != expected->second) {
                std::abort();
            }
            break;
        }
        }

        if (table->size() != reference.size()) {
            std::abort(); // a lost or duplicated entry
        }
    }

    // Everything the reference still holds must be findable: the probe chains
    // must have survived every backward shift.
    for (const auto& [id, value] : reference) {
        const auto* found{std::as_const(*table).find(key_of(id))};
        if (found == nullptr || *found != value) {
            std::abort();
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const auto selector{data[0]};
    const auto body_len{size - 1};
    if (body_len == 0) {
        project({});
        return 0;
    }

    // Sized to the input exactly, so a projection overread lands in a redzone.
    auto* buf{static_cast<std::byte*>(::operator new(body_len))};
    std::memcpy(buf, data + 1, body_len);
    const std::span<const std::byte> body{buf, body_len};

    if ((selector & 1U) != 0U) {
        exercise_table(body);
    } else {
        project(body);
    }

    ::operator delete(buf, body_len);
    return 0;
}
