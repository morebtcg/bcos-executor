/**
 *  Copyright (C) 2021 FISCO BCOS.
 *  SPDX-License-Identifier: Apache-2.0
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */
/**
 * @brief : unitest for DAG executor implementation
 * @author: catli
 * @date: 2021-10-27
 */

#include "../mock/MockTransactionalStorage.h"
#include "../mock/MockTxPool.h"
#include "Common.h"
#include "bcos-executor/TransactionExecutor.h"
#include "interfaces/crypto/CommonType.h"
#include "interfaces/crypto/CryptoSuite.h"
#include "interfaces/crypto/Hash.h"
#include "interfaces/executor/ExecutionMessage.h"
#include "interfaces/protocol/Transaction.h"
#include "libprotocol/protobuf/PBBlockHeader.h"
#include "libstorage/StateStorage.h"
#include "precompiled/ParallelConfigPrecompiled.h"
#include "precompiled/PrecompiledCodec.h"
#include "precompiled/Utilities.h"
#include <bcos-framework/libexecutor/NativeExecutionMessage.h>
#include <bcos-framework/testutils/crypto/HashImpl.h>
#include <bcos-framework/testutils/crypto/SignatureImpl.h>
#include <bcos-framework/testutils/protocol/FakeBlockHeader.h>
#include <bcos-framework/testutils/protocol/FakeTransaction.h>
#include <unistd.h>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/test/tools/old/interface.hpp>
#include <boost/test/unit_test.hpp>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <set>

using namespace std;
using namespace bcos;
using namespace bcos::executor;
using namespace bcos::storage;
using namespace bcos::precompiled;

namespace bcos
{
namespace test
{
struct DagExecutorFixture
{
    DagExecutorFixture()
    {
        hashImpl = std::make_shared<Keccak256Hash>();
        assert(hashImpl);
        auto signatureImpl = std::make_shared<Secp256k1SignatureImpl>();
        assert(signatureImpl);
        cryptoSuite = std::make_shared<CryptoSuite>(hashImpl, signatureImpl, nullptr);

        txpool = std::make_shared<MockTxPool>();
        backend = std::make_shared<MockTransactionalStorage>(hashImpl);

        keyPair = cryptoSuite->signatureImpl()->generateKeyPair();
        memcpy(keyPair->secretKey()->mutableData(),
            fromHexString("ff6f30856ad3bae00b1169808488502786a13e3c174d85682135ffd51310310e")
                ->data(),
            32);
        memcpy(keyPair->publicKey()->mutableData(),
            fromHexString(
                "ccd8de502ac45462767e649b462b5f4ca7eadd69c7e1f1b410bdf754359be29b1b88ffd79744"
                "03f56e250af52b25682014554f7b3297d6152401e85d426a06ae")
                ->data(),
            64);
        createSysTable();
    }

    void createSysTable()
    {
        // create / table
        std::promise<std::optional<Table>> promise2;
        backend->asyncCreateTable(
            "/", FS_FIELD_COMBINED, [&](Error::UniquePtr&& _error, std::optional<Table>&& _table) {
                BOOST_CHECK(!_error);
                promise2.set_value(std::move(_table));
            });

        // create /apps table
        std::promise<std::optional<Table>> promise3;
        backend->asyncCreateTable("/apps", FS_FIELD_COMBINED,
            [&](Error::UniquePtr&& _error, std::optional<Table>&& _table) {
                BOOST_CHECK(!_error);
                promise3.set_value(std::move(_table));
            });
        promise3.get_future().get();
        auto rootTable = promise2.get_future().get();
        assert(rootTable != std::nullopt);
        auto dirEntry = rootTable->newEntry();
        dirEntry.setField(FS_FIELD_TYPE, FS_TYPE_DIR);
        dirEntry.setField(FS_FIELD_EXTRA, "");
        rootTable->setRow("apps", dirEntry);
        rootTable->setRow("/", dirEntry);
    }
    CryptoSuite::Ptr cryptoSuite;
    std::shared_ptr<MockTxPool> txpool;
    std::shared_ptr<MockTransactionalStorage> backend;
    std::shared_ptr<Keccak256Hash> hashImpl;

    KeyPairInterface::Ptr keyPair;
    int64_t gas = 3000000000;
};
BOOST_FIXTURE_TEST_SUITE(TestDagExecutor, DagExecutorFixture)

BOOST_AUTO_TEST_CASE(callWasmConcurrentlyTransfer)
{
    auto executionResultFactory = std::make_shared<NativeExecutionMessageFactory>();
    auto executor = std::make_shared<TransactionExecutor>(
        txpool, nullptr, backend, executionResultFactory, hashImpl, true);
    auto codec = std::make_unique<bcos::precompiled::PrecompiledCodec>(hashImpl, true);

    string transferPath = "../test/liquid/transfer.wasm";
    auto in = ifstream(transferPath, ios::binary | ios::ate);
    BOOST_CHECK(in.is_open());
    auto size = in.tellg();
    BOOST_CHECK(size != 0);

    bytes transferBin;
    in.seekg(0, std::ios::beg);
    transferBin.resize(size);
    BOOST_CHECK(in.read(reinterpret_cast<char*>(transferBin.data()), size));
    transferBin = codec->encode(transferBin);
    auto transferAbi = codec->encode(string(
        R"([{"inputs":[],"type":"constructor"},{"conflictFields":[{"kind":3,"path":[0],"read_only":false,"slot":0},{"kind":3,"path":[1],"read_only":false,"slot":0}],"constant":false,"inputs":[{"internalType":"string","name":"from","type":"string"},{"internalType":"string","name":"to","type":"string"},{"internalType":"uint32","name":"amount","type":"uint32"}],"name":"transfer","outputs":[{"internalType":"bool","type":"bool"}],"type":"function"},{"constant":true,"inputs":[{"internalType":"string","name":"name","type":"string"}],"name":"query","outputs":[{"internalType":"uint32","type":"uint32"}],"type":"function"}])"));

    bytes input;
    input.push_back(0);
    input.insert(input.end(), transferBin.begin(), transferBin.end());
    input.push_back(0);

    string transferAddress = "/usr/alice/transfer";

    input.insert(input.end(), transferAbi.begin(), transferAbi.end());

    auto tx = fakeTransaction(cryptoSuite, keyPair, "", input, 101, 100001, "1", "1");
    auto sender = boost::algorithm::hex_lower(std::string(tx->sender()));

    auto hash = tx->hash();
    txpool->hash2Transaction.emplace(hash, tx);

    auto params = std::make_unique<NativeExecutionMessage>();
    params->setContextID(99);
    params->setSeq(1000);
    params->setDepth(0);
    params->setOrigin(std::string(sender));
    params->setFrom(std::string(sender));
    params->setTo(transferAddress);
    params->setStaticCall(false);
    params->setGasAvailable(gas);
    params->setType(NativeExecutionMessage::TXHASH);
    params->setTransactionHash(hash);
    params->setCreate(true);

    NativeExecutionMessage paramsBak = *params;

    auto blockHeader = std::make_shared<bcos::protocol::PBBlockHeader>(cryptoSuite);
    blockHeader->setNumber(1);

    std::promise<void> nextPromise;
    executor->nextBlockHeader(blockHeader, [&](bcos::Error::Ptr&& error) {
        BOOST_CHECK(!error);
        nextPromise.set_value();
    });
    nextPromise.get_future().get();

    // --------------------------------
    // Create contract transfer
    // --------------------------------
    std::promise<bcos::protocol::ExecutionMessage::UniquePtr> executePromise;
    executor->executeTransaction(std::move(params),
        [&](bcos::Error::UniquePtr&& error, bcos::protocol::ExecutionMessage::UniquePtr&& result) {
            BOOST_CHECK(!error);
            executePromise.set_value(std::move(result));
        });

    auto result = executePromise.get_future().get();

    BOOST_CHECK_EQUAL(result->status(), 0);
    BOOST_CHECK_EQUAL(result->origin(), sender);
    BOOST_CHECK_EQUAL(result->from(), paramsBak.to());
    BOOST_CHECK_EQUAL(result->to(), sender);

    BOOST_CHECK(result->message().empty());
    BOOST_CHECK(!result->newEVMContractAddress().empty());
    BOOST_CHECK_LT(result->gasAvailable(), gas);

    auto address = result->newEVMContractAddress();

    bcos::executor::TransactionExecutor::TwoPCParams commitParams;
    commitParams.number = 1;

    std::promise<void> preparePromise;
    executor->prepare(commitParams, [&](bcos::Error::Ptr&& error) {
        BOOST_CHECK(!error);
        preparePromise.set_value();
    });
    preparePromise.get_future().get();

    std::promise<void> commitPromise;
    executor->commit(commitParams, [&](bcos::Error::Ptr&& error) {
        BOOST_CHECK(!error);
        commitPromise.set_value();
    });
    commitPromise.get_future().get();
    auto tableName = std::string("/apps") + string(address);

    EXECUTOR_LOG(TRACE) << "Checking table: " << tableName;
    std::promise<Table> tablePromise;
    backend->asyncOpenTable(tableName, [&](Error::UniquePtr&& error, std::optional<Table>&& table) {
        BOOST_CHECK(!error);
        BOOST_CHECK(table);
        tablePromise.set_value(std::move(*table));
    });
    auto table = tablePromise.get_future().get();

    auto entry = table.getRow("code");
    BOOST_CHECK(entry);
    BOOST_CHECK_GT(entry->getField(STORAGE_VALUE).size(), 0);

    entry = table.getRow("abi");
    BOOST_CHECK(entry);
    BOOST_CHECK_GT(entry->getField(STORAGE_VALUE).size(), 0);

    std::vector<ExecutionMessage::UniquePtr> requests;
    auto cases = vector<tuple<string, string, uint32_t>>();

    cases.push_back(make_tuple("alice", "bob", 1000));
    cases.push_back(make_tuple("charlie", "david", 2000));
    cases.push_back(make_tuple("bob", "david", 200));
    cases.push_back(make_tuple("david", "alice", 400));

    for (size_t i = 0; i < cases.size(); ++i)
    {
        std::string from = std::get<0>(cases[i]);
        std::string to = std::get<1>(cases[i]);
        uint32_t amount = std::get<2>(cases[i]);
        bytes input;
        input.push_back(1);
        auto encodedParams =
            codec->encodeWithSig("transfer(string,string,uint32)", from, to, amount);
        input.insert(input.end(), encodedParams.begin(), encodedParams.end());

        auto tx = fakeTransaction(cryptoSuite, keyPair, address, input, 101 + i, 100001, "1", "1");
        auto sender = boost::algorithm::hex_lower(std::string(tx->sender()));

        auto hash = tx->hash();
        txpool->hash2Transaction.emplace(hash, tx);

        auto params = std::make_unique<NativeExecutionMessage>();
        params->setType(bcos::protocol::ExecutionMessage::TXHASH);
        params->setContextID(i);
        params->setSeq(6000);
        params->setDepth(0);
        params->setFrom(std::string(sender));
        params->setTo(std::string(address));
        params->setOrigin(std::string(sender));
        params->setStaticCall(false);
        params->setGasAvailable(gas);
        params->setTransactionHash(hash);
        params->setCreate(false);

        requests.emplace_back(std::move(params));
    }

    executor->dagExecuteTransactions(
        requests, [&](bcos::Error::UniquePtr error,
                      std::vector<bcos::protocol::ExecutionMessage::UniquePtr> results) {
            BOOST_CHECK(!error);

            vector<pair<string, uint32_t>> expected;
            expected.push_back({"alice", numeric_limits<uint32_t>::max() - 1000 + 400});
            expected.push_back({"bob", 1000 - 200});
            expected.push_back({"charlie", numeric_limits<uint32_t>::max() - 2000});
            expected.push_back({"david", 2000 + 200 - 400});

            for (size_t i = 0; i < results.size(); ++i)
            {
                auto& result = results[i];
                BOOST_CHECK_EQUAL(result->status(), 0);
                BOOST_CHECK(result->message().empty());
                bool flag;
                codec->decode(result->data(), flag);
                BOOST_CHECK(flag);
            }

            for (size_t i = 0; i < expected.size(); ++i)
            {
                bytes queryBytes;
                queryBytes.push_back(1);
                auto encodedParams =
                    codec->encodeWithSig("query(string)", std::get<0>(expected[i]));
                queryBytes.insert(queryBytes.end(), encodedParams.begin(), encodedParams.end());

                auto params = std::make_unique<NativeExecutionMessage>();
                params->setContextID(888 + i);
                params->setSeq(999);
                params->setDepth(0);
                params->setFrom(std::string(sender));
                params->setTo(transferAddress);
                params->setOrigin(std::string(sender));
                params->setStaticCall(false);
                params->setGasAvailable(gas);
                params->setData(std::move(queryBytes));
                params->setType(ExecutionMessage::MESSAGE);

                std::promise<ExecutionMessage::UniquePtr> executePromise;
                executor->executeTransaction(
                    std::move(params), [&executePromise](bcos::Error::UniquePtr&& error,
                                           ExecutionMessage::UniquePtr&& result) {
                        BOOST_CHECK(!error);
                        executePromise.set_value(std::move(result));
                    });
                result = executePromise.get_future().get();

                BOOST_CHECK(result);
                BOOST_CHECK_EQUAL(result->status(), 0);
                BOOST_CHECK_EQUAL(result->message(), "");
                BOOST_CHECK_EQUAL(result->newEVMContractAddress(), "");
                BOOST_CHECK_LT(result->gasAvailable(), gas);

                uint32_t dept;
                codec->decode(result->data(), dept);
                BOOST_CHECK_EQUAL(dept, std::get<1>(expected[i]));
            }
        });
}  // namespace test

BOOST_AUTO_TEST_CASE(callWasmConcurrentlyHelloWorld)
{
    auto executionResultFactory = std::make_shared<NativeExecutionMessageFactory>();
    auto executor = std::make_shared<TransactionExecutor>(
        txpool, nullptr, backend, executionResultFactory, hashImpl, true);
    auto codec = std::make_unique<bcos::precompiled::PrecompiledCodec>(hashImpl, true);

    string helloWorldPath = "../test/liquid/hello_world.wasm";
    auto in = ifstream(helloWorldPath, ios::binary | ios::ate);
    BOOST_CHECK(in.is_open());
    auto size = in.tellg();
    BOOST_CHECK(size != 0);

    bytes helloWorldBin;
    in.seekg(0, std::ios::beg);
    helloWorldBin.resize(size);
    BOOST_CHECK(in.read(reinterpret_cast<char*>(helloWorldBin.data()), size));
    helloWorldBin = codec->encode(helloWorldBin);
    auto helloWorldAbi = codec->encode(string(
        R"([{"inputs":[{"internalType":"string","name":"name","type":"string"}],"type":"constructor"},{"conflictFields":[{"kind":0,"path":[],"read_only":false,"slot":0}],"constant":false,"inputs":[{"internalType":"string","name":"name","type":"string"}],"name":"set","outputs":[],"type":"function"},{"constant":true,"inputs":[],"name":"get","outputs":[{"internalType":"string","type":"string"}],"type":"function"}])"));

    bytes input;
    input.push_back(0);
    input.insert(input.end(), helloWorldBin.begin(), helloWorldBin.end());

    bytes constructorParam = codec->encode(string("alice"));
    constructorParam = codec->encode(constructorParam);
    input.insert(input.end(), constructorParam.begin(), constructorParam.end());

    string helloWorldAddress = "/usr/alice/hello_world";

    input.insert(input.end(), helloWorldAbi.begin(), helloWorldAbi.end());

    auto tx = fakeTransaction(cryptoSuite, keyPair, "", input, 101, 100001, "1", "1");
    auto sender = boost::algorithm::hex_lower(std::string(tx->sender()));

    auto hash = tx->hash();
    txpool->hash2Transaction.emplace(hash, tx);

    auto params = std::make_unique<NativeExecutionMessage>();
    params->setContextID(99);
    params->setSeq(1000);
    params->setDepth(0);
    params->setOrigin(std::string(sender));
    params->setFrom(std::string(sender));
    params->setTo(helloWorldAddress);
    params->setStaticCall(false);
    params->setGasAvailable(gas);
    params->setType(NativeExecutionMessage::TXHASH);
    params->setTransactionHash(hash);
    params->setCreate(true);

    NativeExecutionMessage paramsBak = *params;

    auto blockHeader = std::make_shared<bcos::protocol::PBBlockHeader>(cryptoSuite);
    blockHeader->setNumber(1);

    std::promise<void> nextPromise;
    executor->nextBlockHeader(blockHeader, [&](bcos::Error::Ptr&& error) {
        BOOST_CHECK(!error);
        nextPromise.set_value();
    });
    nextPromise.get_future().get();

    // --------------------------------
    // Create contract hello world
    // --------------------------------
    std::promise<bcos::protocol::ExecutionMessage::UniquePtr> executePromise;
    executor->executeTransaction(std::move(params),
        [&](bcos::Error::UniquePtr&& error, bcos::protocol::ExecutionMessage::UniquePtr&& result) {
            BOOST_CHECK(!error);
            executePromise.set_value(std::move(result));
        });

    auto result = executePromise.get_future().get();

    BOOST_CHECK_EQUAL(result->status(), 0);
    BOOST_CHECK_EQUAL(result->origin(), sender);
    BOOST_CHECK_EQUAL(result->from(), paramsBak.to());
    BOOST_CHECK_EQUAL(result->to(), sender);

    BOOST_CHECK(result->message().empty());
    BOOST_CHECK(!result->newEVMContractAddress().empty());
    BOOST_CHECK_LT(result->gasAvailable(), gas);

    auto address = result->newEVMContractAddress();

    bcos::executor::TransactionExecutor::TwoPCParams commitParams;
    commitParams.number = 1;

    std::promise<void> preparePromise;
    executor->prepare(commitParams, [&](bcos::Error::Ptr&& error) {
        BOOST_CHECK(!error);
        preparePromise.set_value();
    });
    preparePromise.get_future().get();

    std::promise<void> commitPromise;
    executor->commit(commitParams, [&](bcos::Error::Ptr&& error) {
        BOOST_CHECK(!error);
        commitPromise.set_value();
    });
    commitPromise.get_future().get();
    auto tableName = std::string("/apps") + string(address);

    EXECUTOR_LOG(TRACE) << "Checking table: " << tableName;
    std::promise<Table> tablePromise;
    backend->asyncOpenTable(tableName, [&](Error::UniquePtr&& error, std::optional<Table>&& table) {
        BOOST_CHECK(!error);
        BOOST_CHECK(table);
        tablePromise.set_value(std::move(*table));
    });
    auto table = tablePromise.get_future().get();

    auto entry = table.getRow("code");
    BOOST_CHECK(entry);
    BOOST_CHECK_GT(entry->getField(STORAGE_VALUE).size(), 0);

    entry = table.getRow("abi");
    BOOST_CHECK(entry);
    BOOST_CHECK_GT(entry->getField(STORAGE_VALUE).size(), 0);

    std::vector<ExecutionMessage::UniquePtr> requests;
    auto cases = vector<string>();

    cases.push_back("alice");
    cases.push_back("charlie");
    cases.push_back("bob");
    cases.push_back("david");

    for (size_t i = 0; i < cases.size(); ++i)
    {
        std::string name = cases[i];
        bytes input;
        input.push_back(1);

        auto encodedParams = codec->encodeWithSig("set(string)", name);
        input.insert(input.end(), encodedParams.begin(), encodedParams.end());

        auto tx = fakeTransaction(cryptoSuite, keyPair, address, input, 101 + i, 100001, "1", "1");
        auto sender = boost::algorithm::hex_lower(std::string(tx->sender()));

        auto hash = tx->hash();
        txpool->hash2Transaction.emplace(hash, tx);

        auto params = std::make_unique<NativeExecutionMessage>();
        params->setType(bcos::protocol::ExecutionMessage::TXHASH);
        params->setContextID(i);
        params->setSeq(6000);
        params->setDepth(0);
        params->setFrom(std::string(sender));
        params->setTo(std::string(address));
        params->setOrigin(std::string(sender));
        params->setStaticCall(false);
        params->setGasAvailable(gas);
        params->setTransactionHash(hash);
        params->setCreate(false);

        requests.emplace_back(std::move(params));
    }

    executor->dagExecuteTransactions(
        requests, [&](bcos::Error::UniquePtr error,
                      std::vector<bcos::protocol::ExecutionMessage::UniquePtr> results) {
            BOOST_CHECK(!error);

            for (size_t i = 0; i < results.size(); ++i)
            {
                auto& result = results[i];
                BOOST_CHECK_EQUAL(result->status(), 0);
                BOOST_CHECK(result->message().empty());
            }


            bytes getBytes;
            getBytes.push_back(1);

            auto encodedParams = codec->encodeWithSig("get()");
            getBytes.insert(getBytes.end(), encodedParams.begin(), encodedParams.end());

            auto params = std::make_unique<NativeExecutionMessage>();
            params->setContextID(888);
            params->setSeq(999);
            params->setDepth(0);
            params->setFrom(std::string(sender));
            params->setTo(helloWorldAddress);
            params->setOrigin(std::string(sender));
            params->setStaticCall(false);
            params->setGasAvailable(gas);
            params->setData(std::move(getBytes));
            params->setType(ExecutionMessage::MESSAGE);

            std::promise<ExecutionMessage::UniquePtr> executePromise;
            executor->executeTransaction(
                std::move(params), [&executePromise](bcos::Error::UniquePtr&& error,
                                       ExecutionMessage::UniquePtr&& result) {
                    BOOST_CHECK(!error);
                    executePromise.set_value(std::move(result));
                });
            result = executePromise.get_future().get();

            BOOST_CHECK(result);
            BOOST_CHECK_EQUAL(result->status(), 0);
            BOOST_CHECK_EQUAL(result->message(), "");
            BOOST_CHECK_EQUAL(result->newEVMContractAddress(), "");
            BOOST_CHECK_LT(result->gasAvailable(), gas);

            string name;
            codec->decode(result->data(), name);
            BOOST_CHECK_EQUAL(name, "david");
        });
}

BOOST_AUTO_TEST_CASE(callEvmConcurrentlyTransfer)
{
    size_t count = 100;
    auto executionResultFactory = std::make_shared<NativeExecutionMessageFactory>();
    auto executor = std::make_shared<TransactionExecutor>(
        txpool, nullptr, backend, executionResultFactory, hashImpl, false);
    auto codec = std::make_unique<bcos::precompiled::PrecompiledCodec>(hashImpl, false);

    std::string bin =
        "608060405234801561001057600080fd5b506105db806100206000396000f30060806040526004361061006257"
        "6000357c0100000000000000000000000000000000000000000000000000000000900463ffffffff16806335ee"
        "5f87146100675780638a42ebe9146100e45780639b80b05014610157578063fad42f8714610210575b600080fd"
        "5b34801561007357600080fd5b506100ce60048036038101908080359060200190820180359060200190808060"
        "1f0160208091040260200160405190810160405280939291908181526020018383808284378201915050505050"
        "5091929192905050506102c9565b6040518082815260200191505060405180910390f35b3480156100f0576000"
        "80fd5b50610155600480360381019080803590602001908201803590602001908080601f016020809104026020"
        "016040519081016040528093929190818152602001838380828437820191505050505050919291929080359060"
        "20019092919050505061033d565b005b34801561016357600080fd5b5061020e60048036038101908080359060"
        "2001908201803590602001908080601f0160208091040260200160405190810160405280939291908181526020"
        "018383808284378201915050505050509192919290803590602001908201803590602001908080601f01602080"
        "910402602001604051908101604052809392919081815260200183838082843782019150505050505091929192"
        "90803590602001909291905050506103b1565b005b34801561021c57600080fd5b506102c76004803603810190"
        "80803590602001908201803590602001908080601f016020809104026020016040519081016040528093929190"
        "818152602001838380828437820191505050505050919291929080359060200190820180359060200190808060"
        "1f0160208091040260200160405190810160405280939291908181526020018383808284378201915050505050"
        "509192919290803590602001909291905050506104a8565b005b60008082604051808280519060200190808383"
        "5b60208310151561030257805182526020820191506020810190506020830392506102dd565b60018360200361"
        "01000a038019825116818451168082178552505050505050905001915050908152602001604051809103902054"
        "9050919050565b806000836040518082805190602001908083835b602083101515610376578051825260208201"
        "9150602081019050602083039250610351565b6001836020036101000a03801982511681845116808217855250"
        "50505050509050019150509081526020016040518091039020819055505050565b806000846040518082805190"
        "602001908083835b6020831015156103ea57805182526020820191506020810190506020830392506103c5565b"
        "6001836020036101000a0380198251168184511680821785525050505050509050019150509081526020016040"
        "51809103902060008282540392505081905550806000836040518082805190602001908083835b602083101515"
        "610463578051825260208201915060208101905060208303925061043e565b6001836020036101000a03801982"
        "511681845116808217855250505050505090500191505090815260200160405180910390206000828254019250"
        "5081905550505050565b806000846040518082805190602001908083835b6020831015156104e1578051825260"
        "20820191506020810190506020830392506104bc565b6001836020036101000a03801982511681845116808217"
        "855250505050505090500191505090815260200160405180910390206000828254039250508190555080600083"
        "6040518082805190602001908083835b60208310151561055a5780518252602082019150602081019050602083"
        "039250610535565b6001836020036101000a038019825116818451168082178552505050505050905001915050"
        "908152602001604051809103902060008282540192505081905550606481111515156105aa57600080fd5b5050"
        "505600a165627a7a723058205669c1a68cebcef35822edcec77a15792da5c32a8aa127803290253b3d5f627200"
        "29";

    bytes input;
    boost::algorithm::unhex(bin, std::back_inserter(input));
    auto tx = fakeTransaction(cryptoSuite, keyPair, "", input, 101, 100001, "1", "1");
    auto sender = boost::algorithm::hex_lower(std::string(tx->sender()));

    auto hash = tx->hash();
    txpool->hash2Transaction.emplace(hash, tx);

    auto params = std::make_unique<NativeExecutionMessage>();
    params->setContextID(99);
    params->setSeq(1000);
    params->setDepth(0);

    params->setOrigin(std::string(sender));
    params->setFrom(std::string(sender));

    // The contract address
    h256 addressCreate("ff6f30856ad3bae00b1169808488502786a13e3c174d85682135ffd51310310e");
    std::string addressString = addressCreate.hex().substr(0, 40);
    // toChecksumAddress(addressString, hashImpl);
    params->setTo(std::move(addressString));

    params->setStaticCall(false);
    params->setGasAvailable(gas);
    params->setData(input);
    params->setType(NativeExecutionMessage::TXHASH);
    params->setTransactionHash(hash);
    params->setCreate(true);

    NativeExecutionMessage paramsBak = *params;

    auto blockHeader = std::make_shared<bcos::protocol::PBBlockHeader>(cryptoSuite);
    blockHeader->setNumber(1);

    std::promise<void> nextPromise;
    executor->nextBlockHeader(blockHeader, [&](bcos::Error::Ptr&& error) {
        BOOST_CHECK(!error);
        nextPromise.set_value();
    });
    nextPromise.get_future().get();

    // --------------------------------
    // Create contract ParallelOk
    // --------------------------------
    std::promise<bcos::protocol::ExecutionMessage::UniquePtr> executePromise;
    executor->executeTransaction(std::move(params),
        [&](bcos::Error::UniquePtr&& error, bcos::protocol::ExecutionMessage::UniquePtr&& result) {
            BOOST_CHECK(!error);
            executePromise.set_value(std::move(result));
        });

    auto result = executePromise.get_future().get();

    auto address = result->newEVMContractAddress();

    // Set user
    for (size_t i = 0; i < count; ++i)
    {
        params = std::make_unique<NativeExecutionMessage>();
        params->setContextID(i);
        params->setSeq(5000);
        params->setDepth(0);
        params->setFrom(std::string(sender));
        params->setTo(std::string(address));
        params->setOrigin(std::string(sender));
        params->setStaticCall(false);
        params->setGasAvailable(gas);
        params->setCreate(false);

        std::string user = "user" + boost::lexical_cast<std::string>(i);
        bcos::u256 value(1000000);
        params->setData(codec->encodeWithSig("set(string,uint256)", user, value));
        params->setType(NativeExecutionMessage::MESSAGE);

        std::promise<ExecutionMessage::UniquePtr> executePromise2;
        executor->executeTransaction(std::move(params),
            [&](bcos::Error::UniquePtr&& error, NativeExecutionMessage::UniquePtr&& result) {
                if (error)
                {
                    std::cout << "Error!" << boost::diagnostic_information(*error);
                }
                executePromise2.set_value(std::move(result));
            });
        auto result2 = executePromise2.get_future().get();
        // BOOST_CHECK_EQUAL(result->status(), 0);
    }

    std::vector<ExecutionMessage::UniquePtr> requests;
    requests.reserve(count);
    // Transfer
    for (size_t i = 0; i < count; ++i)
    {
        std::string from = "user" + boost::lexical_cast<std::string>(i);
        std::string to = "user" + boost::lexical_cast<std::string>(count - 1);
        bcos::u256 value(10);

        auto input = codec->encodeWithSig("transfer(string,string,uint256)", from, to, value);
        auto tx = fakeTransaction(cryptoSuite, keyPair, address, input, 101 + i, 100001, "1", "1");
        auto sender = boost::algorithm::hex_lower(std::string(tx->sender()));

        auto hash = tx->hash();
        txpool->hash2Transaction.emplace(hash, tx);

        params = std::make_unique<NativeExecutionMessage>();
        params->setContextID(i);
        params->setSeq(6000);
        params->setDepth(0);
        params->setFrom(std::string(sender));
        params->setTo(std::string(address));
        params->setOrigin(std::string(sender));
        params->setStaticCall(false);
        params->setGasAvailable(gas);
        params->setCreate(false);
        params->setType(NativeExecutionMessage::TXHASH);
        params->setTransactionHash(hash);

        requests.emplace_back(std::move(params));
    }

    std::promise<std::optional<Table>> tablePromise;
    backend->asyncCreateTable("cp_ff6f30856ad3bae00b1169808488502786a13e3c", PARA_VALUE_NAMES,
        [&](Error::UniquePtr&& error, std::optional<Table>&& table) {
            BOOST_CHECK(!error);
            BOOST_CHECK(table);
            tablePromise.set_value(std::move(*table));
        });
    auto table = tablePromise.get_future().get();

    Entry entry = table->newEntry();
    entry.setField(PARA_FUNC_NAME, "transfer(string,string,uint256)");
    entry.setField(PARA_CRITICAL_SIZE, boost::lexical_cast<std::string>(2));
    auto selector = getFuncSelector("transfer(string,string,uint256)", hashImpl);
    table->setRow(to_string(selector), entry);

    executor->dagExecuteTransactions(
        requests, [&](bcos::Error::UniquePtr error,
                      std::vector<bcos::protocol::ExecutionMessage::UniquePtr> results) {
            BOOST_CHECK(!error);

            for (size_t i = 0; i < results.size(); ++i)
            {
                auto& result = results[i];
                BOOST_CHECK_EQUAL(result->status(), 0);
                BOOST_CHECK(result->message().empty());
            }

            // Check result
            for (size_t i = 0; i < count; ++i)
            {
                params = std::make_unique<NativeExecutionMessage>();
                params->setContextID(i);
                params->setSeq(7000);
                params->setDepth(0);
                params->setFrom(std::string(sender));
                params->setTo(std::string(address));
                params->setOrigin(std::string(sender));
                params->setStaticCall(false);
                params->setGasAvailable(gas);
                params->setCreate(false);

                std::string account = "user" + boost::lexical_cast<std::string>(i);
                params->setData(codec->encodeWithSig("balanceOf(string)", account));
                params->setType(NativeExecutionMessage::MESSAGE);

                std::optional<ExecutionMessage::UniquePtr> output;
                executor->executeTransaction(
                    std::move(params), [&output](bcos::Error::UniquePtr&& error,
                                           NativeExecutionMessage::UniquePtr&& result) {
                        if (error)
                        {
                            std::cout << "Error!" << boost::diagnostic_information(*error);
                        }
                        // BOOST_CHECK(!error);
                        output = std::move(result);
                    });
                auto& balanceResult = *output;

                bcos::u256 value(0);
                codec->decode(balanceResult->data(), value);

                if (i < count - 1)
                {
                    BOOST_CHECK_EQUAL(value, u256(1000000 - 10));
                }
                else
                {
                    BOOST_CHECK_EQUAL(value, u256(1000000 + 10 * (count - 1)));
                }
            }
        });
}
BOOST_AUTO_TEST_SUITE_END()
}  // namespace test
}  // namespace bcos