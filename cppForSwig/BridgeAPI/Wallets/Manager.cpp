////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016-2025, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include <filesystem>
#include <string_view>

#include "Manager.h"
#include "Wallets/Seeds/Backups.h"
#include "../PassphrasePrompt.h"
#include "../Wallets/Seeds/Seeds.h"
#include "../Wallets/KDF.h"
#include "../Wallets/IOHeader.h"
#include "../AsyncClient.h"

using namespace Armory;
using namespace Armory::Bridge;
using namespace std::string_view_literals;
using namespace std::chrono_literals;

////////////////////////////////////////////////////////////////////////////////
////
//// WalletManager
////
////////////////////////////////////////////////////////////////////////////////
WalletManager::WalletManager(const std::filesystem::path& path) :
   path_(path)
{
   if (!FileUtils::isDir(path_)) {
      std::string err{path_.string() + std::string{"is not a valid datadir"sv}};
      LOGERR << err;
      throw std::runtime_error(err);
   }
}

////
const std::filesystem::path& WalletManager::getWalletDir() const
{
   return path_;
}

////
bool WalletManager::hasWallet(const std::string& id)
{
   std::unique_lock<std::mutex> lock(mu_);
   auto wltIter = wallets_.find(id);
   return wltIter != wallets_.end();
}

////////////////////////////////////////////////////////////////////////////////
std::map<std::string, std::set<Wallets::AddressAccountId>>
WalletManager::getAccountIdMap() const
{
   std::map<std::string, std::set<Wallets::AddressAccountId>> result;
   for (const auto& wltIt : wallets_) {
      auto wltIter = result.emplace(
         wltIt.first, std::set<Wallets::AddressAccountId>{});
      for (const auto& accIt : wltIt.second) {
         wltIter.first->second.emplace(accIt.first);
      }
   }
   return result;
}

////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<WalletContainer> WalletManager::getWalletContainer(
   const std::string& wltId) const
{
   auto iter = wallets_.find(wltId);
   if (iter == wallets_.end()) {
      std::string errStr{"no wallet for id "sv};
      errStr += wltId;
      throw std::runtime_error(errStr);
   }
   return iter->second.begin()->second;
}

////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<WalletContainer> WalletManager::getWalletContainer(
   const std::string& wltId, const Wallets::AddressAccountId& accId) const
{
   auto wltIter = wallets_.find(wltId);
   if (wltIter == wallets_.end()) {
      std::string errStr{"i do not know wallet "sv};
      errStr += wltId;
      throw std::runtime_error(errStr);
   }

   auto accIter = wltIter->second.find(accId);
   if (accIter == wltIter->second.end()) {
      std::string errStr{"there is no account "sv};
      errStr += accId.toHexStr() + std::string{"for wallet "sv} + wltId;
      throw std::runtime_error(errStr);
   }

   return accIter->second;
}

////////////////////////////////////////////////////////////////////////////////
void WalletManager::setBdvPtr(
   std::shared_ptr<AsyncClient::BlockDataViewer> bdvPtr)
{
   bdvPtr_ = bdvPtr;
   for (auto& wltIt : wallets_) {
      for (auto& accIt : wltIt.second) {
         accIt.second->setBdvPtr(bdvPtr);
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
void WalletManager::registerWallets()
{
   for (const auto& wltIt : wallets_) {
      for (const auto& accIt : wltIt.second) {
         accIt.second->registerWithBDV(false);
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
const std::string& WalletManager::registerWallet(const std::string& wltId,
   const Wallets::AddressAccountId& accId, bool isNew)
{
   auto wltIter = wallets_.find(wltId);
   if (wltIter == wallets_.end()) {
      throw std::runtime_error("[WalletManager::registerWallet]");
   }

   auto accIter = wltIter->second.find(accId);
   if (accIter == wltIter->second.end()) {
      throw std::runtime_error("[WalletManager::registerWallet]");
   }

   accIter->second->registerWithBDV(isNew);
   return accIter->second->getDbId();
}

////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<WalletContainer> WalletManager::addAccount(
   std::shared_ptr<Wallets::AssetWallet> wltPtr,
   const Wallets::AddressAccountId& accId)
{
   ReentrantLock lock(this);

   //check we dont have this wallet
   auto wltIter = wallets_.find(wltPtr->getID());
   if (wltIter == wallets_.end()) {
      wltIter = wallets_.emplace(wltPtr->getID(),
         std::map<Wallets::AddressAccountId, std::shared_ptr<WalletContainer>>{}).first;
   }

   auto accIter = wltIter->second.find(accId);
   if (accIter != wltIter->second.end()) {
      return accIter->second;
   }

   //create wrapper object
   auto wltContPtr = new WalletContainer(wltPtr->getID(), accId);
   std::shared_ptr<WalletContainer> wltCont;
   wltCont.reset(wltContPtr);

   //set bdvPtr if we have it
   if (bdvPtr_ != nullptr) {
      wltCont->setBdvPtr(bdvPtr_);
   }

   //set & add to map
   wltCont->setWalletPtr(wltPtr, accId);
   wltIter->second.emplace(accId, wltCont);
   walletsByDbId_.emplace(wltCont->getDbId(), wltCont);

   //return it
   return wltCont;
}

////
void WalletManager::addAllAccounts(std::shared_ptr<Wallets::AssetWallet> wltPtr)
{
   const auto& accIds = wltPtr->getAccountIDs();
   for (const auto& accId : accIds) {
      addAccount(wltPtr, accId);
   }
}

////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<WalletContainer> WalletManager::createNewWallet(
   const SecureBinaryData& extraEntropy,
   const Wallets::IO::CreationParams& params)
{
   auto root = CryptoPRNG::generateRandom(32);
   if (!extraEntropy.empty()) {
      auto hashTropy = BtcUtils::getHash256(extraEntropy);
      root.XOR(hashTropy);
   }

   auto seed = std::make_unique<Seeds::ClearTextSeed_Armory135>(
      root, Seeds::ClearTextSeed_Armory135::LegacyType::Armory200);
   auto wallet = Wallets::AssetWallet_Single::createFromSeed(
      std::move(seed), params);
   return addAccount(wallet, wallet->getMainAccountID());
}

////////////////////////////////////////////////////////////////////////////////
std::filesystem::path WalletManager::unloadWallet(const std::string& wltId)
{
   ReentrantLock lock(this);
   auto iter = wallets_.find(wltId);
   if (iter == wallets_.end()) {
      return {};
   }

   //unregister all accounts
   std::filesystem::path path;
   for (auto& acc : iter->second) {
      try {
         if (path.empty()) {
            path = acc.second->getWalletPtr()->getDbFilename();
         }
         acc.second->unregisterFromBDV();
      } catch (const std::exception&) {
         //we do not care if the unregister operation fails
      }
   }

   //remove containers from map, this should unload the underlying AssetWallet
   wallets_.erase(wltId);
   return path;
}

////////////////////////////////////////////////////////////////////////////////
void WalletManager::deleteWallet(const std::string& wltId)
{
   ReentrantLock lock(this);
   auto wltCont = getWalletContainer(wltId);
   wallets_.erase(wltId);

   //delete from disk
   wltCont->eraseFromDisk();
   try {
      //unregister from db
      wltCont->unregisterFromBDV();
   } catch (const std::exception&) {
      //we do not care if the unregister operation fails
   }
   wltCont.reset();
}

////////////////////////////////////////////////////////////////////////////////
void WalletManager::loadWallet(const Wallets::IO::OpenFileParams& params)
{
   auto wltPtr = Wallets::AssetWallet::loadMainWalletFromFile(params);
   addAllAccounts(wltPtr);
}

////////////////////////////////////////////////////////////////////////////////
std::map<std::string, std::shared_ptr<WalletFileInfo>>
WalletManager::listWallets()
{
   //iterate over files in datadir
   std::vector<std::filesystem::path> walletPaths, a135Paths;
   for (const auto& dirEntry : std::filesystem::directory_iterator{path_} ) {
      const auto& path = dirEntry.path();
      const auto& extension = path.extension();

      //ignore this file if we've parsed it before
      auto iter = walletFiles_.find(path.stem().string());
      if (iter != walletFiles_.end()) {
         continue;
      }

      if (extension == ".lmdb") {
         walletPaths.emplace_back(path);
      } else if (extension == ".wallet") {
         a135Paths.emplace_back(path);
      }
   }

   //read the potential wallet files
   ReentrantLock lock(this);
   for (const auto& wltPath : walletPaths) {
      try {
         auto wltPtr = Wallets::AssetWallet::loadMainWalletFromFile(
            Wallets::IO::OpenFileParams{wltPath});
         walletFiles_.emplace(wltPath.stem().string(),
            std::make_shared<LMDBWalletInfo>(wltPath, wltPtr));
      } catch (const Wallets::Encryption::DecryptedDataContainerException& e) {
         //could not decrypt wallet control header, track as encrypted wallet
         LOGDEBUG << "could not open wallet with decryption error: " << e.what();
         walletFiles_.emplace(wltPath.stem().string(),
            std::make_shared<LMDBWalletInfo>(wltPath, nullptr));
      }
   }

   //parse the potential armory 1.35 wallet files
   for (const auto& wltPath : a135Paths) {
      auto a135 = std::make_shared<Armory135Header>(wltPath);
      if (!a135->isInitialized()) {
         continue;
      }

      //an armory v1.35 wallet was loaded, check if we need to
      //migrate it to v3.x
      auto& id = a135->getID();
      auto iter = wallets_.find(id);
      if (iter != wallets_.end()) {
         continue;
      }

      //no equivalent v3.x wallet loaded, add it to the list
      walletFiles_.emplace(wltPath.stem().string(),
         std::make_shared<A135FileInfo>(a135));
   }

   std::map<std::string, std::shared_ptr<WalletFileInfo>> result;
   for (const auto& entry : walletFiles_) {
      switch (entry.second->state())
      {
         case WalletLoadState::Migrated:
         case WalletLoadState::Loaded:
            break;

         default:
            result.emplace(entry);
      }
   }
   return result;
}

/////////
void WalletManager::unlockControlHeader(const std::string& path,
   const PassphraseLambda& lbd)
{
   //sanity checks
   if (path.empty() || lbd == nullptr) {
      throw std::runtime_error("tried to unlock control header with empty id/lambda");
   }

   auto iter = walletFiles_.find(path);
   if (iter == walletFiles_.end()) {
      throw std::runtime_error("this file is not a known wallet: " + path);
   }

   auto infoObj = std::dynamic_pointer_cast<LMDBWalletInfo>(iter->second);
   if (infoObj == nullptr) {
      throw std::runtime_error("invalid wallet info type");
   }

   infoObj->unlockControlHeader(lbd);
}

/////////
bool WalletManager::stageWallet(const std::string& walletId, bool stage)
{
   for (auto& knownFile : walletFiles_) {
      if (knownFile.second->walletId() == walletId) {
         try {
            knownFile.second->setStaged(stage);
            return true;
         } catch (const std::exception&) {
            return false;
         }
      }
   }
   return false;
}

/////////
void WalletManager::loadWallets()
{
   ReentrantLock lock(this);
   listWallets();
   for (auto& entry : walletFiles_) {
      auto wltFile = std::dynamic_pointer_cast<LMDBWalletInfo>(entry.second);
      if (wltFile == nullptr) {
         continue;
      }
      try {
         auto wltPtr = wltFile->moveWltPtr();
         addAllAccounts(wltPtr);
      } catch (const std::exception&) {
         continue;
      }
   }
}

////////////////////////////////////////////////////////////////////////////////
void WalletManager::updateStateFromDB(const std::function<void(void)>& callback)
{
   auto lbd = [this, callback](void)->void
   {
      ReentrantLock lock(this);

      //grab wallet balances
      auto promBal = std::make_shared<std::promise<std::map<
         std::string, AsyncClient::CombinedBalances>>>();
      auto futBal = promBal->get_future();
      auto lbdBal = [promBal]
         (ReturnMessage<std::map<std::string, AsyncClient::CombinedBalances>> result)->void
      {
         promBal->set_value(result.get());
      };
      bdvPtr_->getCombinedBalances(lbdBal);
      auto balances = std::move(futBal.get());

      //update wallet balances
      for (const auto& wltBalance : balances) {
         auto wltContIter = walletsByDbId_.find(wltBalance.first);
         if (wltContIter == walletsByDbId_.end()) {
            continue;
         }
         wltContIter->second->updateWalletBalanceState(wltBalance.second);
         wltContIter->second->updateAddressCountState(wltBalance.second);
      }

      //fire the lambda
      callback();
   };

   std::thread thr(lbd);
   if (thr.joinable()) {
      thr.detach();
   }
}
