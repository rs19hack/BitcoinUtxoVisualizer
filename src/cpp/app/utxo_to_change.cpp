#include <app/BlockEncoder.h>
#include <app/Cfg.h>
#include <app/Utxo.h>
#include <app/fetchAllBlockHashes.h>
#include <util/HttpClient.h>
#include <util/Throttle.h>
#include <util/args.h>
#include <util/hex.h>
#include <util/kbhit.h>
#include <util/log.h>
#include <util/parallelToSequential.h>
#include <util/reserve.h>
#include <util/rss.h>

#include <doctest.h>
#include <fmt/format.h>
#include <simdjson.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <string_view>

using namespace std::literals;

namespace {

// 5781.343 src/cpp/app/utxo_to_change.cpp(134) |     660105 height,    7788404 bytes,   6377.508 MB max RSS, utxo: (  80314186
// txids,  115288432 vout's used,  117964800 allocated (  18 bulk))

[[nodiscard]] auto integrateBlockData(simdjson::dom::element const& blockData, buv::Utxo& utxo) -> buv::ChangesInBlock {
    auto cib = buv::ChangesInBlock();
    auto& bd = cib.beginBlock(blockData["height"].get_uint64());

    bd.hash = util::fromHex<32>(blockData["hash"].get_string().value().data());
    bd.merkleRoot = util::fromHex<32>(blockData["merkleroot"].get_string().value().data());
    bd.chainWork = util::fromHex<32>(blockData["chainwork"].get_string().value().data());
    bd.difficulty(blockData["difficulty"].get_double());
    bd.version = blockData["version"].get_uint64();
    bd.time = blockData["time"].get_uint64().value();
    bd.medianTime = blockData["mediantime"].get_uint64().value();
    bd.nonce = blockData["nonce"].get_uint64();
    bd.bits = util::fromHex<4>(blockData["bits"].get_string().value().data());
    bd.nTx = blockData["nTx"].get_uint64();
    bd.size = blockData["size"].get_uint64();
    bd.strippedSize = blockData["strippedsize"].get_uint64();
    bd.weight = blockData["weight"].get_uint64();

    auto isCoinbaseTx = true;
    for (auto const& tx : blockData["tx"]) {
        // remove all inputs consumed by this transaction from utxo
        if (!isCoinbaseTx) {
            // first transaction is coinbase, has no inputs
            for (auto const& vin : tx["vin"]) {
                // txid & voutNr exactly define what is spent
                auto sourceTxid = util::fromHex<buv::txidPrefixSize>(vin["txid"].get_c_str());
                auto sourceVout = static_cast<uint16_t>(vin["vout"].get_uint64());

                // LOG("remove {} {}", util::toHex(sourceTxid), sourceVout);
                auto [satoshi, blockHeight] = utxo.remove(sourceTxid, sourceVout);

                // found an output that's spent! negative amount, because it's spent
                cib.addChange(-satoshi, blockHeight);
            }
        } else {
            isCoinbaseTx = false;
        }

        // add all outputs from this transaction to the utxo
        auto txid = util::fromHex<buv::txidPrefixSize>(tx["txid"].get_c_str());
        auto inserter = utxo.inserter(txid, bd.blockHeight);

        auto n = 0;
        for (auto const& vout : tx["vout"]) {
            auto sat = std::llround(vout["value"].get_double() * 100'000'000);
            // LOG("insert {} {}", util::toHex(txid), n);
            inserter.insert(n, sat);
            cib.addChange(sat, bd.blockHeight);
            ++n;
        }
    }
    cib.finalizeBlock();
    return cib;
}

struct ResourceData {
    std::unique_ptr<util::HttpClient> cli{};
    std::string jsonData{};
    simdjson::dom::parser jsonParser{};
    simdjson::dom::element blockData{};
};

} // namespace

TEST_CASE("utxo_to_change" * doctest::skip()) {
    auto cfg = buv::parseCfg(util::args::get("-cfg").value());

    auto cli = util::HttpClient::create(cfg.bitcoinRpcUrl.c_str());
    auto jsonParser = simdjson::dom::parser();

    auto allBlockHashes = buv::fetchAllBlockHashes(cli);
    LOG("got {} hashes", allBlockHashes.size());

    auto throttler = util::ThrottlePeriodic(1s);
    // auto utxoDumpThrottler = util::LogThrottler(20s);

    auto fout = std::ofstream(cfg.blkFile, std::ios::binary | std::ios::out);
    auto utxo = std::make_unique<buv::Utxo>();

    auto resources = std::vector<ResourceData>(std::thread::hardware_concurrency() * 2);
    for (auto& resource : resources) {
        resource.cli = util::HttpClient::create(cfg.bitcoinRpcUrl.c_str());
    }

    util::parallelToSequential(
        util::SequenceId{allBlockHashes.size()},
        util::ResourceId{resources.size()},
        util::ConcurrentWorkers{std::thread::hardware_concurrency()},

        [&](util::ResourceId resourceId, util::SequenceId sequenceId) {
            auto& res = resources[resourceId.count()];
            auto hash = util::toHex(allBlockHashes[sequenceId.count()]);

            res.jsonData = res.cli->get("/rest/block/{}.json", hash);
            res.blockData = res.jsonParser.parse(res.jsonData);
        },
        [&](util::ResourceId resourceId, util::SequenceId /*sequenceId*/) {
            auto& res = resources[resourceId.count()];
            auto cib = integrateBlockData(res.blockData, *utxo);
            fout << cib.encode();

            // free the memory of the resource. Also helps find bugs (operating on old data. Not that it has ever happened, but
            // still)
            res.jsonData = std::string();

            if (throttler()) {
                if (util::kbhit()) {
                    switch (std::getchar()) {
                    case 'q':
                        buv::serialize(cib.blockData().blockHeight, *utxo, "utxo.dat");
                    }
                } else {
                    LOG("{:10} height {:10.3f} MB max RSS, utxo: {}",
                        cib.blockData().blockHeight,
                        util::maxRss() / 1048576.0,
                        *utxo);
                }
            }
        });

    LOG("Done! utxo: {:d}", *utxo);
}
