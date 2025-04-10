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

   struct WalletData
   {
      const std::string walletId;
      const std::string accountId;
      const std::string masterId;

      const std::string label;
      const std::string desc;

      const bool encrypted;
      const bool watchingOnly;
      const std::set<BinaryData> addresses;

      const std::filesystem::path path;
   };

   WalletData capnToWalletData(const Bridge::WalletData::Reader& capnWlt)
   {
      auto capnAddrs = capnWlt.getAddressData();
      std::set<BinaryData> addresses;
      for (const auto& capnAddr : capnAddrs) {
         auto addrHash = capnAddr.getPrefixedHash();
         addresses.emplace(BinaryData{addrHash.begin(), addrHash.end()});
      }

      std::string walletId = capnWlt.getWalletId();
      return WalletData{
         walletId, capnWlt.getAccountId(), capnWlt.getMasterId(),
         capnWlt.getLabel(), capnWlt.getDesc(),
         capnWlt.getUsesEncryption(), capnWlt.getWatchingOnly(),
         std::move(addresses), std::filesystem::path(capnWlt.getPath())
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
      IO::CreationParams params{
         homedir_,
         SecureBinaryData::fromString("privpass1"), 1ms,
         {}, 1ms,
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
      IO::CreationParams params{
         homedir_,
         SecureBinaryData::fromString("privpass" + std::to_string(i)), 1ms,
         SecureBinaryData::fromString("controlpass" + std::to_string(i)), 1ms,
         nullptr, 4
      };

      std::unique_ptr<Armory::Seeds::ClearTextSeed> seed(
         new Armory::Seeds::ClearTextSeed_Armory135());
      auto assetWlt = AssetWallet_Single::createFromSeed(
         std::move(seed), params);
      walletFiles.emplace_back(assetWlt->getDbFilename());
   }

   //list wallets
   WalletManager mgr{homedir_};
   auto theList = mgr.listWallets();
   ASSERT_EQ(theList.size(), 4);
   for (const auto& path : walletFiles) {
      ASSERT_NE(theList.find(path.stem().string()), theList.end());
   }

   auto checkState = [&walletFiles](
      const std::map<std::string, WalletFileInfo>& theList,
      unsigned intId, WalletLoadState expLoadState, bool expectedStaged)->bool
   {
      auto listEntry = theList.at(walletFiles[intId].stem().string());
      if (listEntry.loadState != expLoadState) {
         return false;
      }
      if (listEntry.loadState == WalletLoadState::Ready &&
         listEntry.walletId.empty()) {
         return false;
      }
      return listEntry.staged == expectedStaged;
   };

   EXPECT_TRUE(checkState(theList, 0, WalletLoadState::Ready, true));
   EXPECT_TRUE(checkState(theList, 1, WalletLoadState::Encrypted, false));
   EXPECT_TRUE(checkState(theList, 2, WalletLoadState::Encrypted, false));
   EXPECT_TRUE(checkState(theList, 3, WalletLoadState::Encrypted, false));

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
   EXPECT_TRUE(checkState(theList, 0, WalletLoadState::Ready, true));
   EXPECT_TRUE(checkState(theList, 1, WalletLoadState::Encrypted, false));
   EXPECT_TRUE(checkState(theList, 2, WalletLoadState::Ready, true));
   EXPECT_TRUE(checkState(theList, 3, WalletLoadState::Ready, true));

   //unstage wlt4
   {
      auto wlt4Info = theList.at(walletFiles[3].stem().string());
      ASSERT_TRUE(mgr.stageWallet(wlt4Info.walletId, false));
   }

   //recheck the list
   theList = mgr.listWallets();
   ASSERT_EQ(theList.size(), 4);
   for (const auto& path : walletFiles) {
      ASSERT_NE(theList.find(path.stem().string()), theList.end());
   }
   EXPECT_TRUE(checkState(theList, 0, WalletLoadState::Ready, true));
   EXPECT_TRUE(checkState(theList, 1, WalletLoadState::Encrypted, false));
   EXPECT_TRUE(checkState(theList, 2, WalletLoadState::Ready, true));
   EXPECT_TRUE(checkState(theList, 3, WalletLoadState::Ready, false));

   //load wallets
   mgr.loadWallets();

   //check loaded wallets
   auto checkHasWallet = [&theList, &walletFiles, &mgr](unsigned intId)->bool
   {
      auto entry = theList.at(walletFiles[intId].stem().string());
      if (entry.walletId.empty()) {
         return false;
      }
      return mgr.hasWallet(entry.walletId);
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
         auto capnPassphrases = notifReply.initPassphrases(1);
         capnPassphrases.set(0, passphrase);

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
            notifReply.setSuccess(true);
            notifReply.setCounter(counter);

            if (attempts != count) {
               auto badPass = BtcUtils::fortuna_.generateRandom(10).toHexStr();
               auto capnPassphrases = notifReply.initPassphrases(1);
               capnPassphrases.set(0, badPass);
            } else {
               auto capnPassphrases = notifReply.initPassphrases(0);
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
      IO::CreationParams params{
         homedir,
         SecureBinaryData::fromString("privpass1"), 1ms,
         {}, 1ms,
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
      IO::CreationParams params{
         homedir,
         SecureBinaryData::fromString("privpass" + std::to_string(i)), 1ms,
         SecureBinaryData::fromString("controlpass" + std::to_string(i)), 1ms,
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
   } catch (...) {
      ASSERT_TRUE(false);
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

   createWltReq.setPrivPassphrase("pass1");
   createWltReq.setPrivKdfTargetMs(500);
   createWltReq.setPrivKdfTargetMB(128 * 1024 * 1024);

   auto rawReq = serializeCapnp(message);
   pushRequest(rawReq);

   //handle progress notifs
   std::filesystem::path wltPath;
   std::string masterId;
   unsigned notifCount = 0;

   while (true) {
      auto result = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      ASSERT_EQ(fromBridge.which(), Bridge::FromBridge::NOTIFICATION);

      auto notif = fromBridge.getNotification();
      ASSERT_EQ(notif.getCallbackId(), callbackId);

      if (notif.which() != Bridge::Notification::WALLET_PROGRESS) {
         ASSERT_EQ(notif.which(), Bridge::Notification::CLEANUP);
         break;
      }

      auto wltNotif = notif.getWalletProgress();
      switch (wltNotif.which())
      {
         case Bridge::Notification::WalletProgress::CREATE_FILE:
         {
            ASSERT_EQ(notifCount++, 0);
            wltPath = std::filesystem::path(wltNotif.getCreateFile());
            break;
         }

         case Bridge::Notification::WalletProgress::INIT_FILE:
         {
            ASSERT_EQ(notifCount++, 1);
            masterId = wltNotif.getInitFile();
            break;
         }

         case Bridge::Notification::WalletProgress::READ_FILE:
         {
            ASSERT_EQ(notifCount++, 2);
            ASSERT_EQ(wltNotif.getReadFile(), masterId);
            break;
         }

         case Bridge::Notification::WalletProgress::CREATE_ACCOUNT:
         {
            ASSERT_EQ(notifCount++, 3);
            EXPECT_EQ(wltNotif.getCreateAccount(), "Armory Legacy");
            break;
         }

         case Bridge::Notification::WalletProgress::EXTEND_CHAIN:
         {
            ASSERT_EQ(notifCount++, 4);
            auto extendNotif = wltNotif.getExtendChain();
            EXPECT_EQ(extendNotif.getTotal(), 100);
            EXPECT_EQ(extendNotif.getCurrent(), 0);
            break;
         }

         default:
            ASSERT_TRUE(false);
      }
   }
   ASSERT_EQ(notifCount, 5);

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
   EXPECT_EQ(wltData.path.stem().string(), wltPath);
   EXPECT_EQ(wltData.masterId, masterId);

   EXPECT_EQ(wltData.label, "labl");
   EXPECT_EQ(wltData.desc, "desc");

   EXPECT_TRUE(wltData.encrypted);
   EXPECT_FALSE(wltData.watchingOnly);
   EXPECT_EQ(wltData.addresses.size(), 1);
}

////////////////////////////////////////////////////////////////////////////////
TEST_F(BridgeTests, RestoreWallet_Legacy)
{
   const std::string walletId{"292AxMD9H"};

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
      rootLines.set(0, "oiow rfta wueg hewo  wuaj jawj rddi uufu  tusi");
      rootLines.set(1, "idnt enrd sjgo tgfi  esni eutw ktna ustg  arfe");

      auto ccLines = restoreWltReq.initChaincode(2);
      ccLines.set(0, "jdtf fink jshs ewda  kkor daet kgtr eiha  ejgd");
      ccLines.set(1, "uaew ggod ngjk ejuu  rugf kufg awnn ofas  rhtf");

      auto rawReq = serializeCapnp(message);
      pushRequest(rawReq);
   }

   //handle notifs
   std::string wltPath;
   std::string masterId;

   bool run = true;
   unsigned notifCount = 0;
   while (run) {
      auto result = waitOnReply();
      kj::ArrayPtr<const capnp::word> words(
         reinterpret_cast<const capnp::word*>(result->data.getPtr()),
         result->data.getSize() / sizeof(capnp::word));
      capnp::FlatArrayMessageReader reader(words);
      auto fromBridge = reader.getRoot<Bridge::FromBridge>();
      ASSERT_EQ(fromBridge.which(), Bridge::FromBridge::NOTIFICATION);

      auto notif = fromBridge.getNotification();
      ASSERT_EQ(notif.getCallbackId(), callbackId);

      switch (notif.which())
      {
         case Bridge::Notification::CLEANUP:
         {
            //exit condition
            run = false;
            break;
         }

         case Bridge::Notification::RESTORE:
         {
            auto restoreNotif = notif.getRestore();
            switch (restoreNotif.which())
            {
               case Bridge::Notification::RestorePrompt::CHECK_WALLET_ID:
               {
                  //return merge decision
                  auto meta = restoreNotif.getCheckWalletId();
                  ASSERT_EQ(meta.getWalletId(), walletId);
                  ASSERT_EQ(meta.getBackupType(), 0);

                  auto counter = notif.getCounter();
                  capnp::MallocMessageBuilder notifMsg;
                  auto toBridge = notifMsg.initRoot<Bridge::ToBridge>();
                  toBridge.setReferenceId(rand());

                  auto notifReply = toBridge.initNotification();
                  notifReply.setSuccess(true);
                  notifReply.setCounter(counter);
                  notifReply.setRestore(
                     Bridge::NotificationReply::RestoreMode::OVERWRITE);

                  auto rawReq = serializeCapnp(notifMsg);
                  pushRequest(rawReq);
                  break;
               }

               case Bridge::Notification::RestorePrompt::GET_PASSPHRASES:
               {
                  //return passphrase
                  auto counter = notif.getCounter();
                  capnp::MallocMessageBuilder notifMsg;
                  auto toBridge = notifMsg.initRoot<Bridge::ToBridge>();
                  toBridge.setReferenceId(rand());

                  auto notifReply = toBridge.initNotification();
                  notifReply.setSuccess(true);
                  notifReply.setCounter(counter);
                  auto pass = notifReply.initPassphrases(1);
                  pass.set(0, "privPass");

                  auto rawReq = serializeCapnp(notifMsg);
                  pushRequest(rawReq);
                  break;
               }

               default:
                  ASSERT_TRUE(false);
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
                  ASSERT_EQ(notifCount++, 0);
                  wltPath = std::filesystem::path(wltNotif.getCreateFile());
                  break;
               }

               case Bridge::Notification::WalletProgress::INIT_FILE:
               {
                  ASSERT_EQ(notifCount++, 1);
                  masterId = wltNotif.getInitFile();
                  break;
               }

               case Bridge::Notification::WalletProgress::READ_FILE:
               {
                  ASSERT_EQ(notifCount++, 2);
                  ASSERT_EQ(wltNotif.getReadFile(), masterId);
                  break;
               }

               case Bridge::Notification::WalletProgress::CREATE_ACCOUNT:
               {
                  ASSERT_EQ(notifCount++, 3);
                  EXPECT_EQ(wltNotif.getCreateAccount(), "Armory Legacy");
                  break;
               }

               case Bridge::Notification::WalletProgress::EXTEND_CHAIN:
               {
                  ASSERT_EQ(notifCount++, 4);
                  auto extendNotif = wltNotif.getExtendChain();
                  EXPECT_EQ(extendNotif.getTotal(), 100);
                  EXPECT_EQ(extendNotif.getCurrent(), 0);
                  break;
               }

               default:
                  ASSERT_TRUE(false);
            }
            break;
         }

         default:
            ASSERT_TRUE(false);
      }
   };

   ASSERT_EQ(notifCount, 5);

   //validate reply
   auto result = waitOnReply();
   kj::ArrayPtr<const capnp::word> words(
      reinterpret_cast<const capnp::word*>(result->data.getPtr()),
      result->data.getSize() / sizeof(capnp::word));
   capnp::FlatArrayMessageReader reader(words);
   auto fromBridge = reader.getRoot<Bridge::FromBridge>();
   ASSERT_EQ(fromBridge.which(), Bridge::FromBridge::REPLY);
   auto reply = fromBridge.getReply();
   ASSERT_TRUE(reply.getSuccess());
   ASSERT_EQ(reply.getReferenceId(), refId);

   //get the wallet data & validate it
   auto wltData = getWalletData(walletId);
   EXPECT_EQ(wltData.walletId, walletId);
   EXPECT_FALSE(wltData.accountId.empty());
   EXPECT_EQ(wltData.path.stem().string(), wltPath);
   EXPECT_EQ(wltData.masterId, masterId);

   EXPECT_TRUE(wltData.encrypted);
   EXPECT_FALSE(wltData.watchingOnly);
   EXPECT_EQ(wltData.addresses.size(), 1);
}

////////////////////////////////////////////////////////////////////////////////
GTEST_API_ int main(int argc, char **argv)
{
   CryptoECDSA::setupContext();

   srand(time(0));
   std::cout << "Running main() from gtest_main.cc\n";

   LOGDISABLESTDOUT();

   testing::InitGoogleTest(&argc, argv);
   int exitCode = RUN_ALL_TESTS();

   FLUSHLOG();
   CLEANUPLOG();

   CryptoECDSA::shutdown();
   return exitCode;
}
