////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2011-2015, Armory Technologies, Inc.                        //
//  Distributed under the GNU Affero General Public License (AGPL v3)         //
//  See LICENSE-ATI or http://www.gnu.org/licenses/agpl.html                  //
//                                                                            //
//                                                                            //
//  Copyright (C) 2016-2026, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////
#include <algorithm>
#include <cstring>

#include "BlockDataViewer.h"
#include <BlockchainDatabase/BlockUtils.h>
#include <BlockchainDatabase/lmdb_wrapper.h>
#include <BlockchainDatabase/txio.h>
#include <Utils/DBUtils.h>
#include <Utils/ArmoryConfig.h>
#include <ZeroConf/Parser.h>
#include <ZeroConf/Utils.h>
#include <ZeroConf/Notifications.h>
#include <Ledgers/LedgerEntry.h>
#include "BtcWallet.h"

using namespace Armory;

/////////////////////////////////////////////////////////////////////////////
// BlockDataViewer
BlockDataViewer::BlockDataViewer(std::shared_ptr<BlockDataManager> bdm) :
   bdm_(bdm), rescanZC_(false), zeroConfCont_(bdm->zeroConfCont()),
   saf_{bdm->getScrAddrFilter()},
   wallets_{this, saf_}, lockboxes_{this, saf_}
{
   db_ = bdm->getIFace();
   bc_ = bdm->blockchain();
   flagRescanZC(false);
}

BlockDataViewer::~BlockDataViewer()
{
   wallets_.reset();
   lockboxes_.reset();
}

void BlockDataViewer::reset()
{
   wallets_.reset();
   lockboxes_.reset();
   rescanZC_   = false;
   lastScanned_ = 0;
}

////////
std::shared_ptr<BlockDataManager> BlockDataViewer::bdm() const
{
   return bdm_;
}

bool BlockDataViewer::isBDMRunning() const
{
   if (bdm_ == nullptr) {
      return false;
   }
   return bdm_->isRunning();
}

void BlockDataViewer::blockUntilBDMisReady() const
{
   if (bdm_ == nullptr) {
      throw std::runtime_error("no bdmPtr_");
   }
   bdm_->blockUntilReady();
}

LMDBBlockDatabase* BlockDataViewer::getDB() const
{
   return db_;
}

const Blockchain& BlockDataViewer::blockchain() const
{
   return *bc_;
}

ZeroConf::ZeroConfContainer* BlockDataViewer::zcContainer() const
{
   return zeroConfCont_.get();
}

std::shared_ptr<ScrAddrFilter> BlockDataViewer::getSAF() const
{
   return saf_;
}

////////
bool BlockDataViewer::isZcEnabled() const
{
   if (bdm_ == nullptr) {
      return false;
   }
   return bdm_->isZcEnabled();
}

void BlockDataViewer::flagRescanZC(bool flag)
{
   rescanZC_.store(flag, std::memory_order_release);
}

bool BlockDataViewer::getZCflag() const
{
   return rescanZC_.load(std::memory_order_acquire);
}

////////
bool BlockDataViewer::hasWallet(const std::string& ID) const
{
   return wallets_.hasID(ID);
}

void BlockDataViewer::registerAWallet(WalletRegistrationRequest& request,
   const std::function<void(bool)>& callback)
{
   switch (request.type)
   {
      case WalletRegType::WALLET:
         wallets_.registerAddresses(request, callback);
         break;

      case WalletRegType::LOCKBOX:
         lockboxes_.registerAddresses(request, callback);
         break;

      default:
         LOGWARN << "invalid wallet registration group!";
   }
}

void BlockDataViewer::unregisterWallet(const std::string& walletID)
{
   wallets_.unregisterWallet(walletID);
   lockboxes_.unregisterWallet(walletID);
}

////////
bool BlockDataViewer::scrAddressIsRegistered(const BinaryData& scrAddr) const
{
   auto scrAddrMap = saf_->getScanFilterAddrMap();
   auto saIter = scrAddrMap->find(scrAddr);
   if (saIter == scrAddrMap->end()) {
      return false;
   }
   return true;
}


bool BlockDataViewer::hasScrAddress(const Types::ScrAddr& scrAddr) const
{
   {
      ReadWriteLock::WriteLock wl(wallets_.lock);
      for (const auto& wlt : wallets_.wallets) {
         if (wlt.second->hasScrAddress(scrAddr)) {
            return true;
         }
      }
   }

   ReadWriteLock::WriteLock wl(lockboxes_.lock);
   for (const auto& wlt : lockboxes_.wallets) {
      if (wlt.second->hasScrAddress(scrAddr)) {
         return true;
      }
   }
   return false;
}

std::set<Types::ScrAddr> BlockDataViewer::getAddrSet() const
{
   std::set<Types::ScrAddr> addrSet;
   {
      ReadWriteLock::WriteLock wl(wallets_.lock);
      for (const auto& wlt : wallets_.wallets) {
         auto wltAddresses = wlt.second->getAddrSet();
         addrSet.insert(wltAddresses.begin(), wltAddresses.end());
      }
   }

   ReadWriteLock::WriteLock wl(lockboxes_.lock);
   for (const auto& wlt : lockboxes_.wallets) {
      auto wltAddresses = wlt.second->getAddrSet();
      addrSet.insert(wltAddresses.begin(), wltAddresses.end());
   }

   return addrSet;
}

////////
std::shared_ptr<BtcWallet> BlockDataViewer::getWalletOrLockbox(
   const std::string& wltID) const
{
   std::shared_ptr<BtcWallet> wlt;
   {
      ReadWriteLock::WriteLock wl(wallets_.lock);
      auto iter = wallets_.wallets.find(wltID);
      if (iter != wallets_.wallets.end()) {
         wlt = iter->second;
      }
   }

   if (wlt == nullptr) {
      ReadWriteLock::WriteLock wl(lockboxes_.lock);
      auto iter = lockboxes_.wallets.find(wltID);
      if (iter != lockboxes_.wallets.end()) {
         wlt = iter->second;
      }
   }

   if (wlt == nullptr) {
      throw std::runtime_error("Unregistered wallet ID");
   }
   return wlt;
}

////////
std::map<Types::TxIOKey, TxIOPairUint> BlockDataViewer::getTxioForRange(
   uint32_t fromHeight) const
{
   //convert height to blockId
   try {
      auto header = bc_->getHeaderByHeight(fromHeight);
      return wallets_.getTxioForRange(header->getUniqueID(), UINT32_MAX);
   } catch (const std::range_error&) {
      return {};
   }
}

////////////////////////////////////////////////////////////////////////////////
// ReadWriteLock
void ReadWriteLock::lockRead()
{
   std::unique_lock<std::mutex> rl(all_lock);
   std::thread::id this_thread_id = std::this_thread::get_id();
   auto idIter = thread_ids_.find(this_thread_id);

   if (idIter != thread_ids_.end()) {
      idIter->second++;
   }

   if (idIter == thread_ids_.end()) {
      while (has_writer) {
         no_writers.wait(rl);
      }
      thread_ids_.emplace(this_thread_id, 1);
   }

   num_readers++;
}

void ReadWriteLock::unlockRead()
{
   std::unique_lock<std::mutex> rl(all_lock);
   std::thread::id this_thread_id = std::this_thread::get_id();
   auto idIter = thread_ids_.find(this_thread_id);

   if (idIter == thread_ids_.end()) {
      throw std::runtime_error("unregistered thread attempted to release a lock");
   }

   idIter->second--;
   if (idIter->second == 0) {
      thread_ids_.erase(idIter);
   }
   num_readers--;
   if (num_readers == 0) {
      no_readers.notify_all();
   }
}

void ReadWriteLock::lockWrite()
{
   std::unique_lock<std::mutex> rl(all_lock);
   std::thread::id this_thread_id = std::this_thread::get_id();

   auto idIter = thread_ids_.find(this_thread_id);
   if (idIter != thread_ids_.end()) {
      throw std::runtime_error("ReadWriteLock deadlock: requested write lock"
         "within a thread already holding a read lock");
   }

   has_writer = true;
   while (num_readers > 0) {
      no_readers.wait(rl);
   }

   rl.release();
}

void ReadWriteLock::unlockWrite()
{
   has_writer = false;
   no_writers.notify_all();
   all_lock.unlock();
}

////////
ReadWriteLock::ReadLock::ReadLock(ReadWriteLock& rwl) :
   l(&rwl)
{
   l->lockRead();
}

ReadWriteLock::ReadLock::~ReadLock()
{
   if (locked) {
      l->unlockRead();
   }
}

void ReadWriteLock::ReadLock::unlock()
{
   locked=false;
   l->unlockRead();
}

////////
ReadWriteLock::WriteLock::WriteLock(ReadWriteLock &rwl) :
   l(&rwl)
{
   l->lockWrite();
}

ReadWriteLock::WriteLock::~WriteLock()
{
   if (locked) {
      l->unlockWrite();
   }
}

void ReadWriteLock::WriteLock::unlock()
{
   locked=false;
   l->unlockRead();
}

////////////////////////////////////////////////////////////////////////////////
// WalletGroup
WalletGroup::WalletGroup(BlockDataViewer* bdvPtr,
   std::shared_ptr<ScrAddrFilter> saf) :
   bdvPtr(bdvPtr), saf(saf)
{}

WalletGroup::~WalletGroup()
{
   for (auto& wlt : wallets) {
      wlt.second->unregister();
   }
}

////////
std::shared_ptr<BtcWallet> WalletGroup::getOrSetWallet(const std::string& id)
{
   ReadWriteLock::WriteLock wl(lock);
   std::shared_ptr<BtcWallet> theWallet;

   auto wltIter = wallets.find(id);
   if (wltIter != wallets.end()) {
      theWallet = wltIter->second;
   } else {
      auto insertResult = wallets.emplace(id,
         std::make_shared<BtcWallet>(bdvPtr, id));
      theWallet = insertResult.first->second;
   }
   return theWallet;
}

////////////////////////////////////////////////////////////////////////////////
bool WalletGroup::unregisterWallet(const std::string& id)
{
   ReadWriteLock::WriteLock wl(lock);
   auto wltIter = wallets.find(id);
   if (wltIter == wallets.end()) {
      return false;
   }

   wallets.erase(wltIter);
   return true;
}

////////////////////////////////////////////////////////////////////////////////
void WalletGroup::registerAddresses(WalletRegistrationRequest& request,
   const std::function<void(bool)>& callback)
{
   if (request.walletId.empty()) {
      callback(false);
      return;
   }

   auto theWallet = getOrSetWallet(request.walletId);
   if (theWallet == nullptr) {
      LOGWARN << "failed to get or set wallet";
      callback(false);
      return;
   }

   //strip collisions from set of addresses to register
   auto addrMap = theWallet->scrAddrMap_.get();
   std::vector<Types::ScrAddr> scrAddrVec;
   scrAddrVec.reserve(request.addresses.size());
   for (auto& addr : request.addresses) {
      if (addr.empty()) {
         continue;
      }
      if (addrMap->find(addr) != addrMap->end()) {
         continue;
      }
      scrAddrVec.emplace_back(std::move(addr));
   }

   auto registrationCompleteCB = [theWallet, callback]
   (std::vector<std::shared_ptr<AddrAndHash>> addrVec, bool success)
   {
      if (!success) {
         callback(false);
         return;
      }

      auto bdvPtr = theWallet->bdvPtr_;
      auto dbPtr = theWallet->bdvPtr_->getDB();
      std::map<Types::ScrAddr, std::shared_ptr<ScrAddrObj>> saMap;
      for (const auto& addr : addrVec) {
         auto scrAddrPtr = std::make_shared<ScrAddrObj>(
            addr->scrAddr, addr->id, dbPtr);
         saMap.emplace(addr->scrAddr, scrAddrPtr);
      }

      if (!saMap.empty()) {
         theWallet->scrAddrMap_.update(saMap);
      }
      theWallet->setRegistered();
      callback(true);
   };

   auto batch = std::make_shared<RegistrationBatch>(
      std::vector<std::string>{request.walletId}, std::move(scrAddrVec),
      request.isNew, registrationCompleteCB);
   saf->pushAddressBatch(batch);
   theWallet->resetCounters();
}

////////////////////////////////////////////////////////////////////////////////
bool WalletGroup::hasID(const std::string& ID) const
{
   ReadWriteLock::ReadLock rl(lock);
   return wallets.find(ID) != wallets.end();
}

/////////////////////////////////////////////////////////////////////////////
void WalletGroup::reset()
{
   ReadWriteLock::ReadLock rl(lock);
   for (const auto& wltPair : wallets) {
      wltPair.second->reset();
   }
}

////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<BtcWallet> WalletGroup::getWalletByID(
   const std::string& ID) const
{
   auto iter = wallets.find(ID);
   if (iter != wallets.end()) {
      return iter->second;
   }
   return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
uint32_t WalletGroup::getBlockInVicinity(uint32_t blk) const
{
   //expect history has been computed, it will throw otherwise
   return hist.getBlockInVicinity(blk);
}

uint32_t WalletGroup::getPageIdForBlockHeight(uint32_t blk) const
{
   //same as above
   return hist.getPageIdForBlockHeight(blk);
}

////////
std::map<Types::TxIOKey, TxIOPairUint> WalletGroup::getTxioForRange(
   uint32_t from, uint32_t to) const
{
   std::map<Types::TxIOKey, TxIOPairUint> result;
   ReadWriteLock::ReadLock rl(lock);
   for (const auto& wlt : wallets) {
      auto txioRange = wlt.second->getTxioForRange(from, to);
      result.insert(txioRange.begin(), txioRange.end());
   }
   return result;
}

std::map<Types::TxIOKey, std::shared_ptr<const TxIOPairUint>>
BlockDataViewer::getZcTxios() const
{
   auto snapshot = zcContainer()->getSnapshot();
   if (snapshot == nullptr) {
      return {};
   }

   std::map<Types::TxIOKey, std::shared_ptr<const TxIOPairUint>> result;
   auto addrSet = getAddrSet();

   for (const auto& addr : addrSet) {
      auto txioMap = snapshot->getTxioMapForScrAddr(addr);
      if (txioMap.empty()) {
         continue;
      }
      result.insert(txioMap.begin(), txioMap.end());
   }
   return result;
}
