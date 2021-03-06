#include <app/Chunk.h>
#include <util/log.h>

#include <doctest.h>
#include <nanobench.h>

#include <tuple>
#include <unordered_map>

TEST_CASE("chunk_single") {
    auto chunkStore = buv::ChunkStore();

    auto* chunk = chunkStore.insert(12, 12345, nullptr);
    REQUIRE(chunk->next() == nullptr);
    auto* chunk2 = chunkStore.insert(13, 4444, chunk);
    REQUIRE(chunk != chunk2);
    REQUIRE(chunk->voutSatoshi() == buv::VoutSatoshi{12, 12345});
    REQUIRE(chunk->next() != nullptr);
    REQUIRE(chunk->next()->voutSatoshi() == buv::VoutSatoshi{13, 4444});

    // remove first entry
    auto [satoshi, newChunk] = chunkStore.remove(12, chunk);
    REQUIRE(satoshi == 12345);
    REQUIRE(newChunk == chunk2);

    // last entry moved forward
    REQUIRE(newChunk->voutSatoshi() == buv::VoutSatoshi{13, 4444});
    REQUIRE(newChunk->next() == nullptr);
    std::tie(satoshi, newChunk) = chunkStore.remove(13, newChunk);
    REQUIRE(newChunk == nullptr);
    REQUIRE(satoshi == 4444);
}

TEST_CASE("chunk_random") {
    auto voutAndSatoshi = std::vector<buv::VoutSatoshi>();

    auto rng = ankerl::nanobench::Rng(123);

    static constexpr auto maxNumElements = size_t(1000);
    static constexpr auto numTrials = size_t(1000);

    auto chunkStore = buv::ChunkStore();

    for (size_t trial = 0; trial < numTrials; ++trial) {

        auto numElements = static_cast<uint16_t>(rng.bounded(maxNumElements));
        for (uint16_t i = 0; i < numElements; ++i) {
            voutAndSatoshi.emplace_back(i, i * 1'000'000);
        }

        // now fill up chunks
        if (trial == 0) {
            REQUIRE(chunkStore.numAllocatedChunks() == 0);
            REQUIRE(chunkStore.numFreeChunks() == 0);
        } else {
            REQUIRE(chunkStore.numAllocatedChunks() == chunkStore.numFreeChunks());
        }

        buv::Chunk* baseChunk = nullptr;
        buv::Chunk* lastChunk = nullptr;
        for (auto const& vs : voutAndSatoshi) {
            lastChunk = chunkStore.insert(vs.vout(), vs.satoshi(), lastChunk);
            if (baseChunk == nullptr) {
                baseChunk = lastChunk;
            }
        }
        if (numElements > 1) {
            REQUIRE(lastChunk != baseChunk);
        } else {
            REQUIRE(lastChunk == baseChunk);
        }
        REQUIRE(chunkStore.numAllocatedChunks() == chunkStore.numChunksPerBulk());
        REQUIRE(chunkStore.numAllocatedChunks() - chunkStore.numFreeChunks() == voutAndSatoshi.size());

        // now that we have plenty of data, remove until empty and check that we get exactly the same result as the vector.
        buv::Chunk* newBaseChunk = baseChunk;
        while (!voutAndSatoshi.empty()) {
            auto idx = rng.bounded(voutAndSatoshi.size());
            auto removed = voutAndSatoshi[idx];
            voutAndSatoshi[idx] = voutAndSatoshi.back();
            voutAndSatoshi.pop_back();

            auto satoshi = int64_t();
            std::tie(satoshi, newBaseChunk) = chunkStore.remove(removed.vout(), newBaseChunk);

            NOLOG("{:>10} total, {:>10} free ({:4.1f}%)",
                  chunkStore.numAllocatedChunks(),
                  chunkStore.numFreeChunks(),
                  100.0 * chunkStore.numFreeChunks() / chunkStore.numAllocatedChunks());
            REQUIRE(chunkStore.numAllocatedChunks() - chunkStore.numFreeChunks() == voutAndSatoshi.size());

            if (voutAndSatoshi.empty()) {
                REQUIRE(newBaseChunk == nullptr);
            } else {
                REQUIRE(newBaseChunk != nullptr);
            }
            REQUIRE(satoshi == removed.satoshi());
        }
    }
}

static_assert(sizeof(buv::Chunk) == 16);
