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
#include <BlockchainDatabase/BlockchainData.h>
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

const std::shared_ptr<BlockHeader> BlockDataViewer::getTopBlockHeader() const
{
   return bc_->top();
}

uint32_t BlockDataViewer::getTopBlockHeight() const
{
   return bc_->top()->getBlockHeight();
}

ZeroConf::ZeroConfContainer* BlockDataViewer::zcContainer() const
{
   return zeroConfCont_.get();
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
void BlockDataViewer::scanWallets(std::shared_ptr<BDV_Notification> action)
{
   uint32_t startBlock = UINT32_MAX;
   uint32_t endBlock = UINT32_MAX;
   uint32_t prevTopBlock = UINT32_MAX;

   bool reorg = false;
   bool refresh = false;

   ScanWalletStruct scanData;

   switch (action->actionType())
   {
      case BDV_Init:
      {
         prevTopBlock = startBlock = 0;
         endBlock = blockchain().top()->getBlockHeight();
         refresh = true;
         break;
      }

      case BDV_NewBlock:
      {
         auto reorgNotif =
            std::dynamic_pointer_cast<BDV_Notification_NewBlock>(action);
         const auto& reorgState = reorgNotif->reorgState;
         if (!reorgState.hasNewTop) {
            return;
         }

         if (!reorgState.prevTopStillValid) {
            //reorg
            reorg = true;
            startBlock = reorgState.reorgBranchPoint->getBlockHeight();
         } else {
            startBlock = reorgState.prevTop->getBlockHeight();
         }
         endBlock = reorgState.newTop->getBlockHeight();

         //set invalidated keys
         if (reorgNotif->zcPurgePacket != nullptr) {
            scanData.saStruct_.invalidatedZcKeys_ =
               &reorgNotif->zcPurgePacket->invalidatedZcKeys;

            //carry zc state
            scanData.saStruct_.zcState_ = reorgNotif->zcPurgePacket->ssPtr;
            scanData.saStruct_.scrAddrToTxioKeys_ =
               reorgNotif->zcPurgePacket->scrAddrToTxioKeys;
         }

         prevTopBlock = reorgState.prevTop->getBlockHeight() + 1;
         break;
      }

      case BDV_ZC:
      {
         auto zcAction = std::dynamic_pointer_cast<BDV_Notification_ZC>(action);
         scanData.saStruct_.scrAddrToTxioKeys_ =
            std::move(zcAction->packet->scrAddrToTxioKeys);

         scanData.saStruct_.zcState_ = zcAction->packet->ssPtr;
         scanData.saStruct_.newKeysAndScrAddr_ =
            zcAction->packet->newKeysAndScrAddr;

         if (zcAction->packet->purgePacket != nullptr) {
            scanData.saStruct_.invalidatedZcKeys_ =
               &zcAction->packet->purgePacket->invalidatedZcKeys;
         }

         prevTopBlock = startBlock = endBlock =
            blockchain().top()->getBlockHeight();
         break;
      }

      case BDV_Refresh:
      {
         auto refreshNotif =
            std::dynamic_pointer_cast<BDV_Notification_Refresh>(action);

         if (refreshNotif->refresh == BDV_refreshSkipRescan) {
            //only flagged the wallet to send a refresh notification, do not
            //perform any other operations
            ++updateID_;
            return;
         }

         scanData.saStruct_.scrAddrToTxioKeys_ =
            std::move(refreshNotif->zcPacket->scrAddrToTxioKeys);
         scanData.saStruct_.zcState_ = refreshNotif->zcPacket->ssPtr;
         refresh = true;
         break;
      }

      default:
         return;
   }

   scanData.prevTopBlockHeight_ = prevTopBlock;
   scanData.endBlock_ = endBlock;
   scanData.action_ = action->actionType();
   scanData.reorg_ = reorg;

   auto walletsStartBlock = startBlock;
   if (wallets_.pageHistory(refresh, false)) {
      walletsStartBlock = wallets_.hist.getPageBottom(0);
   }
   auto lockboxesStartBlock = startBlock;
   if (lockboxes_.pageHistory(refresh, false)) {
      lockboxesStartBlock = lockboxes_.hist.getPageBottom(0);
   }

   ++updateID_;
   scanData.startBlock_ = walletsStartBlock;
   wallets_.scanWallets(scanData, updateID_);
   scanData.startBlock_ = lockboxesStartBlock;
   lockboxes_.scanWallets(scanData, updateID_);
   lastScanned_ = endBlock;
}

////////
Tx BlockDataViewer::getTxByHash(BinaryDataRef txhash) const
{
   auto key = db_->getDBKeyForHash(txhash);
   if (key != UINT64_MAX) {
      return bdm_->blockchainData()->getTx(key);
   } else {
      return zeroConfCont_->getTxByHash(txhash);
   }
}

Tx BlockDataViewer::getTxByKey(Types::TxKey dbKey) const
{
   return bdm_->blockchainData()->getTx(dbKey);
}

////////
TxOut BlockDataViewer::getPrevTxOut(const TxIn& txin) const
{
   if (txin.isCoinbase()) {
      throw std::runtime_error("txin is coinbase");
   }

   Outpoint op = txin.getOutPoint();
   Tx theTx = getTxByHash(op.getTxHash());
   uint32_t idx = op.getTxOutIndex();
   return theTx.getTxOutCopy(idx);
}

Tx BlockDataViewer::getPrevTx(const TxIn& txin) const
{
   if (txin.isCoinbase()) {
      throw std::runtime_error("txin is coinbase");
   }
   Outpoint op = txin.getOutPoint();
   return getTxByHash(op.getTxHash());
}

BinaryData BlockDataViewer::getSenderScrAddr(const TxIn& txin) const
{
   if (txin.isCoinbase()) {
      return {};
   }
   return getPrevTxOut(txin).getScrAddress();
}

int64_t BlockDataViewer::getSentValue(const TxIn& txin) const
{
   if (txin.isCoinbase()) {
      return -1;
   }
   return getPrevTxOut(txin).getAmount();
}

////////
size_t BlockDataViewer::getWalletsPageCount() const
{
   return wallets_.getPageCount();
}

std::vector<Ledgers::Entry> BlockDataViewer::getWalletsHistoryPage(
   uint32_t pageId, bool rebuildLedger, bool remapWallets)
{
   return wallets_.getHistoryPage(
      pageId, updateID_, rebuildLedger, remapWallets);
}

size_t BlockDataViewer::getLockboxesPageCount(void) const
{
   return lockboxes_.getPageCount();
}

std::vector<Ledgers::Entry> BlockDataViewer::getLockboxesHistoryPage(
   uint32_t pageId, bool rebuildLedger, bool remapWallets)
{
   return lockboxes_.getHistoryPage(
      pageId, updateID_, rebuildLedger, remapWallets);
}

////////
StoredHeader BlockDataViewer::getBlockFromDB(
   uint32_t height) const
{
   auto header = bc_->getHeaderByHeight(height);
   StoredHeader sbh;
   db_->getStoredHeader(sbh, header, true);
   return sbh;
}

////////////////////////////////////////////////////////////////////////////////
bool BlockDataViewer::scrAddressIsRegistered(const BinaryData& scrAddr) const
{
   auto scrAddrMap = saf_->getScanFilterAddrMap();
   auto saIter = scrAddrMap->find(scrAddr);
   if (saIter == scrAddrMap->end()) {
      return false;
   }
   return true;
}

////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<BlockHeader> BlockDataViewer::getHeaderByHash(
   const BinaryData& blockHash) const
{
   return bc_->getHeaderByHash(blockHash);
}

////////
Ledgers::Delegate BlockDataViewer::getLedgerDelegateForWallets()
{
   auto getHist = [this](uint32_t pageID)->std::vector<Ledgers::Entry>
   { return this->getWalletsHistoryPage(pageID, false, false); };

   auto getBlock = [this](uint32_t block)->uint32_t
   { return this->wallets_.getBlockInVicinity(block); };

   auto getPageId = [this](uint32_t block)->uint32_t
   { return this->wallets_.getPageIdForBlockHeight(block); };

   auto getPageCount = [this](void)->uint32_t
   { return this->getWalletsPageCount(); };

   return Ledgers::Delegate(getHist, getBlock, getPageId, getPageCount);
}

Ledgers::Delegate BlockDataViewer::getLedgerDelegateForLockboxes()
{
   auto getHist = [this](uint32_t pageID)->std::vector<Ledgers::Entry>
   { return this->getLockboxesHistoryPage(pageID, false, false); };

   auto getBlock = [this](uint32_t block)->uint32_t
   { return this->lockboxes_.getBlockInVicinity(block); };

   auto getPageId = [this](uint32_t block)->uint32_t
   { return this->lockboxes_.getPageIdForBlockHeight(block); };

   auto getPageCount = [this](void)->uint32_t
   { return this->getLockboxesPageCount(); };

   return Ledgers::Delegate(getHist, getBlock, getPageId, getPageCount);
}

////////////////////////////////////////////////////////////////////////////////
Ledgers::Delegate BlockDataViewer::getLedgerDelegateForWallet(
   const std::string& wltID)
{
   auto wlt = getWalletOrLockbox(wltID);

   auto getHist = [wlt](uint32_t pageID)->std::vector<Ledgers::Entry>
   { return wlt->getHistoryPageAsVector(pageID); };

   auto getBlock = [wlt](uint32_t block)->uint32_t
   { return wlt->historyPager().getBlockInVicinity(block); };

   auto getPageId = [wlt](uint32_t block)->uint32_t
   { return wlt->historyPager().getPageIdForBlockHeight(block); };

   auto getPageCount = [wlt](void)->uint32_t
   { return wlt->historyPager().getPageCount(); };

   return Ledgers::Delegate(getHist, getBlock, getPageId, getPageCount);
}

////////////////////////////////////////////////////////////////////////////////
Ledgers::Delegate BlockDataViewer::getLedgerDelegateForScrAddr(
   const std::string& wltID, const BinaryData& scrAddr)
{
   auto wlt = getWalletOrLockbox(wltID);
   ScrAddrObj& sca = wlt->getScrAddrObjRef(scrAddr);

   auto getHist = [&](uint32_t pageID)->std::vector<Ledgers::Entry>
   { return sca.getHistoryPageById(pageID); };

   auto getBlock = [&](uint32_t block)->uint32_t
   { return sca.getBlockInVicinity(block); };

   auto getPageId = [&](uint32_t block)->uint32_t
   { return sca.getPageIdForBlockHeight(block); };

   auto getPageCount = [&](void)->uint32_t
   { return sca.getPageCount(); };

   return Ledgers::Delegate(getHist, getBlock, getPageId, getPageCount);
}

////////////////////////////////////////////////////////////////////////////////
uint32_t BlockDataViewer::getClosestBlockHeightForTime(uint32_t timestamp)
{
   //get timestamp of genesis block
   auto genBlock = blockchain().getHeaderByHash(
      Config::BitcoinSettings::getGenesisBlockHash());

   //sanity check
   if (timestamp < genBlock->getTimestamp()) {
      return 0;
   }

   //get time diff and divide by average time per block (600 sec for Bitcoin)
   uint32_t diff = timestamp - genBlock->getTimestamp();
   int32_t blockHint = diff/600;

   //look for a block in the hint vicinity with a timestamp lower than ours
   while (blockHint > 0) {
      auto block = blockchain().getHeaderByHeight(blockHint);
      if (block->getTimestamp() < timestamp) {
         break;
      }
      blockHint -= 1000;
   }

   //another sanity check
   if (blockHint < 0) {
      return 0;
   }

   for (uint32_t id = blockHint;
      id < blockchain().top()->getBlockHeight() - 1;
      id++) {
      //not looking for a really precise block, 
      //anything within the an hour of the timestamp is enough
      auto block = blockchain().getHeaderByHeight(id);
      if (block->getTimestamp() + 3600 > timestamp) {
         return block->getBlockHeight();
      }
   }
   return blockchain().top()->getBlockHeight() - 1;
}

////////////////////////////////////////////////////////////////////////////////
Tx BlockDataViewer::getSpenderTxForTxOut(uint32_t height, uint32_t txindex,
   uint16_t txoutid) const
{
   throw std::runtime_error("[BlockDataViewer::getSpenderTxForTxOut] deprecated");
   #if 0
   StoredTxOut stxo;
   db_->getStoredTxOut(stxo, height, txindex, txoutid);

   if (!stxo.isSpent()) {
      throw std::runtime_error("output is not spent!");
   }
   return getTxByKey(stxo.spentByTxInKey.getSliceRef(0, 6));
   #endif
}

////////////////////////////////////////////////////////////////////////////////
bool BlockDataViewer::isRBF(const BinaryData& txHash) const
{
   try {
      auto zctx = zeroConfCont_->getTxByHash(txHash);
      return zctx.isRBF();
   } catch (const std::exception&) {
      return false;
   }
}

////////////////////////////////////////////////////////////////////////////////
bool BlockDataViewer::hasScrAddress(const BinaryDataRef& scrAddr) const
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

////////////////////////////////////////////////////////////////////////////////
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

////////////////////////////////////////////////////////////////////////////////
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

///////////////////////////////////////////////////////////////////////////////
std::tuple<uint64_t, uint64_t> BlockDataViewer::getAddrFullBalance(
   const BinaryData& scrAddr)
{
   StoredScriptHistory ssh;
   db_->getStoredScriptHistorySummary(ssh, scrAddr);
   return std::make_tuple(ssh.totalUnspent, ssh.totalTxioCount);
}

////////////////////////////////////////////////////////////////////////////////
std::vector<std::pair<StoredTxOut, BinaryDataRef>>
BlockDataViewer::getOutputsForOutpoints(
   const std::map<BinaryDataRef, std::set<unsigned>>& outpoints, bool withZc) const
{
   throw std::runtime_error("[BlockDataViewer::getOutputsForOutpoints] deprecated");
   #if 0
   std::vector<std::pair<StoredTxOut, BinaryDataRef>> result;
   auto zcSS = !withZc ? nullptr : zeroConfCont_->getSnapshot();

   auto stxo_tx = db_->beginTransaction(DB_SELECT::STXO, LMDB::Mode::ReadOnly);
   for (auto& opSet : outpoints) {
      //get dbkey for this txhash
      auto dbkey = db_->getDBKeyForHash(opSet.first);
      if (dbkey != UINT64_MAX) {
         for (auto& op : opSet.second) {
            //set txout index
            std::pair<StoredTxOut, BinaryDataRef> stxoPair;
            stxoPair.second = opSet.first;
            auto& stxo = stxoPair.first;
            stxo.txOutIndex = op;

            auto stxoKey = dbkey;
            stxoKey.append(WRITE_UINT16_BE(op));
            if (!db_->getStoredTxOut(stxo, stxoKey)) {
               throw std::runtime_error("invalid outpoint");
            }
            if (stxo.isSpent()) {
               stxo.spenderHash = db_->getTxHashForLdbKey(
                  stxo.spentByTxInKey, nullptr);
            }
            result.emplace_back(std::move(stxoPair));
         }
         continue;
      }

      if (!withZc || zcSS == nullptr) {
         continue;
      }

      BinaryData zcKey;
      try {
         zcKey = zcSS->getKeyForHash(opSet.first);
      } catch (const std::range_error&) {
         continue;
      }

      auto txFromSS = zcSS->getTxByKey(zcKey);
      if (txFromSS == nullptr) {
         continue;
      }

      for (auto& op : opSet.second) {
         //set txout index
         std::pair<StoredTxOut, BinaryDataRef> stxoPair;
         stxoPair.second = opSet.first;

         auto& stxo = stxoPair.first;
         stxo.txOutIndex = op;
         if (txFromSS->outputs.size() <= op) {
            throw std::runtime_error("invalid outpoint");
         }

         const auto& output = txFromSS->outputs[op];
         const auto& theTx = txFromSS->getTxObj();
         BinaryRefReader brr{theTx.getPtr(), theTx.getSize()};
         brr.advance(output.offset);
         auto txOutRef = brr.get_BinaryDataRef(output.len);

         stxo.unserialize(txOutRef);
         stxo.blockHeight = UINT32_MAX;
         stxo.txIndex = UINT16_MAX;

         //check spentness
         BinaryWriter bwKey(8);
         bwKey.put_BinaryData(zcKey);
         bwKey.put_uint16_t(op, BE);
         auto txioKey = bwKey.getDataRef();

         if (zcSS->isTxOutSpentByZC(txioKey)) {
            //this zc output is spent, get the txio
            auto zcTxio = zcSS->getTxioByKey(txioKey);
            if (!zcTxio->hasTxInZC()) {
               throw std::runtime_error("this zc txio should have a txin");
            }

            //get hash for the txin key, this is our spender
            auto txInRef = zcTxio->getTxRefOfInput();
            stxoPair.first.spenderHash =
               zcSS->getHashForKey(txInRef.getDBKeyRef());
         }
         result.emplace_back(stxoPair);
      }
   }
   return result;
   #endif
}

////////
CombinedBalances BlockDataViewer::getCombinedBalances() const
{
   CombinedBalances result;
   auto compileResult = [&result, height=getTopBlockHeight()]
   (const std::map<std::string, std::shared_ptr<BtcWallet>>& wltMap)
   {
      for (const auto& wlt : wltMap) {
         std::map<BinaryData, CombinedBalances::BalanceAndCount> bnc;
         auto txnCounts = wlt.second->getAddrTxnCounts(-1);
         auto addrBalances = wlt.second->getAddrBalances(-1, height);

         uint32_t count = 0;
         for (const auto& txnCount : txnCounts) {
            count += txnCount.second;
            auto iter = addrBalances.find(txnCount.first);
            if (iter != addrBalances.end()) {
               bnc.emplace(txnCount.first, CombinedBalances::BalanceAndCount{
                  std::get<0>(iter->second),
                  std::get<1>(iter->second),
                  std::get<2>(iter->second),
                  txnCount.second});
            } else {
               bnc.emplace(txnCount.first, CombinedBalances::BalanceAndCount{
                  0, 0, 0, txnCount.second});
            }
         }

         auto full = wlt.second->getFullBalance();
         auto spendable = wlt.second->getSpendableBalance(height);
         auto unconfirmed = wlt.second->getUnconfirmedBalance(height);
         CombinedBalances::BalanceAndCount wltBnc{
            full, spendable, unconfirmed, count
         };

         result.wallets.emplace(wlt.first,
            CombinedBalances::Wallet{wltBnc, bnc});
      }
   };

   compileResult(wallets_.wallets);
   compileResult(lockboxes_.wallets);
   return result;
}

////////
bool BlockDataViewer::isTxOutSpentByZC(const Types::TxIOKey& dbKey) const
{
   return zeroConfCont_->isTxOutSpentByZC(dbKey);
}

std::map<Types::TxKey, std::shared_ptr<const TxIOPair>>
BlockDataViewer::getRBFTxIOsforScrAddr(
   const Types::ScrAddr& scrAddr) const
{
   throw std::runtime_error("[BlockDataViewer::getRBFTxIOsforScrAddr] deprecated");
   #if 0
   return zeroConfCont_->getRBFTxIOsforScrAddr(scrAddr);
   #endif
}

std::vector<TxOut> BlockDataViewer::getZcTxOutsForKeys(
   const std::set<Types::TxIOKey>& keys) const
{
   return zeroConfCont_->getZcTxOutsForKey(keys);
}

std::vector<UTXO> BlockDataViewer::getZcUTXOsForKeys(
   const std::set<Types::TxIOKey>& keys) const
{
   return zeroConfCont_->getZcUTXOsForKey(keys);
}

std::shared_ptr<ScrAddrFilter> BlockDataViewer::getSAF() const
{
   return saf_;
}

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

size_t WalletGroup::getPageCount() const
{
   return hist.getPageCount();
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
std::map<uint32_t, uint32_t> WalletGroup::computeWalletsSSHSummary(
   bool forcePaging, bool pageAnyway)
{
   ReadWriteLock::ReadLock rl(lock);
   std::map<uint32_t, uint32_t> fullSummary;

   bool isAlreadyPaged = true;
   for (auto& wltPair : wallets) {
      if (forcePaging) {
         wltPair.second->mapPages();
      }

      if (wltPair.second->isPaged()) {
         isAlreadyPaged = false;
      } else {
         wltPair.second->mapPages();
      }
   }

   if (isAlreadyPaged) {
      if (!forcePaging && !pageAnyway) {
         throw AlreadyPagedException();
      }
   }

   for (const auto& wltPair : wallets) {
      if (wltPair.second->uiFilter_ == false) {
         continue;
      }
      const auto& wltSummary = wltPair.second->getSSHSummary();
      for (const auto& summary : wltSummary) {
         fullSummary[summary.first] += summary.second;
      }
   }
   return fullSummary;
}

////////////////////////////////////////////////////////////////////////////////
bool WalletGroup::pageHistory(bool forcePaging, bool pageAnyway)
{
   return hist.mapHistory([this, forcePaging, pageAnyway]
      (void)->std::map<uint32_t, uint32_t>
      { return computeWalletsSSHSummary(forcePaging, pageAnyway); }
   );
}

////////////////////////////////////////////////////////////////////////////////
std::vector<Ledgers::Entry> WalletGroup::getHistoryPage(
   uint32_t pageId, unsigned updateID,
   bool rebuildLedger, bool remapWallets)
{
   throw std::runtime_error("[WalletGroup::getHistoryPage] deprecated");
   #if 0
   if (pageId >= hist.getPageCount()) {
      throw std::range_error("pageId out of range");
   }

   if (order == order_ascending) {
      pageId = hist.getPageCount() - pageId - 1;
   }

   if (rebuildLedger || remapWallets) {
      pageHistory(remapWallets, false);
   }

   if (rebuildLedger || remapWallets) {
      updateID = UINT32_MAX;
   }

   std::vector<Ledgers::Entry> vle;
   {
      ReadWriteLock::ReadLock rl(lock);
      std::map<std::string, std::shared_ptr<BtcWallet>> localWalletMap;

      for (auto& wlt_pair : wallets) {
         if (!wlt_pair.second->uiFilter_) {
            continue;
         }
         localWalletMap.emplace(wlt_pair);
      }

      auto getTxio = [&localWalletMap](
         uint32_t, uint32_t)->std::map<BinaryData, TxIOPair>
      { return {}; };

      auto buildLedgers = [&localWalletMap](
         const std::map<BinaryData, TxIOPair>&,
         uint32_t startBlock, uint32_t endBlock)
      ->std::map<BinaryData, Ledgers::Entry>
      {
         std::map<BinaryData, Ledgers::Entry> result;
         unsigned i = 0;
         for (auto& wlt_pair : localWalletMap) {
            auto txio_map = wlt_pair.second->getTxioForRange(
               startBlock, endBlock);
            auto ledgerMap = wlt_pair.second->updateWalletLedgersFromTxio(
               txio_map, startBlock, endBlock);

            for (auto& ledger : ledgerMap) {
               BinaryWriter bw;
               bw.put_uint32_t(i++);
               result.emplace(bw.getData(), std::move(ledger.second));
            }
         }
         return result;
      };

      auto leMap = hist.getPageLedgerMap(
         getTxio, buildLedgers, pageId, updateID, nullptr);

      if (leMap != nullptr) {
         for (auto& le : *leMap) {
            vle.emplace_back(le.second);
         }
      }
   }

   if (order == order_ascending) {
      std::sort(vle.begin(), vle.end());
   } else {
      std::sort(vle.begin(), vle.end(), Ledgers::DescendingOrder{});
   }
   return vle;
   #endif
}

////////////////////////////////////////////////////////////////////////////////
void WalletGroup::updateLedgerFilter(const std::vector<std::string>& walletsList)
{
   ReadWriteLock::ReadLock rl(lock);

   std::vector<std::string> enabledIDs;
   for (auto& wlt_pair : wallets) {
      if (wlt_pair.second->uiFilter_) {
         enabledIDs.push_back(wlt_pair.first);
      }
      wlt_pair.second->uiFilter_ = false;
   }


   for (auto walletID : walletsList) {
      auto iter = wallets.find(walletID);
      if (iter == wallets.end()) {
         continue;
      }
      iter->second->uiFilter_ = true;
   }

   auto vec_copy = walletsList;
   sort(vec_copy.begin(), vec_copy.end());
   sort(enabledIDs.begin(), enabledIDs.end());

   if (vec_copy == enabledIDs) {
      return;
   }
   pageHistory(false, true);
}

////////////////////////////////////////////////////////////////////////////////
void WalletGroup::scanWallets(ScanWalletStruct& scanData, int32_t updateID)
{
   ReadWriteLock::ReadLock rl(lock);
   for (auto& wlt : wallets) {
      wlt.second->scanWallet(scanData, updateID);
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
