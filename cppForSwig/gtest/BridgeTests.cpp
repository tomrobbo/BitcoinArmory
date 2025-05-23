////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2025, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include <cstdlib>

#include "TestUtils.h"
#include "../Wallets/WalletFileInterface.h"
#include "../Wallets/Seeds/Seeds.h"
#include "../BridgeAPI/CppBridge.h"
#include "../BridgeAPI/BridgeSocket.h"
#include "../BridgeAPI/ProtoCommandParser.h"

#include <capnp/message.h>
#include <capnp/serialize.h>
#include "capnp/Bridge.capnp.h"

using namespace Armory::Config;
using namespace Armory::Wallets;
using namespace Armory::Codec;

using namespace std::chrono_literals;
using namespace std::string_view_literals;

////////////////////////////////////////////////////////////////////////////////
namespace {
   BinaryData serializeCapnp(capnp::MallocMessageBuilder& msg)
   {
      auto flat = capnp::messageToFlatArray(msg);
      auto bytes = flat.asBytes();
      return BinaryData(bytes.begin(), bytes.end());
   }

   struct WltListEntry
   {
      std::string walletId;
      int loadState;
      bool staged;
   };

   struct AddressData
   {
      const int32_t index;
      const BinaryData hash;

      bool operator<(const AddressData& rhs) const
      {
         return hash < rhs.hash;
      }
   };

   struct WalletData
   {
      const std::string walletId;
      const std::string accountId;
      const std::string masterId;

      const std::string label;
      const std::string desc;

      const bool encrypted;
      const bool watchingOnly;
      const std::set<AddressData> addresses;
      const int64_t lookup;

      const std::filesystem::path path;
      const uint32_t kdfMemReq;
   };

   AddressData capnToAddressData(const Bridge::WalletData::AddressData::Reader& capnAddr)
   {
      auto capnHash = capnAddr.getPrefixedHash();
      BinaryData addrHash{capnHash.begin(), capnHash.end()};
      return AddressData{capnAddr.getIndex(), std::move(addrHash)};
   }

   WalletData capnToWalletData(const Bridge::WalletData::Reader& capnWlt)
   {
      auto capnAddrs = capnWlt.getAddressData();
      std::set<AddressData> addresses;
      for (const auto& capnAddr : capnAddrs) {
         addresses.emplace(capnToAddressData(capnAddr));
      }

      std::string walletId = capnWlt.getWalletId();
      return WalletData{
         walletId, capnWlt.getAccountId(), capnWlt.getMasterId(),
         capnWlt.getLabel(), capnWlt.getDesc(),
         capnWlt.getUsesEncryption(), capnWlt.getWatchingOnly(),
         std::move(addresses), capnWlt.getLookupCount(),
         std::filesystem::path(capnWlt.getPath()), capnWlt.getKdfMemReq()
      };
   }
}

////////////////////////////////////////////////////////////////////////////////
class WalletManagerTests : public ::testing::Test
{
protected:
   virtual void SetUp(void)
   {
      homedir_ = std::filesystem::path("./fakehomedir");
      FileUtils::removeDirectory(homedir_);
      std::filesystem::create_directory(homedir_);

      Armory::Config::parseArgs({
         "--offline",
         "--datadir=./fakehomedir" },
         Armory::Config::ProcessType::DB);
   }

   virtual void TearDown(void)
   {
      Armory::Config::reset();
      FileUtils::removeDirectory(homedir_);
   }

   std::filesystem::path homedir_;
};

////////////////////////////////////////////////////////////////////////////////
TEST_F(WalletManagerTests, ListStageLoad)
{
   /*
   covers the following scenarios:
      1: wallet without control passphrase
      2: with control passphrase, keep encrypted
      3: with control passphrase, decrypted
      4: with control passphrase, decrypted but unstaged
   */
   std::vector<std::filesystem::path> walletFiles;

   //wallet 1
   {
      IO::CreateWalletParams params{
         homedir_,
         {1ms, 0, SecureBinaryData::fromString("privpass1")},
         {1ms, 0, {}},
         nullptr, 4
      };

      std::unique_ptr<Armory::Seeds::ClearTextSeed> seed(
         new Armory::Seeds::ClearTextSeed_Armory135());
      auto assetWlt = AssetWallet_Single::createFromSeed(
         std::move(seed), params);
      walletFiles.emplace_back(assetWlt->getDbFilename());
   }

   //wallet 2, 3, 4
   for (unsigned i=2; i<5; i++) {
      IO::CreateWalletParams params{
         homedir_,
         {1ms, 0, SecureBinaryData::fromString("privpass" + std::to_string(i))},
         {1ms, 0, SecureBinaryData::fromString("controlpass" + std::to_string(i))},
         nullptr, 4
      };

      std::unique_ptr<Armory::Seeds::ClearTextSeed> seed(
         new Armory::Seeds::ClearTextSeed_Armory135());
      auto assetWlt = AssetWallet_Single::createFromSeed(
         std::move(seed), params);
      walletFiles.emplace_back(assetWlt->getDbFilename());
   }

   //list wallets
   Armory::Bridge::WalletManager mgr{homedir_};
   auto theList = mgr.listWallets();
   ASSERT_EQ(theList.size(), 4);
   for (const auto& path : walletFiles) {
      ASSERT_NE(theList.find(path.stem().string()), theList.end());
   }

   auto checkState = [&walletFiles](
      const std::map<std::string, std::shared_ptr<Armory::Bridge::WalletFileInfo>>& theList,
      unsigned intId, Armory::Bridge::WalletLoadState expLoadState, bool expectedStaged)->bool
   {
      auto listEntry = theList.at(walletFiles[intId].stem().string());
      if (listEntry->state() != expLoadState) {
         return false;
      }
      if (listEntry->state() == Armory::Bridge::WalletLoadState::Ready &&
         listEntry->walletId().empty()) {
         return false;
      }
      return listEntry->staged() == expectedStaged;
   };

   EXPECT_TRUE(checkState(theList, 0, Armory::Bridge::WalletLoadState::Ready, true));
   EXPECT_TRUE(checkState(theList, 1, Armory::Bridge::WalletLoadState::Encrypted, false));
   EXPECT_TRUE(checkState(theList, 2, Armory::Bridge::WalletLoadState::Encrypted, false));
   EXPECT_TRUE(checkState(theList, 3, Armory::Bridge::WalletLoadState::Encrypted, false));

   //unlock wallets 3 & 4, fail for wtl2
   {
      try {
         mgr.unlockControlHeader(walletFiles[2].stem().string(), [](
            const std::set<EncryptionKeyId>&)->SecureBinaryData {
               return SecureBinaryData::fromString("controlpass3");
            }
         );

         mgr.unlockControlHeader(walletFiles[3].stem().string(), [](
            const std::set<EncryptionKeyId>&)->SecureBinaryData {
               return SecureBinaryData::fromString("controlpass4");
            }
         );
      } catch (const std::exception& e) {
         ASSERT_TRUE(false) << e.what();
      }

      unsigned count = 0;
      auto failUnlockLbd = [&count](
         const std::set<EncryptionKeyId>&)->SecureBinaryData {
         if (count++ < 2) {
            return BtcUtils::fortuna_.generateRandom(10);
         }
         //give up after 2 tries
         return {};
      };
      try {
         mgr.unlockControlHeader(walletFiles[1].stem().string(), failUnlockLbd);
         ASSERT_TRUE(false);
      } catch (const Encryption::DecryptedDataContainerException& e) {
         ASSERT_EQ(e.what(), std::string{"empty passphrase"}) << e.what();
      }
   }

   //recheck the list
   theList = mgr.listWallets();
   ASSERT_EQ(theList.size(), 4);
   for (const auto& path : walletFiles) {
      ASSERT_NE(theList.find(path.stem().string()), theList.end());
   }
   EXPECT_TRUE(checkState(theList, 0, Armory::Bridge::WalletLoadState::Ready, true));
   EXPECT_TRUE(checkState(theList, 1, Armory::Bridge::WalletLoadState::Encrypted, false));
   EXPECT_TRUE(checkState(theList, 2, Armory::Bridge::WalletLoadState::Ready, true));
   EXPECT_TRUE(checkState(theList, 3, Armory::Bridge::WalletLoadState::Ready, true));

   //unstage wlt4
   {
      auto wlt4Info = theList.at(walletFiles[3].stem().string());
      ASSERT_TRUE(mgr.stageWallet(wlt4Info->walletId(), false));
   }

   //recheck the list
   theList = mgr.listWallets();
   ASSERT_EQ(theList.size(), 4);
   for (const auto& path : walletFiles) {
      ASSERT_NE(theList.find(path.stem().string()), theList.end());
   }
   EXPECT_TRUE(checkState(theList, 0, Armory::Bridge::WalletLoadState::Ready, true));
   EXPECT_TRUE(checkState(theList, 1, Armory::Bridge::WalletLoadState::Encrypted, false));
   EXPECT_TRUE(checkState(theList, 2, Armory::Bridge::WalletLoadState::Ready, true));
   EXPECT_TRUE(checkState(theList, 3, Armory::Bridge::WalletLoadState::Ready, false));

   //load wallets
   mgr.loadWallets();

   //check loaded wallets
   auto checkHasWallet = [&theList, &walletFiles, &mgr](unsigned intId)->bool
   {
      auto entry = theList.at(walletFiles[intId].stem().string());
      try {
         return mgr.hasWallet(entry->walletId());
      } catch (const std::exception&) {
         return false;
      }
      
   };

   EXPECT_TRUE(checkHasWallet(0));
   EXPECT_FALSE(checkHasWallet(1));
   EXPECT_TRUE(checkHasWallet(2));
   EXPECT_FALSE(checkHasWallet(3));

   //check list only has wlt1
   theList = mgr.listWallets();
   ASSERT_EQ(theList.size(), 2);

   ASSERT_EQ(theList.find(walletFiles[0].stem().string()), theList.end());
   ASSERT_NE(theList.find(walletFiles[1].stem().string()), theList.end());
   ASSERT_EQ(theList.find(walletFiles[2].stem().string()), theList.end());
   ASSERT_NE(theList.find(walletFiles[3].stem().string()), theList.end());
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(WalletManagerTests, Migrate)
{
   //copy test legacy wallet to datadir
   const auto& wltId = "28m472Xbm"sv;
   std::string fileName = std::string{"armory_"sv} +
      std::string{wltId} +
      std::string{"_.wallet"sv};
   const std::filesystem::path wltPath{fileName};
   std::filesystem::copy(
      "input_files/legacy.wallet"sv,
      homedir_ / wltPath
   );

   //list wallets
   Armory::Bridge::WalletManager mgr{homedir_};
   auto theList = mgr.listWallets();
   ASSERT_EQ(theList.size(), 1);

   //sanity check
   {
      auto iter = theList.find(wltPath.stem().string());
      ASSERT_NE(iter, theList.end());
      EXPECT_EQ(iter->second->state(), Armory::Bridge::WalletLoadState::Legacy);
      EXPECT_EQ(iter->second->walletId(), wltId);
      EXPECT_FALSE(iter->second->staged());
   }

   //migrate the wallet
   auto passFunc = [](const std::set<EncryptionKeyId>&)->SecureBinaryData
   {
      return SecureBinaryData::fromString("testnet");
   };
   mgr.migrateWallet(wltPath.stem().string(), passFunc,
      IO::CreateWalletParams{
         homedir_,
         {1ms, 0, {}},
         {1ms, 0, {}},
         nullptr, 0
      });

   theList = mgr.listWallets();
   ASSERT_EQ(theList.size(), 2);

   //check legacy wallet
   {
      auto iter = theList.find(wltPath.stem().string());
      ASSERT_NE(iter, theList.end());
      EXPECT_EQ(iter->second->state(), Armory::Bridge::WalletLoadState::Legacy);
      EXPECT_EQ(iter->second->walletId(), wltId);
      EXPECT_FALSE(iter->second->staged());
   }

   //check migrated wallet
   {
      auto iter = theList.begin();
      while (iter != theList.end()) {
         if (iter->first != wltPath.stem().string()) {
            break;
         }
         ++iter;
      }
      ASSERT_NE(iter, theList.end());
      EXPECT_EQ(iter->second->state(), Armory::Bridge::WalletLoadState::Ready);
      EXPECT_EQ(iter->second->walletId(), wltId);
      EXPECT_TRUE(iter->second->staged());
   }

   //load wallets
   mgr.loadWallets();

   //check migrated wallet
   try {
      auto wltContainer = mgr.getWalletContainer(std::string{wltId});
      ASSERT_NE(wltContainer, nullptr);
      EXPECT_EQ(wltContainer->getHighestUsedIndex(), 2);

      auto wltPtr = wltContainer->getWalletPtr();
      EXPECT_EQ(wltPtr->getLabel(), "legacy1");
      EXPECT_EQ(wltPtr->getDescription(), "migration test");

      //TODO: check addresses
   } catch (const std::exception& e) {
      ASSERT_TRUE(false) << e.what();
   }
}

////////////////////////////////////////////////////////////////////////////////
using MsgPtr = std::unique_ptr<Armory::Bridge::WritePayload_Bridge>;

class BridgeTests : public ::testing::Test
{
protected:
   //setup
   virtual void SetUp(void)
   {
      std::srand(std::time({}));
      homedir = std::filesystem::path("./fakehomedir");
      FileUtils::removeDirectory(homedir);
      std::filesystem::create_directory(homedir);

      Armory::Config::parseArgs({
         "--offline",
         "--datadir=./fakehomedir" },
         Armory::Config::ProcessType::DB);

      bridge_ = std::make_shared<Armory::Bridge::CppBridge>();
      bridge_->setWriteLambda([this](MsgPtr payload) {
         std::unique_lock<std::mutex> lock(commsMutex_);
         replyQueue_.emplace_back(std::move(payload));
         commsCV_.notify_all();
      });
   }

   virtual void TearDown(void)
   {
      bridge_.reset();
      Armory::Config::reset();
      FileUtils::removeDirectory(homedir);
   }

   //message queue
   MsgPtr waitOnReply()
   {
      std::unique_lock<std::mutex> lock(commsMutex_);
      while (replyQueue_.empty()) {
         commsCV_.wait(lock);
      }
      auto result = std::move(replyQueue_.front());
      replyQueue_.pop_front();
      return result;
   }

   void pushRequest(const BinaryData& rawRequest)
   {
      Armory::Bridge::ProtoCommandParser::processData(bridge_, rawRequest);
   }

   //helpers
   std::map<std::string, WltListEntry> listWallets()
   {
      auto refId = rand();

      capnp::MallocMessageBuilder message;
      auto toBridge = message.initRoot<Bridge::ToBridge>();
      toBridge.setReferenceId(refId);
      auto request = toBridge.initWalletManager();
      request.setListWallets();

      auto rawReq = serializeCapnp(message);
      pushRequest(rawReq);

      auto result = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      auto reply = fromBridge.getReply();
      if (!reply.getSuccess() || reply.getReferenceId() != refId) {
         throw std::runtime_error({});
      }

      auto replyMgr = reply.getWalletManager();
      auto replyListWlts = replyMgr.getListWallets();

      std::map<std::string, WltListEntry> wltMap;
      for (auto capnEntry : replyListWlts) {
         wltMap.emplace(std::string(capnEntry.getPath()), WltListEntry{
            capnEntry.getWalletId(),
            (int)capnEntry.getState(),
            capnEntry.getStaged()
         });
      }
      return wltMap;
   }

   bool unlockWallet(const std::string& path, const std::string& passphrase)
   {
      auto refId = rand();
      auto callbackId = BtcUtils::fortuna_.generateRandom(10).toHexStr();

      //request unlock
      {
         capnp::MallocMessageBuilder message;
         auto toBridge = message.initRoot<Bridge::ToBridge>();
         toBridge.setReferenceId(refId);
         auto request = toBridge.initWalletManager();
         auto unlockReq = request.initUnlockControlHeader();

         unlockReq.setCallbackId(callbackId);
         unlockReq.setWalletPath(path);

         auto rawReq = serializeCapnp(message);
         pushRequest(rawReq);
      }

      //wait on passphrase prompt
      auto rawPrompt = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(rawPrompt->data.getPtr()),
         rawPrompt->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      if (fromBridge.which() != Bridge::FromBridge::NOTIFICATION) {
         return false;
      }

      auto notif = fromBridge.getNotification();
      if (notif.getCallbackId() != callbackId) {
         return false;
      }
      if (notif.which() != Bridge::Notification::UNLOCK_REQUEST) {
         return false;
      }
      auto counter = notif.getCounter();

      //push passphrase
      {
         capnp::MallocMessageBuilder message;
         auto toBridge = message.initRoot<Bridge::ToBridge>();
         auto notifReply = toBridge.initNotification();
         notifReply.setSuccess(true);
         notifReply.setCounter(counter);
         auto capnWalletCreation = notifReply.initWalletCreation();
         capnWalletCreation.setPassphrase(passphrase);
         capnWalletCreation.setKdfTargetMs(1);
         capnWalletCreation.setKdfTargetMB(0);

         auto rawReq = serializeCapnp(message);
         pushRequest(rawReq);
      }

      //expect success notif
      bool success = false;
      {
         auto rawReply = waitOnReply();
         kj::ArrayPtr<const capnp::word> words(
            reinterpret_cast<const capnp::word*>(rawReply->data.getPtr()),
            rawReply->data.getSize() / sizeof(capnp::word));
         capnp::FlatArrayMessageReader reader(words);
         auto fromBridge = reader.getRoot<Bridge::FromBridge>();
         if (fromBridge.which() != Bridge::FromBridge::REPLY) {
            return false;
         }

         auto rpcReply = fromBridge.getReply();
         if (rpcReply.getReferenceId() != refId) {
            success = false;
         } else {
            success = rpcReply.getSuccess();
         }
      }

      //expect prompt cleanup
      {
         auto rawReply = waitOnReply();
         kj::ArrayPtr<const capnp::word> words(
            reinterpret_cast<const capnp::word*>(rawReply->data.getPtr()),
            rawReply->data.getSize() / sizeof(capnp::word));
         capnp::FlatArrayMessageReader reader(words);
         auto fromBridge = reader.getRoot<Bridge::FromBridge>();
         if (fromBridge.which() != Bridge::FromBridge::NOTIFICATION) {
            return false;
         }
   
         auto notif = fromBridge.getNotification();
         if (notif.getCallbackId() != callbackId) {
            return false;
         }
         if (notif.which() != Bridge::Notification::CLEANUP) {
            return false;
         }
      }

      return success;
   }

   int failToUnlockWallet(const std::string& path, unsigned count)
   {
      auto refId = rand();
      auto callbackId = BtcUtils::fortuna_.generateRandom(10).toHexStr();

      //request unlock
      {
         capnp::MallocMessageBuilder message;
         auto toBridge = message.initRoot<Bridge::ToBridge>();
         toBridge.setReferenceId(refId);
         auto request = toBridge.initWalletManager();
         auto unlockReq = request.initUnlockControlHeader();

         unlockReq.setCallbackId(callbackId);
         unlockReq.setWalletPath(path);

         auto rawReq = serializeCapnp(message);
         pushRequest(rawReq);
      }

      unsigned attempts = 0;
      while (attempts <= count) {
         //wait on passphrase prompt
         auto rawPrompt = waitOnReply();
         kj::ArrayPtr<const capnp::word> words(
            reinterpret_cast<const capnp::word*>(rawPrompt->data.getPtr()),
            rawPrompt->data.getSize() / sizeof(capnp::word));
         capnp::FlatArrayMessageReader reader(words);
         auto fromBridge = reader.getRoot<Bridge::FromBridge>();
         if (fromBridge.which() != Bridge::FromBridge::NOTIFICATION) {
            return -1;
         }

         auto notif = fromBridge.getNotification();
         if (notif.getCallbackId() != callbackId) {
            return -2;
         }
         if (notif.which() != Bridge::Notification::UNLOCK_REQUEST) {
            return -3;
         }
         auto counter = notif.getCounter();

         //push passphrase
         {
            capnp::MallocMessageBuilder message;
            auto toBridge = message.initRoot<Bridge::ToBridge>();
            auto notifReply = toBridge.initNotification();
            notifReply.setCounter(counter);

            if (attempts != count) {
               notifReply.setSuccess(true);
               auto badPass = BtcUtils::fortuna_.generateRandom(10).toHexStr();
               notifReply.setUnlockRequest(badPass);
            } else {
               notifReply.setSuccess(false);
            }

            auto rawReq = serializeCapnp(message);
            pushRequest(rawReq);
         }
         ++attempts;
      }

      //expect failure notif
      {
         auto rawReply = waitOnReply();
         kj::ArrayPtr<const capnp::word> words(
            reinterpret_cast<const capnp::word*>(rawReply->data.getPtr()),
            rawReply->data.getSize() / sizeof(capnp::word));
         capnp::FlatArrayMessageReader reader(words);
         auto fromBridge = reader.getRoot<Bridge::FromBridge>();
         if (fromBridge.which() != Bridge::FromBridge::REPLY) {
            return -4;
         }

         auto rpcReply = fromBridge.getReply();
         if (rpcReply.getReferenceId() != refId) {
            return -5;
         }
         if (rpcReply.getSuccess() != false) {
            return -6;
         }

         std::string errorStr = rpcReply.getError();
         if (errorStr != "empty passphrase") {
            return -7;
         }
      }

      //expect prompt cleanup
      {
         auto rawReply = waitOnReply();
         kj::ArrayPtr<const capnp::word> words(
            reinterpret_cast<const capnp::word*>(rawReply->data.getPtr()),
            rawReply->data.getSize() / sizeof(capnp::word));
         capnp::FlatArrayMessageReader reader(words);
         auto fromBridge = reader.getRoot<Bridge::FromBridge>();
         if (fromBridge.which() != Bridge::FromBridge::NOTIFICATION) {
            return -8;
         }
   
         auto notif = fromBridge.getNotification();
         if (notif.getCallbackId() != callbackId) {
            return -9;
         }
         if (notif.which() != Bridge::Notification::CLEANUP) {
            return -10;
         }
      }
      return attempts;
   }

   bool stageWallet(const std::string& walletId, bool stage)
   {
      auto refId = rand();

      //request staging change
      {
         capnp::MallocMessageBuilder message;
         auto toBridge = message.initRoot<Bridge::ToBridge>();
         toBridge.setReferenceId(refId);
         auto request = toBridge.initWalletManager();
         auto stageReq = request.initStageWallet();

         stageReq.setWalletId(walletId);
         stageReq.setStage(stage);

         auto rawReq = serializeCapnp(message);
         pushRequest(rawReq);
      }

      //expect success
      auto result = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      auto reply = fromBridge.getReply();
      if (reply.getReferenceId() != refId) {
         return false;
      }
      return reply.getSuccess();
   }

   std::map<std::string, WalletData> loadWallets()
   {
      auto refId = rand();

      capnp::MallocMessageBuilder message;
      auto toBridge = message.initRoot<Bridge::ToBridge>();
      toBridge.setReferenceId(refId);
      auto request = toBridge.initWalletManager();
      request.setLoadWallets();

      auto rawReq = serializeCapnp(message);
      pushRequest(rawReq);

      auto result = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      auto reply = fromBridge.getReply();
      if (!reply.getSuccess() || reply.getReferenceId() != refId) {
         throw std::runtime_error({});
      }

      auto replyMgr = reply.getWalletManager();
      auto replyLoadWlts = replyMgr.getLoadWallets();

      std::map<std::string, WalletData> wltsData;
      for (const auto& capnWlt : replyLoadWlts) {
         wltsData.emplace(capnWlt.getWalletId(), capnToWalletData(capnWlt));
      }
      return wltsData;
   }

   WalletData getWalletData(const std::string& walletId)
   {
      auto refId = rand();

      capnp::MallocMessageBuilder message;
      auto toBridge = message.initRoot<Bridge::ToBridge>();
      toBridge.setReferenceId(refId);
      auto request = toBridge.initWallet();
      request.setWalletId(walletId);
      request.setGetData();

      auto rawReq = serializeCapnp(message);
      pushRequest(rawReq);

      auto result = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      auto reply = fromBridge.getReply();
      if (!reply.getSuccess() || reply.getReferenceId() != refId) {
         throw std::runtime_error({});
      }

      auto replyMgr = reply.getWallet();
      return capnToWalletData(replyMgr.getGetData());
   }

   std::chrono::milliseconds testKDFUnlock(const std::string& walletId)
   {
      auto refId = rand();
      capnp::MallocMessageBuilder message;
      auto toBridge = message.initRoot<Bridge::ToBridge>();
      toBridge.setReferenceId(refId);
      auto request = toBridge.initWallet();
      request.setWalletId(walletId);
      request.setGetUnlockTime();

      auto rawReq = serializeCapnp(message);
      pushRequest(rawReq);

      auto result = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      auto reply = fromBridge.getReply();
      if (!reply.getSuccess() || reply.getReferenceId() != refId) {
         return {};
      }
      return std::chrono::milliseconds(reply.getWallet().getGetUnlockTime());
   }

   WalletData progressWalletCreation(const std::string& callbackId,
      const std::string& passphrase, std::chrono::milliseconds targetMs,
      uint32_t targetMB, int lookup)
   {
      std::string masterId;
      std::filesystem::path path;
      int notifCount=0;
      bool run = true;
      while (run) {
         auto result = waitOnReply();
         kj::ArrayPtr<const capnp::word> words(
            reinterpret_cast<const capnp::word*>(result->data.getPtr()),
            result->data.getSize() / sizeof(capnp::word));
         capnp::FlatArrayMessageReader reader(words);
         auto fromBridge = reader.getRoot<Bridge::FromBridge>();
         if (fromBridge.which() != Bridge::FromBridge::NOTIFICATION) {
            throw std::runtime_error("invalid FromBridge which");
         }

         auto notif = fromBridge.getNotification();
         if (notif.getCallbackId() != callbackId) {
            throw std::runtime_error("invalid callbackId");
         }
         auto counter = notif.getCounter();

         switch (notif.which())
         {
            case Bridge::Notification::WALLET_CREATION:
            {
               auto wltNotif = notif.getWalletCreation();
               switch (wltNotif.which())
               {
                  case Bridge::Notification::WalletCreation::SET_CTRL_PASS:
                  {
                     capnp::MallocMessageBuilder message;
                     auto toBridge = message.initRoot<Bridge::ToBridge>();
                     auto notifReply = toBridge.initNotification();
                     notifReply.setSuccess(true);
                     notifReply.setCounter(counter);
                     auto capnWalletCreation = notifReply.initWalletCreation();
                     capnWalletCreation.setKdfTargetMs(250);
                     capnWalletCreation.setKdfTargetMB(0);

                     auto rawReq = serializeCapnp(message);
                     pushRequest(rawReq);
                     break;
                  }

                  case Bridge::Notification::WalletCreation::SET_PRIV_PASS:
                  {
                     capnp::MallocMessageBuilder message;
                     auto toBridge = message.initRoot<Bridge::ToBridge>();
                     auto notifReply = toBridge.initNotification();
                     notifReply.setSuccess(true);
                     notifReply.setCounter(counter);
                     auto capnWalletCreation = notifReply.initWalletCreation();
                     capnWalletCreation.setPassphrase(passphrase);
                     capnWalletCreation.setKdfTargetMs(targetMs.count());
                     capnWalletCreation.setKdfTargetMB(targetMB);

                     auto rawReq = serializeCapnp(message);
                     pushRequest(rawReq);
                     break;
                  }

                  default:
                     throw std::runtime_error("unexpected wallet creation notif");
               }
               break;
            }

            case Bridge::Notification::WALLET_PROGRESS:
            {
               auto wltNotif = notif.getWalletProgress();
               switch (wltNotif.which())
               {
                  case Bridge::Notification::WalletProgress::CREATE_FILE:
                  {
                     if (notifCount++ != 0) {
                        throw std::runtime_error("count != 0");
                     }
                     path = std::filesystem::path(wltNotif.getCreateFile());
                     break;
                  }

                  case Bridge::Notification::WalletProgress::INIT_FILE:
                  {
                     if (notifCount++ != 1) {
                        throw std::runtime_error("count != 1");
                     }
                     masterId = wltNotif.getInitFile();
                     break;
                  }

                  case Bridge::Notification::WalletProgress::READ_FILE:
                  {
                     if (notifCount++ != 2) {
                        throw std::runtime_error("count != 2");
                     }
                     if (wltNotif.getReadFile() != masterId) {
                        throw std::runtime_error("masterId mismatch");
                     }
                     break;
                  }

                  case Bridge::Notification::WalletProgress::CREATE_ACCOUNT:
                  {
                     if (notifCount++ != 3) {
                        throw std::runtime_error("count != 3");
                     }
                     EXPECT_EQ(wltNotif.getCreateAccount(), "Armory Legacy");
                     break;
                  }

                  case Bridge::Notification::WalletProgress::EXTEND_CHAIN:
                  {
                     if (notifCount++ != 4) {
                        throw std::runtime_error("count != 4");
                     }
                     auto extendNotif = wltNotif.getExtendChain();
                     EXPECT_EQ(extendNotif.getTotal(), lookup);
                     EXPECT_EQ(extendNotif.getCurrent(), 0);
                     break;
                  }

                  default:
                     throw std::runtime_error("unexpected wallet progress notif");
               }
               break;
            }

            case Bridge::Notification::CLEANUP:
            {
               run = false;
               break;
            }

            default:
               throw std::runtime_error("unexpected wallet notif");
         }
      }

      if (notifCount != 5) {
         throw std::runtime_error("unexpected notif count");
      }

      return WalletData{
         {}, {}, masterId,
         {}, {},
         true, false, {}, 0,
         path
      };
   }

   std::vector<std::string> getBackup(const std::string& walletId,
      const std::string& passphrase, Bridge::WalletBackup::Type backupType)
   {
      auto refId = rand();
      capnp::MallocMessageBuilder message;
      auto toBridge = message.initRoot<Bridge::ToBridge>();
      toBridge.setReferenceId(refId);
      auto request = toBridge.initWallet();
      request.setWalletId(walletId);
      auto reqBackup = request.initCreateBackupString();
      reqBackup.setPassphrase(passphrase);
      auto rawReq = serializeCapnp(message);
      pushRequest(rawReq);

      auto result = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      auto reply = fromBridge.getReply();
      if (reply.getSuccess() == false) {
         throw std::runtime_error("failure");
      }
      if (reply.getReferenceId() != refId) {
         throw std::runtime_error("refId mismatch");
      }

      if (reply.which() != Bridge::RpcReply::WALLET) {
         throw std::runtime_error("which mismatch");
      }
      auto walletReply = reply.getWallet();

      if (walletReply.which() != Bridge::WalletReply::CREATE_BACKUP_STRING) {
         throw std::runtime_error("which mismatch");
      }
      auto capnBackup = walletReply.getCreateBackupString();

      //backup type
      if (capnBackup.getBackupType() != backupType) {
         throw std::runtime_error("backup type mismatch");
      }

      //root
      std::vector<std::string> lines;
      for (const auto& line : capnBackup.getRootClear()) {
         lines.emplace_back(line);
      }

      //chaincode
      if (capnBackup.hasChainClear()) {
         for (const auto& line : capnBackup.getChainClear()) {
            lines.emplace_back(line);
         }
      }
      return lines;
   }

   AddressData getAddress(
      const std::string& walletId,
      const std::string& accountId)
   {
      auto refId = rand();
      capnp::MallocMessageBuilder message;
      auto toBridge = message.initRoot<Bridge::ToBridge>();
      toBridge.setReferenceId(refId);
      auto request = toBridge.initWallet();
      request.setWalletId(walletId);
      request.setAccountId(accountId);
      auto reqAddr = request.initGetAddress();
      reqAddr.setNew();
      auto rawReq = serializeCapnp(message);
      pushRequest(rawReq);

      auto result = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      auto reply = fromBridge.getReply();
      if (reply.getSuccess() == false) {
         throw std::runtime_error("failure: " + std::string{reply.getError()});
      }
      if (reply.getReferenceId() != refId) {
         throw std::runtime_error("refId mismatch");
      }

      if (reply.which() != Bridge::RpcReply::WALLET) {
         throw std::runtime_error("which mismatch");
      }
      auto walletReply = reply.getWallet();

      if (walletReply.which() != Bridge::WalletReply::GET_ADDRESS) {
         throw std::runtime_error("which mismatch");
      }

      return capnToAddressData(walletReply.getGetAddress());
   }

   WalletData restoreWallet(const std::vector<std::string>& lines,
      const std::string& expectedWltId, Bridge::WalletBackup::Type backupType,
      const std::string& passphrase, std::chrono::milliseconds targetMs, uint32_t targetMB,
      bool merge, unsigned expectedLookup)
   {
      if (lines.size() < 2) {
         throw std::runtime_error("1");
      }

      //restore the wallet
      auto refId = rand();
      auto callbackId = BtcUtils::fortuna_.generateRandom(10).toHexStr();

      {
         capnp::MallocMessageBuilder message;
         auto toBridge = message.initRoot<Bridge::ToBridge>();
         toBridge.setReferenceId(refId);
         auto request = toBridge.initUtils();
         auto restoreWltReq = request.initRestoreWallet();

         restoreWltReq.setCallbackId(callbackId);
         restoreWltReq.setPrivKdfTargetMs(300);

         auto rootLines = restoreWltReq.initRoot(2);
         rootLines.set(0, lines[0]);
         rootLines.set(1, lines[1]);

         if (lines.size() == 4) {
            auto ccLines = restoreWltReq.initChaincode(2);
            ccLines.set(0, lines[2]);
            ccLines.set(1, lines[3]);
         }

         auto rawReq = serializeCapnp(message);
         pushRequest(rawReq);
      }

      //backup deser notifs
      unsigned notifCount = 0;
      bool restoringBackup = true;
      while (restoringBackup) {
         auto result = waitOnReply();
         kj::ArrayPtr<const capnp::word> words(
            reinterpret_cast<const capnp::word*>(result->data.getPtr()),
            result->data.getSize() / sizeof(capnp::word));
         capnp::FlatArrayMessageReader reader(words);
         auto fromBridge = reader.getRoot<Bridge::FromBridge>();
         if (fromBridge.which() != Bridge::FromBridge::NOTIFICATION) {
            throw std::runtime_error("2");
         }

         auto notif = fromBridge.getNotification();
         if (notif.getCallbackId() != callbackId) {
            throw std::runtime_error("3");
         }

         switch (notif.which())
         {
            case Bridge::Notification::RESTORE:
            {
               auto restoreNotif = notif.getRestore();
               switch (restoreNotif.which())
               {
                  case Bridge::Notification::RestorePrompt::CHECK_WALLET_ID:
                  {
                     //return merge decision
                     auto meta = restoreNotif.getCheckWalletId();
                     if (meta.getWalletId() != expectedWltId ||
                        meta.getBackupType() != backupType) {
                        throw std::runtime_error("4");
                     }

                     auto counter = notif.getCounter();
                     capnp::MallocMessageBuilder notifMsg;
                     auto toBridge = notifMsg.initRoot<Bridge::ToBridge>();
                     toBridge.setReferenceId(rand());

                     auto notifReply = toBridge.initNotification();
                     notifReply.setSuccess(true);
                     notifReply.setCounter(counter);
                     notifReply.setRestore(merge ?
                        Bridge::NotificationReply::RestoreMode::MERGE :
                        Bridge::NotificationReply::RestoreMode::OVERWRITE
                     );

                     auto rawReq = serializeCapnp(notifMsg);
                     pushRequest(rawReq);
                     restoringBackup = false;
                     break;
                  }

                  default:
                     throw std::runtime_error("5");
               }

               break;
            }

            default:
               throw std::runtime_error("6");
         }
      }

      //create wallet notifs
      auto wltData = progressWalletCreation(callbackId,
         passphrase, targetMs, targetMB,
         expectedLookup);

      //validate reply
      auto result = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      if (fromBridge.which() != Bridge::FromBridge::REPLY) {
         throw std::runtime_error("7");
      }

      auto reply = fromBridge.getReply();
      if (!reply.getSuccess() || reply.getReferenceId() != refId) {
         throw std::runtime_error("8");
      }
      return wltData;
   }

public:
   std::filesystem::path homedir;

private:
   std::shared_ptr<Armory::Bridge::CppBridge> bridge_;
   std::mutex commsMutex_;
   std::deque<MsgPtr> replyQueue_;
   std::condition_variable commsCV_;
};

////////////////////////////////////////////////////////////////////////////////
TEST_F(BridgeTests, ListStageLoad)
{
   /*
   This test covers the same scenario as WalletManagerTests.ListStageLoad,
   but through CppBridge rather than WalletManager directly
   */
   std::vector<std::pair<std::filesystem::path, std::string>> walletFiles;

   //wallet 1
   {
      IO::CreateWalletParams params{
         homedir,
         {1ms, 0, SecureBinaryData::fromString("privpass1")},
         {1ms, 0, {}},
         nullptr, 4
      };

      std::unique_ptr<Armory::Seeds::ClearTextSeed> seed(
         new Armory::Seeds::ClearTextSeed_Armory135());
      auto assetWlt = AssetWallet_Single::createFromSeed(
         std::move(seed), params);
      walletFiles.emplace_back(
         std::make_pair(assetWlt->getDbFilename(), assetWlt->getID()));
   }

   //wallet 2, 3, 4
   for (unsigned i=2; i<5; i++) {
      IO::CreateWalletParams params{
         homedir,
         {1ms, 0, SecureBinaryData::fromString("privpass" + std::to_string(i))},
         {1ms, 0, SecureBinaryData::fromString("controlpass" + std::to_string(i))},
         nullptr, 4
      };

      std::unique_ptr<Armory::Seeds::ClearTextSeed> seed(
         new Armory::Seeds::ClearTextSeed_Armory135());
      auto assetWlt = AssetWallet_Single::createFromSeed(
         std::move(seed), params);
      walletFiles.emplace_back(
         std::make_pair(assetWlt->getDbFilename(), assetWlt->getID()));
   }

   //list wallets
   try {
      auto wltList = listWallets();
      ASSERT_EQ(wltList.size(), 4);
      auto checkWltList = [&wltList, &walletFiles](
         unsigned intId, int expectedState, bool expectedStaged)->bool
      {
         auto entry = walletFiles[intId];
         auto path = entry.first.stem().string();
         auto capnEntry = wltList.at(path);

         if (capnEntry.loadState != expectedState) {
            return false;
         }

         if (expectedState == 4) {
            if (capnEntry.walletId != entry.second) {
               return false;
            }
         }
         return capnEntry.staged == expectedStaged;
      };
      EXPECT_TRUE(checkWltList(0, 4, true));
      EXPECT_TRUE(checkWltList(1, 3, false));
      EXPECT_TRUE(checkWltList(2, 3, false));
      EXPECT_TRUE(checkWltList(3, 3, false));
   } catch (const std::exception& e) {
      ASSERT_TRUE(false) << e.what();
   }

   //unlock wallets 3 & 4
   ASSERT_TRUE(unlockWallet(walletFiles[2].first.stem().string(), "controlpass3"));
   ASSERT_TRUE(unlockWallet(walletFiles[3].first.stem().string(), "controlpass4"));

   //list wallets again, check for unlocks
   try {
      auto wltList = listWallets();
      ASSERT_EQ(wltList.size(), 4);
      auto checkWltList = [&wltList, &walletFiles](
         unsigned intId, int expectedState, bool expectedStaged)->bool
      {
         auto entry = walletFiles[intId];
         auto path = entry.first.stem().string();
         auto capnEntry = wltList.at(path);

         if (capnEntry.loadState != expectedState) {
            return false;
         }

         if (expectedState == 4) {
            if (capnEntry.walletId != entry.second) {
               return false;
            }
         }
         return capnEntry.staged == expectedStaged;
      };
      EXPECT_TRUE(checkWltList(0, 4, true));
      EXPECT_TRUE(checkWltList(1, 3, false));
      EXPECT_TRUE(checkWltList(2, 4, true));
      EXPECT_TRUE(checkWltList(3, 4, true));
   } catch (const std::exception& e) {
      ASSERT_TRUE(false) << e.what();
   }

   //fail to unlock wallet 2
   ASSERT_EQ(failToUnlockWallet(walletFiles[1].first.stem().string(), 2), 3);

   //unstage wallet 4
   stageWallet(walletFiles[3].second, false);

   //list wallets again, check for staging
   try {
      auto wltList = listWallets();
      ASSERT_EQ(wltList.size(), 4);
      auto checkWltList = [&wltList, &walletFiles](
         unsigned intId, int expectedState, bool expectedStaged)->bool
      {
         auto entry = walletFiles[intId];
         auto path = entry.first.stem().string();
         auto capnEntry = wltList.at(path);

         if (capnEntry.loadState != expectedState) {
            return false;
         }

         if (expectedState == 4) {
            if (capnEntry.walletId != entry.second) {
               return false;
            }
         }
         return capnEntry.staged == expectedStaged;
      };
      EXPECT_TRUE(checkWltList(0, 4, true));
      EXPECT_TRUE(checkWltList(1, 3, false));
      EXPECT_TRUE(checkWltList(2, 4, true));
      EXPECT_TRUE(checkWltList(3, 4, false));
   } catch (const std::exception& e) {
      ASSERT_TRUE(false) << e.what();
   }

   //load wallets
   auto wallets = loadWallets();
   ASSERT_EQ(wallets.size(), 2);
   EXPECT_NE(wallets.find(walletFiles[0].second), wallets.end());
   EXPECT_NE(wallets.find(walletFiles[2].second), wallets.end());

   //list wallets one last time
   //list wallets again, check for staging
   try {
      auto wltList = listWallets();
      ASSERT_EQ(wltList.size(), 2);
      auto checkWltList = [&wltList, &walletFiles](
         unsigned intId, int expectedState, bool expectedStaged)->bool
      {
         auto entry = walletFiles[intId];
         auto path = entry.first.stem().string();
         auto capnEntry = wltList.at(path);

         if (capnEntry.loadState != expectedState) {
            return false;
         }

         if (expectedState == 4) {
            if (capnEntry.walletId != entry.second) {
               return false;
            }
         }
         return capnEntry.staged == expectedStaged;
      };
      EXPECT_TRUE(checkWltList(1, 3, false));
      EXPECT_TRUE(checkWltList(3, 4, false));
   } catch (const std::exception& e) {
      ASSERT_TRUE(false) << e.what();
   }
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(BridgeTests, CreateWallet)
{
   //create the wallet
   auto refId = rand();
   auto callbackId = BtcUtils::fortuna_.generateRandom(10).toHexStr();

   capnp::MallocMessageBuilder message;
   auto toBridge = message.initRoot<Bridge::ToBridge>();
   toBridge.setReferenceId(refId);
   auto request = toBridge.initUtils();
   auto createWltReq = request.initCreateWallet();

   createWltReq.setCallbackId(callbackId);
   createWltReq.setLookup(100);
   createWltReq.setLabel("labl");
   createWltReq.setDescription("desc");

   auto rawReq = serializeCapnp(message);
   pushRequest(rawReq);

   //handle progress notifs
   std::string masterId;
   std::filesystem::path path;
   try {
      auto walletData = progressWalletCreation(callbackId,
         "pass1", 500ms, 128, 100);
      masterId = walletData.masterId;
      path = walletData.path;
   } catch (const std::exception& e) {
      ASSERT_TRUE(false) << e.what();
   }

   //validate reply
   auto result = waitOnReply();
   kj::ArrayPtr<const capnp::word> words(
      reinterpret_cast<const capnp::word*>(result->data.getPtr()),
      result->data.getSize() / sizeof(capnp::word));
   capnp::FlatArrayMessageReader reader(words);
   auto fromBridge = reader.getRoot<Bridge::FromBridge>();
   auto reply = fromBridge.getReply();
   ASSERT_TRUE(reply.getSuccess());
   ASSERT_EQ(reply.getReferenceId(), refId);

   ASSERT_EQ(reply.which(), Bridge::RpcReply::UTILS);
   auto utilsReply = reply.getUtils();
   ASSERT_EQ(utilsReply.which(), Bridge::UtilsReply::CREATE_WALLET);
   std::string wltId = utilsReply.getCreateWallet();
   ASSERT_FALSE(wltId.empty());

   //get the wallet data & validate it
   auto wltData = getWalletData(wltId);
   EXPECT_EQ(wltData.walletId, wltId);
   EXPECT_FALSE(wltData.accountId.empty());
   EXPECT_EQ(wltData.path.stem().string(), path);
   EXPECT_EQ(wltData.masterId, masterId);

   EXPECT_EQ(wltData.label, "labl");
   EXPECT_EQ(wltData.desc, "desc");

   EXPECT_TRUE(wltData.encrypted);
   EXPECT_FALSE(wltData.watchingOnly);
   EXPECT_EQ(wltData.addresses.size(), 1);
   EXPECT_EQ(wltData.kdfMemReq, 128);

   /* extras
      1. KDF unlock time
      2. change passphrase
      3. change KDF
      4. remove passphrase
   */

   //1 request KDF unlock time
   auto unlockTime = testKDFUnlock(wltId);
   EXPECT_GE(unlockTime, 500ms) << unlockTime.count();
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(BridgeTests, RestoreWallet_Legacy)
{
   const std::string walletId{"292AxMD9H"};
   const std::vector<std::string> lines {
      "oiow rfta wueg hewo  wuaj jawj rddi uufu  tusi",
      "idnt enrd sjgo tgfi  esni eutw ktna ustg  arfe",
      "jdtf fink jshs ewda  kkor daet kgtr eiha  ejgd",
      "uaew ggod ngjk ejuu  rugf kufg awnn ofas  rhtf"
   };
   const std::string passphrase{"privPassTest"};

   //restore the wallet
   auto restoreData = restoreWallet(lines, walletId,
      Bridge::WalletBackup::Type::LEGACY135_A,
      passphrase, 300ms, 0, false, 500);

   //get the wallet data & validate it
   auto wltData = getWalletData(walletId);
   EXPECT_EQ(wltData.walletId, walletId);
   EXPECT_FALSE(wltData.accountId.empty());
   EXPECT_EQ(wltData.path.stem(), restoreData.path);
   EXPECT_EQ(wltData.masterId, restoreData.masterId);

   EXPECT_TRUE(wltData.encrypted);
   EXPECT_FALSE(wltData.watchingOnly);
   EXPECT_EQ(wltData.addresses.size(), 1);
   EXPECT_EQ(wltData.lookup, 500);

   //request KDF unlock time
   auto unlockTime = testKDFUnlock(walletId);
   EXPECT_GE(unlockTime, 300ms) << unlockTime.count();
   EXPECT_LE(unlockTime, 450ms) << unlockTime.count();

   //grab backup strings via callback
   {
      auto refId = rand();
      auto callbackId = BtcUtils::fortuna_.generateRandom(10).toHexStr();

      capnp::MallocMessageBuilder message;
      auto toBridge = message.initRoot<Bridge::ToBridge>();
      toBridge.setReferenceId(refId);
      auto request = toBridge.initWallet();
      request.setWalletId(walletId);
      auto reqBackup = request.initCreateBackupString();
      reqBackup.setCallbackId(callbackId);
      auto rawReq = serializeCapnp(message);
      pushRequest(rawReq);

      //wait for unlock request
      auto result = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      auto notif = fromBridge.getNotification();
      ASSERT_EQ(notif.getCallbackId(), callbackId);
      ASSERT_EQ(notif.which(), Bridge::Notification::UNLOCK_REQUEST);

      //reply with passphrase
      capnp::MallocMessageBuilder notifMsg;
      auto notifBridge = notifMsg.initRoot<Bridge::ToBridge>();
      auto notifReply = notifBridge.initNotification();
      notifReply.setSuccess(true);
      notifReply.setCounter(notif.getCounter());
      auto capnPass = notifReply.initWalletCreation();
      capnPass.setPassphrase(passphrase);
      capnPass.setKdfTargetMs(1);

      auto rawNotif = serializeCapnp(notifMsg);
      pushRequest(rawNotif);

      //cleanup notif
      result = waitOnReply();
      words = kj::ArrayPtr<const capnp::word>(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      reader = capnp::FlatArrayMessageReader(words);
      fromBridge = reader.getRoot<Bridge::FromBridge>();
      notif = fromBridge.getNotification();
      ASSERT_EQ(notif.which(), Bridge::Notification::CLEANUP);

      //check the backup
      auto backup = waitOnReply();
      words = kj::ArrayPtr<const capnp::word>(
         reinterpret_cast<const capnp::word*>(backup->data.getPtr()),
         backup->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader replyReader(words);
      fromBridge = replyReader.getRoot<Bridge::FromBridge>();
      ASSERT_EQ(fromBridge.which(), Bridge::FromBridge::REPLY);
      auto reply = fromBridge.getReply();
      ASSERT_TRUE(reply.getSuccess());
      ASSERT_EQ(reply.getReferenceId(), refId);

      ASSERT_EQ(reply.which(), Bridge::RpcReply::WALLET);
      auto walletReply = reply.getWallet();
      ASSERT_EQ(walletReply.which(), Bridge::WalletReply::CREATE_BACKUP_STRING);

      auto capnBackup = walletReply.getCreateBackupString();
      auto rootLines = capnBackup.getRootClear();
      ASSERT_EQ(rootLines.size(), 2);
      ASSERT_EQ(rootLines[0], lines[0]);
      ASSERT_EQ(rootLines[1], lines[1]);

      auto chainLines = capnBackup.getChainClear();
      ASSERT_EQ(chainLines.size(), 2);
      ASSERT_EQ(chainLines[0], lines[2]);
      ASSERT_EQ(chainLines[1], lines[3]);
   }

   //grab backup strings via passphrase
   try {
      auto capnLines = getBackup(walletId, passphrase,
         Bridge::WalletBackup::Type::LEGACY135_A);
      ASSERT_EQ(capnLines.size(), 4);

      for (unsigned i=0; i<4; i++) {
         ASSERT_EQ(lines[i], capnLines[i]);
      }
   } catch (const std::exception& e) {
      ASSERT_TRUE(false) << e.what();
   }
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(BridgeTests, RestoreMerge)
{
   //create the wallet
   auto refId = rand();
   auto callbackId = BtcUtils::fortuna_.generateRandom(10).toHexStr();
   std::string passphrase{"pass2"};
   std::string passphrase2{"pass3"};
   unsigned lookup = 46;

   capnp::MallocMessageBuilder message;
   auto toBridge = message.initRoot<Bridge::ToBridge>();
   toBridge.setReferenceId(refId);
   auto request = toBridge.initUtils();
   auto createWltReq = request.initCreateWallet();

   createWltReq.setCallbackId(callbackId);
   createWltReq.setLookup(lookup);
   createWltReq.setLabel("labl2");
   createWltReq.setDescription("desc2");

   auto rawReq = serializeCapnp(message);
   pushRequest(rawReq);

   //handle progress notifs
   std::string masterId;
   std::filesystem::path path;
   try {
      auto walletData = progressWalletCreation(callbackId,
         passphrase, 500ms, 128 * 1024 * 1024, lookup);
      masterId = walletData.masterId;
      path = walletData.path;
   } catch (const std::exception& e) {
      ASSERT_TRUE(false) << e.what();
   }

   //validate reply
   auto result = waitOnReply();
   kj::ArrayPtr<const capnp::word> words(
      reinterpret_cast<const capnp::word*>(result->data.getPtr()),
      result->data.getSize() / sizeof(capnp::word));
   capnp::FlatArrayMessageReader reader(words);
   auto fromBridge = reader.getRoot<Bridge::FromBridge>();
   auto reply = fromBridge.getReply();
   ASSERT_TRUE(reply.getSuccess());
   ASSERT_EQ(reply.getReferenceId(), refId);

   ASSERT_EQ(reply.which(), Bridge::RpcReply::UTILS);
   auto utilsReply = reply.getUtils();
   ASSERT_EQ(utilsReply.which(), Bridge::UtilsReply::CREATE_WALLET);
   std::string wltId = utilsReply.getCreateWallet();
   ASSERT_FALSE(wltId.empty());

   //get the wallet data & validate it
   auto wltData = getWalletData(wltId);
   EXPECT_EQ(wltData.walletId, wltId);
   EXPECT_FALSE(wltData.accountId.empty());
   EXPECT_EQ(wltData.path.stem().string(), path);
   EXPECT_EQ(wltData.masterId, masterId);

   EXPECT_EQ(wltData.label, "labl2");
   EXPECT_EQ(wltData.desc, "desc2");

   EXPECT_TRUE(wltData.encrypted);
   EXPECT_FALSE(wltData.watchingOnly);
   EXPECT_EQ(wltData.addresses.size(), 1);
   EXPECT_EQ(wltData.lookup, lookup);

   //grab 3 addresses
   std::vector<AddressData> addresses;
   addresses.emplace_back(*wltData.addresses.begin());
   for (unsigned i=0; i<3; i++) {
      addresses.emplace_back(getAddress(wltId, wltData.accountId));
   }

   for (unsigned i=0; i<4; i++) {
      const auto& addr = addresses[i];
      ASSERT_EQ(addr.index, i);
      ASSERT_FALSE(addr.hash.empty());
   }

   //grab the backup strings
   std::vector<std::string> lines;
   try {
      lines = getBackup(wltId, passphrase,
         Bridge::WalletBackup::Type::LEGACY200_A);
   } catch (const std::exception& e) {
      ASSERT_TRUE(false) << e.what();
   }

   //restore merge the wallet
   ASSERT_EQ(lines.size(), 2);
   for (const auto& line : lines) {
      ASSERT_FALSE(line.empty());
   }
   auto restoreData = restoreWallet(lines, wltId,
      Bridge::WalletBackup::Type::LEGACY200_A,
      passphrase2, 1ms, 0, true,
      //the legacy armory account always starts with asset 0
      lookup - 1
   );
   ASSERT_EQ(restoreData.masterId, masterId);
   ASSERT_EQ(restoreData.path, path);

   //validate restored wallet state
   auto wltData2 = getWalletData(wltId);
   EXPECT_EQ(wltData2.walletId, wltId);
   EXPECT_EQ(wltData2.accountId, wltData.accountId);
   EXPECT_EQ(wltData2.path.stem().string(), path);
   EXPECT_EQ(wltData2.masterId, masterId);

   EXPECT_EQ(wltData2.label, "labl2");
   EXPECT_EQ(wltData2.desc, "desc2");

   EXPECT_TRUE(wltData2.encrypted);
   EXPECT_FALSE(wltData2.watchingOnly);
   EXPECT_EQ(wltData2.addresses.size(), 4);
   EXPECT_EQ(wltData2.lookup, lookup);

   for (const auto& addr : wltData2.addresses) {
      ASSERT_LT(addr.index, 4);
      const auto& addrI = addresses[addr.index];
      ASSERT_EQ(addr.hash, addrI.hash);
   }

   //check it unlocks with passphrase2
   std::vector<std::string> lines2;
   try {
      lines2 = getBackup(wltId, passphrase2,
         Bridge::WalletBackup::Type::LEGACY200_A);
   } catch (const std::exception& e) {
      ASSERT_TRUE(false) << e.what();
   }
   EXPECT_EQ(lines, lines2);
}

////////////////////////////////////////////////////////////////////////////////
GTEST_API_ int main(int argc, char **argv)
{
   CryptoECDSA::setupContext();

   srand(time(0));
   std::cout << "Running main() from gtest_main.cc\n";

   SETLOGLEVEL(LogLvlDebug);
   //LOGENABLESTDOUT();
   LOGDISABLESTDOUT();

   testing::InitGoogleTest(&argc, argv);
   int exitCode = RUN_ALL_TESTS();

   FLUSHLOG();
   CLEANUPLOG();

   CryptoECDSA::shutdown();
   return exitCode;
}
