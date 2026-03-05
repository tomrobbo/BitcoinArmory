////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016-2025, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "Container.h"
#include <Utils/BtcUtils.h>
#include <Utils/Cryptography.h>

#include <Wallets/Wallets.h>
#include <Wallets/Addresses.h>
#include <Wallets/Accounts/AddressAccounts.h>
#include <Wallets/Seeds/Backups.h>
#include <BlockchainDatabase/txio.h>
#include <AsyncClient.h>
#include "TxIOCache.h"

using namespace Armory;
using namespace Armory::Bridge;

////////////////////////////////////////////////////////////////////////////////
////
//// WalletContainer
////
////////////////////////////////////////////////////////////////////////////////
WalletContainer::WalletContainer(
   const Wallets::WalletId& wltId,
   const Armory::Wallets::AddressAccountId& accId,
   std::shared_ptr<TxIOCache> cache) :
   wltId_(wltId), accountId_(accId), cache_(cache)
{
   dbId_ = Cryptography::PRNG::fortuna.generateRandom(6).toHexStr();
   chainDataMain_.reset();
   chainDataZC_.reset();
}

////////////////////////////////////////////////////////////////////////////////
const std::string& WalletContainer::getDbId() const
{
   return dbId_;
}

////////////////////////////////////////////////////////////////////////////////
const Armory::Wallets::AddressAccountId& WalletContainer::getAccountId() const
{
   return accountId_;
}

////////////////////////////////////////////////////////////////////////////////
void WalletContainer::setWalletPtr(std::shared_ptr<Wallets::AssetWallet> wltPtr,
   const Wallets::AddressAccountId& accId)
{
   wallet_ = wltPtr;
   auto acc = wallet_->getAccountForID(accId);
   auto assetAccountIds = acc->getAccountIdSet();

   for (const auto& aaId : assetAccountIds) {
      auto accPtr = acc->getAccountForID(aaId);
      if (accPtr == nullptr) {
         throw std::runtime_error("[setWalletPtr] missing asset account id");
      }
      highestUsedIndex_.emplace(aaId, accPtr->getHighestUsedIndex());
   }
}

////////////////////////////////////////////////////////////////////////////////
void WalletContainer::setBdvPtr(std::shared_ptr<AsyncClient::BlockDataViewer> bdv)
{
   std::unique_lock<std::mutex> lock(stateMutex_);
   bdvPtr_ = bdv;
}

////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<Wallets::AssetWallet> WalletContainer::getWalletPtr() const
{
   return wallet_;
}

////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<Accounts::AddressAccount>
WalletContainer::getAddressAccount() const
{
   auto accPtr = wallet_->getAccountForID(accountId_);
   return accPtr;
}

////////////////////////////////////////////////////////////////////////////////
void WalletContainer::resetCache()
{
   std::unique_lock<std::mutex> lock(stateMutex_);
   chainDataMain_.reset();
   chainDataZC_.reset();
}

////////////////////////////////////////////////////////////////////////////////
void WalletContainer::registerWithBDV(bool isNew)
{
   if (bdvPtr_ == nullptr) {
      throw OfflineException();
   }
   resetCache();

   auto accPtr = wallet_->getAccountForID(accountId_);
   const auto& addrMap = accPtr->getAddressHashMap();

   std::set<BinaryData> addrSet;
   for (const auto& addrIt : addrMap) {
      addrSet.emplace(addrIt.first);
   }

   //convert set to vector
   std::vector<BinaryData> addrVec;
   addrVec.insert(addrVec.end(), addrSet.begin(), addrSet.end());

   asyncWlt_ = std::make_shared<AsyncClient::BtcWallet>(
      bdvPtr_->getWalletObj(dbId_));
   asyncWlt_->registerAddresses(addrVec, isNew);
}

////////////////////////////////////////////////////////////////////////////////
void WalletContainer::unregisterFromBDV()
{
   if (bdvPtr_ == nullptr) {
      throw OfflineException();
   }
   if (asyncWlt_ == nullptr) {
      throw OfflineException();
   }
   asyncWlt_->unregister();
}

uint64_t WalletContainer::getFullBalance() const
{
   if (chainDataMain_ == nullptr) {
      return 0;
   }
   return chainDataMain_->totalBalance + chainDataZC_->totalBalance;
}

uint64_t WalletContainer::getSpendableBalance() const
{
   if (chainDataMain_ == nullptr) {
      return 0;
   }
   return chainDataMain_->spendableBalance + chainDataZC_->spendableBalance;
}

uint64_t WalletContainer::getUnconfirmedBalance() const
{
   if (chainDataMain_ == nullptr) {
      return 0;
   }
   return chainDataMain_->unconfirmedBalance + chainDataZC_->unconfirmedBalance;
}

uint64_t WalletContainer::getTxIOCount() const
{
   if (chainDataMain_ == nullptr) {
      return 0;
   }
   return chainDataMain_->txioCount + chainDataZC_->txioCount;
}

////////////////////////////////////////////////////////////////////////////////
Wallets::AssetKeyType WalletContainer::getHighestUsedIndex() const
{
   auto accPtr = wallet_->getAccountForID(accountId_);
   if (accPtr == nullptr) {
      throw std::runtime_error("[getHighestUsedIndex] invalid acc id");
   }

   auto outerAccId = accPtr->getOuterAccountID();
   if (!outerAccId.isValid()) {
      throw std::runtime_error("[getHighestUsedIndex] invalid outer acc id");
   }

   auto indexIter = highestUsedIndex_.find(outerAccId);
   if (indexIter == highestUsedIndex_.end()) {
      throw std::runtime_error("[getHighestUsedIndex] missing index for id");
   }

   return indexIter->second;
}

////////////////////////////////////////////////////////////////////////////////
void WalletContainer::updateAddressCountState(
   const AsyncClient::CombinedBalances& cnt)
{
   /***
   TODO:
      Need to integrate this to local address/count computing.
      This code compares local address chain length vs on-chain balance
      data and reconciliates them. Crucial when restoring wallets.
   ***/
   std::unique_lock<std::mutex> lock(stateMutex_);

   /*
   std::map<Wallets::AssetAccountId, Wallets::AssetKeyType> topIndexMap;
   std::shared_ptr<Wallets::IO::WalletIfaceTransaction> dbtx;
   std::map<BinaryData, std::shared_ptr<AddressEntry>> updatedAddressMap;
   std::map<Wallets::AssetId, AddressEntryType> addrAndTypeMap;

   for (const auto& addrPair : cnt.addressBalances) {
      auto iter = countMap_.find(addrPair.first);
      if (iter != countMap_.end()) {
         //already tracking count for this address, just update the value
         iter->second = addrPair.second[3];
         continue;
      }

      const auto& ID = wallet_->getAssetIDForScrAddr(addrPair.first);
      auto topIdIter = topIndexMap.find(ID.first.getAssetAccountId());
      if (topIdIter == topIndexMap.end()) {
         topIdIter = topIndexMap.emplace(ID.first.getAssetAccountId(), -1).first;
      }

      //track top used index
      auto idKey = ID.first.getAssetKey();
      if (idKey > topIdIter->second) {
         topIdIter->second = idKey;
      }

      //mark newly seen addresses for further processing
      addrAndTypeMap.emplace(ID);

      //add count to map
      countMap_.emplace(addrPair.first, addrPair.second[3]);
   }

   std::map<Wallets::AssetId, AddressEntryType> unpulledAddresses;
   for (const auto& idPair : addrAndTypeMap) {
      //check scrAddr with on chain data matches scrAddr for
      //address entry in wallet
      if (!wallet_->isAssetUsed(idPair.first)) {
         //db has history for an address that hasn't been pulled
         //from the wallet yet, save it for further processing
         unpulledAddresses.insert(idPair);
         continue;
      }

      auto addrType = wallet_->getAddrTypeForID(idPair.first);
      if (addrType == idPair.second) {
         continue;
      }

      //if we don't have a db tx yet, get one, as we're about to update
      //the address type on disk
      if (dbtx == nullptr) {
         dbtx = wallet_->beginSubDBTransaction(wallet_->getID(), true);
      }

      //address type mismatches, update it
      wallet_->updateAddressEntryType(idPair.first, idPair.second);

      auto addrPtr = wallet_->getAddressEntryForID(idPair.first);
      updatedAddressMap.emplace(addrPtr->getPrefixedHash(), addrPtr);
   }

   //split unpulled addresses by their accounts
   std::map<Wallets::AssetAccountId,
      std::map<Wallets::AssetId, AddressEntryType>> accIDMap;
   for (const auto& idPair : unpulledAddresses) {
      auto accID = idPair.first.getAssetAccountId();
      auto iter = accIDMap.find(accID);
      if (iter == accIDMap.end()) {
         iter = accIDMap.emplace(accID,
            std::map<Wallets::AssetId, AddressEntryType>()).first;
      }
      iter->second.insert(idPair);
   }

   if (dbtx == nullptr) {
      dbtx = wallet_->beginSubDBTransaction(wallet_->getID(), true);
   }

   //run through each account, pulling addresses accordingly
   for (const auto& accData : accIDMap) {
      auto addrAccount = wallet_->getAccountForID(
         accData.first.getAddressAccountId());
      auto assAccount = addrAccount->getAccountForID(accData.first);

      auto currentTop = assAccount->getHighestUsedIndex();
      for (auto& idPair : accData.second) {
         const auto& assetKey = idPair.first.getAssetKey();
         while (assetKey > currentTop + 1) {
            auto addrEntry = wallet_->getNewAddress(
               accData.first, AddressEntryType::Default);
            updatedAddressMap.emplace(
               addrEntry->getPrefixedHash(), addrEntry);

            ++currentTop;
         }

         auto addrEntry = wallet_->getNewAddress(
            accData.first, idPair.second);
         updatedAddressMap.emplace(
            addrEntry->getPrefixedHash(), addrEntry);
         ++currentTop;
      }
   }

   for (const auto& topIndexIt : topIndexMap) {
      auto usedIndexIter = highestUsedIndex_.find(topIndexIt.first);
      if (usedIndexIter == highestUsedIndex_.end()) {
         LOGWARN << "[updateAddressCountState]" <<
            " missing asset account, skipping";
         continue;
      }

      usedIndexIter->second = std::max(
         topIndexIt.second,
         usedIndexIter->second);
   }

   for (const auto& addrPair : updatedAddressMap) {
      auto insertIter = updatedAddressMap_.insert(addrPair);
      if (!insertIter.second) {
         insertIter.first->second = addrPair.second;
      }
   }
   */
}

////////////////////////////////////////////////////////////////////////////////
std::map<BinaryData, std::vector<uint64_t>>
WalletContainer::getAddrBalanceMap() const
{
   if (chainDataMain_ == nullptr) {
      return {};
   }

   std::map<BinaryData, std::vector<uint64_t>> result;
   for (const auto& countPair : chainDataMain_->countMap) {
      std::vector<uint64_t> balVec;
      auto iter = chainDataMain_->balanceMap.find(countPair.first);
      if (iter == chainDataMain_->balanceMap.end()) {
         balVec = {0, 0, 0};
      } else {
         balVec = {
            static_cast<uint64_t>(iter->second[0]),
            static_cast<uint64_t>(iter->second[1]),
            static_cast<uint64_t>(iter->second[2])
         };
      }

      balVec.emplace_back(countPair.second);
      result.emplace(countPair.first, balVec);
   }

   for (const auto& balPair : chainDataZC_->balanceMap) {
      auto& balVec = result.at(balPair.first);
      balVec[0] += balPair.second[0];
      balVec[1] += balPair.second[1];
      balVec[2] += balPair.second[2];
   }
   for (const auto& countPair : chainDataZC_->countMap) {
      auto& balVec = result.at(countPair.first);
      balVec[3] += countPair.second;
   }
   return result;
}

////////////////////////////////////////////////////////////////////////////////
std::vector<AddressBookEntry> WalletContainer::getAddressBook() const
{
   auto addrHashMap = cache_->getAddressBook(
      [this](const BinaryData& scrAddr)->bool
      { return this->hasScrAddr(scrAddr); }
   );

   std::vector<AddressBookEntry> result;
   result.reserve(addrHashMap.size());
   for (auto& hashSet : addrHashMap) {
      AddressBookEntry ae{hashSet.first.getRef()};
      for (auto& hash : hashSet.second) {
         ae.addTxHash(std::move(hash));
      }
      result.emplace_back(std::move(ae));
   }
   return result;
}

////////////////////////////////////////////////////////////////////////////////
std::map<BinaryData, std::shared_ptr<AddressEntry>>
WalletContainer::getUpdatedAddressMap()
{
   auto mapMove = std::move(updatedAddressMap_);
   updatedAddressMap_.clear();
   return mapMove;
}

////////////////////////////////////////////////////////////////////////////////
std::unique_ptr<Seeds::WalletBackup> WalletContainer::getBackupStrings(
   bool isPriv, const Passphrase::UnlockFunc& passLbd) const
{
   auto wltSingle = std::dynamic_pointer_cast<Wallets::AssetWallet_Single>(wallet_);
   if (wltSingle == nullptr) {
      LOGERR << "WalletContainer::getBackupStrings: unexpected wallet type";
      throw std::runtime_error(
         "WalletContainer::getBackupStrings: unexpected wallet type");
   }

   wltSingle->setPassphrasePromptLambda(passLbd);
   auto backupStrings = Seeds::Helpers::getWalletBackup(wltSingle, isPriv);
   wltSingle->resetPassphrasePromptLambda();

   return backupStrings;
}

void WalletContainer::changePassphrase(const Passphrase::UnlockFunc& unlockLbd,
   Passphrase::SetNew& setLbd, bool isPriv)
{
   auto wltSingle = std::dynamic_pointer_cast<Wallets::AssetWallet_Single>(wallet_);
   if (wltSingle == nullptr) {
      LOGERR << "WalletContainer::changePassphrase: unexpected wallet type";
      throw std::runtime_error(
         "WalletContainer::changePassphrase: unexpected wallet type");
   }

   if (isPriv) {
      wltSingle->setPassphrasePromptLambda(unlockLbd);
      wltSingle->changePrivateKeyPassphrase(setLbd);
      wltSingle->resetPassphrasePromptLambda();
   } else {
      wltSingle->changeControlPassphrase(setLbd, unlockLbd);
   }
}

////////////////////////////////////////////////////////////////////////////////
void WalletContainer::eraseFromDisk()
{
   auto wltPtr = move(wallet_);
   Wallets::AssetWallet::eraseFromDisk(wltPtr.get());
   wltPtr.reset();
}

////////////////////////////////////////////////////////////////////////////////
void WalletContainer::setComment(const std::string& key, const std::string& val)
{
   auto keyBd = BinaryData::fromString(key);
   wallet_->setComment(keyBd, val);
}

////////////////////////////////////////////////////////////////////////////////
void WalletContainer::setLabels(const std::string& title, const std::string& desc)
{
   wallet_->setLabel(title);
   wallet_->setDescription(desc);
}

////////////////////////////////////////////////////////////////////////////////
void WalletContainer::extendAddressChain(unsigned count,
   const std::function<void(int)>& progFunc)
{
   wallet_->extendPublicChain(accountId_, count, progFunc);
}

////
void WalletContainer::extendAddressChainToIndex(unsigned count)
{
   wallet_->extendPublicChainToIndex(accountId_, count);
}

////////////////////////////////////////////////////////////////////////////////
bool WalletContainer::hasScrAddr(const BinaryData& addr) const
{
   return wallet_->hasScrAddr(addr, accountId_);
}

////////////////////////////////////////////////////////////////////////////////
bool WalletContainer::hasAddress(const std::string& addr) const
{
   return wallet_->hasAddrStr(addr, accountId_);
}

////////////////////////////////////////////////////////////////////////////////
const Wallets::EncryptionKeyId& WalletContainer::getDefaultEncryptionKeyId() const
{
   return wallet_->getDefaultEncryptionKeyId();
}

////////////////////////////////////////////////////////////////////////////////
std::filesystem::path WalletContainer::forkWatchingOnly(
   const Passphrase::SetNew& ctrlPass)
{
   auto wltSingle = std::dynamic_pointer_cast<Wallets::AssetWallet_Single>(wallet_);
   if (wltSingle == nullptr) {
      throw std::runtime_error("unexpected wallet type");
   }
   auto wpd = Wallets::AssetWallet_Single::exportPublicData(wltSingle);
   return Wallets::AssetWallet_Single::forkWatchingOnly(wpd, ctrlPass);
}

////////////////////////////////////////////////////////////////////////////////
std::vector<UTXO> WalletContainer::getUTXOs(uint64_t val, bool zc, bool rbf)
{
   return cache_->getUTXOs(val, zc, rbf,
      [this](const BinaryData& scrAddr)->bool
      { return this->hasScrAddr(scrAddr); }
   );
}

////////
const std::map<BinaryData, TxIOPair> WalletContainer::getTxioMap() const
{
   if (chainDataMain_ == nullptr) {
      return {};
   }

   auto txioMap = chainDataMain_->txioMap;
   for (const auto& txioPair : chainDataZC_->txioMap) {
      auto iter = txioMap.emplace(txioPair);
      if (!iter.second) {
         iter.first->second.merge(txioPair.second);
      }
   }
   return txioMap;
}

void WalletContainer::resolveTxios(uint32_t fromHeight)
{
   auto result = cache_->resolve(
      [this](const BinaryData& scrAddr)->bool
      { return this->hasScrAddr(scrAddr); },
      fromHeight
   );
   chainDataMain_ = std::make_unique<ChainData>(result);
}

void WalletContainer::resolveZcTxios()
{
   auto result = cache_->resolveZC(
      [this](const BinaryData& scrAddr)->bool
      { return this->hasScrAddr(scrAddr); }
   );
   chainDataZC_ = std::make_unique<ChainData>(result);
}
