////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2026, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "TxIOCache.h"

#include <Utils/DBUtils.h>
#include <Utils/BtcUtils.h>
#include <BlockchainDatabase/txio.h>
#include <Ledgers/Context.h>
#include <TxClasses.h>
#include <AsyncClient.h>
#include "Notifications.h"

using namespace Armory;
using namespace Armory::Bridge;

namespace
{
   const BinaryData firstZCKey = READHEX("FFFF00000000");
}

////////////////////////////////////////////////////////////////////////////////
// TxIOCache
TxIOCache::TxIOCache() :
   dbCache_{std::make_shared<Ledgers::DBCache>()}
{}

std::shared_ptr<const Ledgers::DBCache> TxIOCache::getDBCache() const
{
   return std::const_pointer_cast<const Ledgers::DBCache>(dbCache_);
}

CacheResolveResult TxIOCache::resolve(
   const AddressFilter& filter, uint32_t fromHeight) const
{
   //run through unspent txios first
   CacheResolveResult result{lastKnownBlock_};
   for (const auto& txio : unspentTxios_) {
      const auto& txKey = txio.second.getTxRefOfOutput().getDBKey();

      //skip txios that are not on the main chain
      if (!txKeyIsValid(txKey)) {
         continue;
      }

      //does this txout trigger our filter
      const auto& tx = dbCache_->txMap.at(txKey);
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (filter(scrAddr)) {
         result.addTxio(txio.first, txio.second, scrAddr);
      }
   }

   //check spent txios now
   for (const auto& txio : spentTxios_) {
      //is output on mainchain?
      const auto& txKeyOutput = txio.second.getTxRefOfOutput().getDBKey();
      if (!txKeyIsValid(txKeyOutput)) {
         continue;
      }

      //is input on mainchain?
      const auto& txKeyInput = txio.second.getTxRefOfInput().getDBKey();
      if (!txKeyIsValid(txKeyInput)) {
         continue;
      }

      //do we have an unspent txio in the result map?
      auto txioKey = txio.second.getDBKeyOfOutput();
      auto iter = result.txioMap.find(txioKey);
      if (iter != result.txioMap.end()) {
         //we do, merge in the txin
         iter->second.merge(txio.second);
         continue;
      }

      //we don't, is the txout relevant then?
      const auto& tx = dbCache_->txMap.at(txKeyOutput);
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (filter(scrAddr)) {
         result.addTxio(txioKey, txio.second, scrAddr);
      }
   }
   return result;
}

CacheResolveResult TxIOCache::resolveZC(const AddressFilter& filter) const
{
   CacheResolveResult result{UINT32_MAX};
   for (const auto& txio : zcTxios_) {
      //do we have an unspent txio in the result map?
      auto txioKey = txio.second.getDBKeyOfOutput();
      auto iter = result.txioMap.find(txioKey);
      if (iter != result.txioMap.end()) {
         //we do, merge in the txin
         iter->second.merge(txio.second);
         continue;
      }

      //we don't, is the txout relevant then?
      const auto& txKeyOutput = txio.second.getTxRefOfOutput().getDBKey();
      const auto& tx = dbCache_->txMap.at(txKeyOutput);
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (filter(scrAddr)) {
         result.addTxio(txioKey, txio.second, scrAddr);
      }
   }
   return result;

}


bool TxIOCache::txKeyIsValid(const BinaryData& txKey) const
{
   auto height = DBUtils::hgtxToHeight(txKey.getSliceRef(0, 4));
   auto dupId = DBUtils::hgtxToDupID(txKey.getSliceRef(0, 4));
   return dbCache_->isHeightValid(height, dupId);
}

////////
uint32_t TxIOCache::update(
   std::shared_ptr<AsyncClient::BlockDataViewer> bdvPtr,
   std::shared_ptr<NotifStruct> notif)
{
   if (notif->type == NotifType::ZC) {
      auto zcPtr = std::dynamic_pointer_cast<NotifStruct_ZC>(notif);
      updateZC(bdvPtr, zcPtr->txios);
      return UINT32_MAX;
   }

   NewBlockNotif blockNotif{UINT32_MAX, UINT32_MAX};
   if (notif->type == NotifType::NEWBLOCK) {
      auto blockPtr =
         std::dynamic_pointer_cast<NotifStruct_NewBlock>(notif);
      blockNotif = blockPtr->blockNotif;
   }
   uint32_t fromHeight = 0;
   if (blockNotif.isReorg()) {
      fromHeight = blockNotif.getBranchHeight() + 1;
   } else if (notif->type != NotifType::REFRESH) {
      fromHeight = lastKnownBlock_ + 1;
   }

   //1. txios
   auto promTxios = std::make_shared<std::promise<std::vector<TxIOPair>>>();
   auto futTxios = promTxios->get_future();

   bdvPtr->getTxios(fromHeight, [prom = promTxios]
      (ReturnMessage<std::vector<TxIOPair>> result) {
         prom->set_value(result.get());
   });
   auto txios = std::move(futTxios.get());

   auto fetchedHeight = notif->type == NotifType::REFRESH ?
      UINT32_MAX : blockNotif.getHeight();
   auto missingStuff = addTxios(txios, fetchedHeight);
   auto missingTxKeys = std::move(missingStuff.first);
   auto missingHeights = std::move(missingStuff.second);
   if (blockNotif.isReorg()) {
      missingHeights.clear();
      for (unsigned i = blockNotif.getBranchHeight() + 1;
         i <= blockNotif.getHeight(); i++) {
         missingHeights.emplace(i);
      }
   }

   //2. missing txs
   auto promTxs = std::make_shared<std::promise<std::vector<Tx>>>();
   auto futTxs = promTxs->get_future();
   bdvPtr->getTxsByKey(missingTxKeys, [prom = promTxs]
      (ReturnMessage<std::vector<Tx>> result) {
         prom->set_value(result.get());
   });

   //3. missing blocks
   auto promBlocks = std::make_shared<
      std::promise<std::vector<DBClientClasses::BlockHeader>>>();
   auto futBlocks = promBlocks->get_future();
   AsyncClient::Blockchain bc{*bdvPtr};
   bc.getHeadersByHeight(missingHeights, [prom = promBlocks]
      (ReturnMessage<std::vector<DBClientClasses::BlockHeader>> result) {
         prom->set_value(result.get());
      }
   );

   //4. commit it all to the cache
   auto txs = std::move(futTxs.get());
   auto blocks = std::move(futBlocks.get());
   ReentrantLock lock(this);

   /* we're using this opportunity to detect mined ZCs */

   //list ZC tx hashes, we'll check them again hash of grabbed txs
   std::map<BinaryData, TxIOKey> zcHashes;
   {
      auto txIter = dbCache_->txMap.lower_bound(firstZCKey);
      while (txIter != dbCache_->txMap.end()) {
         zcHashes.emplace(txIter->second.getThisHash(), txIter->first);
         ++txIter;
      }
   }

   std::set<BinaryData> minedKeys;
   for (auto& tx : txs) {
      auto zcIter = zcHashes.find(tx.getThisHash());
      if (zcIter != zcHashes.end()) {
         //we have a zc key for this tx hash, purge it
         dbCache_->minedZcKeys_.emplace(zcIter->second);
         dbCache_->txMap.erase(zcIter->second);
         minedKeys.emplace(zcIter->second);
      }

      //add tx to db cache
      dbCache_->txMap.emplace(tx.getDBKey(), std::move(tx));
   }

   //purge mined zc from map
   auto zcIter = zcTxios_.begin();
   while (zcIter != zcTxios_.end()) {
      auto& zcTxio = zcIter->second;
      if (zcTxio.hasTxOutZC() &&
         minedKeys.find(zcTxio.getTxRefOfOutput().getDBKey()) !=
         minedKeys.end()) {
         zcTxios_.erase(zcIter++);
         continue;
      }

      if (zcTxio.hasTxIn() &&
         minedKeys.find(zcTxio.getTxRefOfInput().getDBKey()) !=
         minedKeys.end()) {
         zcTxios_.erase(zcIter++);
         continue;
      }

      //zc is still valid, move on to next one
      ++zcIter;
   }

   dbCache_->addBlocks(blocks);
   return fromHeight;
}

void TxIOCache::updateZC(
   std::shared_ptr<AsyncClient::BlockDataViewer> bdvPtr,
   const std::vector<TxIOPair>& zcTxios)
{
   std::set<BinaryData> missingTxKeys;
   for (const auto& zcTxio : zcTxios) {
      auto zcOutKey = zcTxio.getTxRefOfOutput().getDBKey();
      if (dbCache_->isZcMined(zcOutKey)) {
         continue;
      }
      if (dbCache_->txMap.find(zcOutKey) == dbCache_->txMap.end()) {
         missingTxKeys.emplace(zcOutKey);
      }

      if (zcTxio.hasTxInZC()) {
         auto zcInKey = zcTxio.getTxRefOfInput().getDBKey();
         if (dbCache_->txMap.find(zcInKey) == dbCache_->txMap.end()) {
            missingTxKeys.emplace(zcInKey);
         }
      }

      auto iter = zcTxios_.emplace(zcTxio.getDBKeyOfOutput(), zcTxio);
      if (!iter.second) {
         iter.first->second.merge(zcTxio);
      }
   }

   auto promTxs = std::make_shared<std::promise<std::vector<Tx>>>();
   auto futTxs = promTxs->get_future();
   bdvPtr->getTxsByKey(missingTxKeys, [prom = promTxs]
      (ReturnMessage<std::vector<Tx>> result) {
         prom->set_value(result.get());
   });
   auto txs = std::move(futTxs.get());

   ReentrantLock lock(this);
   for (auto& tx : txs) {
      dbCache_->txMap.emplace(tx.getDBKey(), std::move(tx));
   }
}

////////
std::pair<std::set<BinaryData>, std::set<uint32_t>> TxIOCache::addTxios(
   std::vector<TxIOPair>& txios, uint32_t fetchedHeight)
{
   std::set<BinaryData> missingTxKeys;
   std::set<uint32_t> missingHeights;

   auto addKey = [&missingTxKeys, &missingHeights, this](const BinaryData& key)
   {
      auto height = DBUtils::hgtxToHeight(key.getSliceRef(0, 4));
      if (dbCache_->blocks.find(height) == dbCache_->blocks.end()) {
         missingHeights.emplace(height);
      }
      if (dbCache_->txMap.find(key) == dbCache_->txMap.end()) {
         missingTxKeys.emplace(key);
      }
   };

   ReentrantLock lock(this);
   for (auto& txio : txios) {
      addKey(txio.getTxRefOfOutput().getDBKey());

      if (txio.hasTxIn()) {
         addKey(txio.getTxRefOfInput().getDBKey());
         spentTxios_.emplace(txio.getDBKeyOfInput(), std::move(txio));
      } else {
         unspentTxios_.emplace(txio.getDBKeyOfOutput(), std::move(txio));
      }
   }

   if (fetchedHeight != UINT32_MAX) {
      lastKnownBlock_ = fetchedHeight;
   }
   return {std::move(missingTxKeys), std::move(missingHeights)};
}

////////
std::map<BinaryData, std::set<BinaryData>> TxIOCache::getAddressBook(
   const AddressFilter& filter) const
{
   std::map<BinaryData, std::set<BinaryData>> result;
   for (const auto& txio : unspentTxios_) {
      const auto& txKey = txio.second.getTxRefOfOutput().getDBKey();

      //skip txios that are not on the main chain
      if (!txKeyIsValid(txKey)) {
         continue;
      }

      //track this <addr, hash>
      const auto& tx = dbCache_->txMap.at(txKey);
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (filter(scrAddr)) {
         auto iter = result.find(scrAddr);
         if (iter == result.end()) {
            iter = result.emplace(scrAddr, std::set<BinaryData>{}).first;
         }
         iter->second.emplace(tx.getThisHash());
      }
   }

   for (const auto& txio : spentTxios_) {
      //is output on mainchain?
      const auto& txKeyOutput = txio.second.getTxRefOfOutput().getDBKey();
      if (!txKeyIsValid(txKeyOutput)) {
         continue;
      }

      //is input on mainchain?
      const auto& txKeyInput = txio.second.getTxRefOfInput().getDBKey();
      if (!txKeyIsValid(txKeyInput)) {
         continue;
      }

      //track this <addr, hash>
      const auto& tx = dbCache_->txMap.at(txKeyOutput);
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (filter(scrAddr)) {
         auto iter = result.find(scrAddr);
         if (iter == result.end()) {
            iter = result.emplace(scrAddr, std::set<BinaryData>{}).first;
         }
         iter->second.emplace(tx.getThisHash());
      }
   }
   return result;
}

std::vector<UTXO> TxIOCache::getZcUTXOs(bool rbf,
   const AddressFilter& filter) const
{
   std::vector<UTXO> result;
   auto addTxio = [&result, filter](const TxIOPair& txio, const Tx& tx)
   {
      auto scrAddr = tx.getScrAddrForTxOut(txio.getIndexOfOutput());
      if (!filter(scrAddr)) {
         return;
      }

      auto txOut = tx.getTxOutCopy(txio.getIndexOfOutput());
      result.emplace_back(UTXO{txio.getValue(),
         tx.getTxHeight(), tx.getTxIndex(), txio.getIndexOfOutput(),
         tx.getThisHash(), txOut.getScript()
      });
   };

   for (const auto& txio : zcTxios_) {
      const auto& txKey = txio.second.getTxRefOfOutput().getDBKey();

      if (rbf) {
         //for rbf outputs, we consider all outputs that are RBF flagged
         if (txio.second.isRBF()) {
            const auto& tx = dbCache_->txMap.at(txKey);
            addTxio(txio.second, tx);
         }
         continue;
      }

      if (txio.second.hasTxIn()) {
         //for zc utxos, we ignore spent ones
         continue;
      }

      if (!txio.second.hasTxOutZC()) {
         //and the output has to be zc as well
         continue;
      }

      const auto& tx = dbCache_->txMap.at(txKey);
      addTxio(txio.second, tx);
   }
   return result;
}

std::vector<UTXO> TxIOCache::getUTXOs(
   uint64_t value, bool zc, bool rbf,
   const AddressFilter& filter) const
{
   if (zc || rbf) {
      return getZcUTXOs(rbf, filter);
   }

   uint64_t total = 0;
   std::set<BinaryData> spentKeys;
   for (const auto& txio : spentTxios_) {
      auto outputKey = txio.second.getDBKeyOfOutput();
      if (!txKeyIsValid(outputKey)) {
         continue;
      }
      spentKeys.emplace(outputKey);
   }

   std::vector<UTXO> result;
   result.reserve(unspentTxios_.size());
   for (const auto& txio : unspentTxios_) {
      auto iter = spentKeys.find(txio.first);
      if (iter != spentKeys.end()) {
         continue;
      }

      const auto& txKey = txio.second.getTxRefOfOutput().getDBKey();
      if (!txKeyIsValid(txKey)) {
         continue;
      }
      const auto& tx = dbCache_->txMap.at(txKey);
      if (tx.getTxIndex() == 0 &&
         tx.getTxHeight() + COINBASE_MATURITY > lastKnownBlock_) {
         continue;
      }
      auto scrAddr = tx.getScrAddrForTxOut(txio.second.getIndexOfOutput());
      if (!filter(scrAddr)) {
         continue;
      }

      auto txOut = tx.getTxOutCopy(txio.second.getIndexOfOutput());
      result.emplace_back(UTXO{txio.second.getValue(),
         tx.getTxHeight(), tx.getTxIndex(), txio.second.getIndexOfOutput(),
         tx.getThisHash(), txOut.getScript()
      });
      total += txio.second.getValue();
      if (total >= value) {
         break;
      }
   }
   return result;
}

/////////
std::map<BinaryData, TxIOPair> TxIOCache::filterTxios(
   const std::vector<TxIOPair>& txios, const AddressFilter& filter) const
{
   ReentrantLock lock(this);
   std::map<BinaryData, TxIOPair> result;
   for (const auto& txio : txios) {
      const auto& txOutKey = txio.getTxRefOfOutput().getDBKey();
      const auto& outTx = dbCache_->txMap.at(txOutKey);
      auto scrAddrOut = outTx.getScrAddrForTxOut(txio.getIndexOfOutput());
      if (filter(scrAddrOut)) {
         result.emplace(txio.getDBKeyOfOutput(), txio);
      }

      if (!txio.hasTxIn()) {
         continue;
      }

      const auto& txInKey = txio.getTxRefOfInput().getDBKey();
      const auto& inTx = dbCache_->txMap.at(txInKey);
      auto scrAddrIn = inTx.getScrAddrForTxOut(txio.getIndexOfOutput());
      if (filter(scrAddrIn)) {
         result.emplace(txio.getDBKeyOfOutput(), txio);
      }
   }
   return result;
}


////////////////////////////////////////////////////////////////////////////////
// CacheResolveResult
void CacheResolveResult::addTxio(const BinaryData& key, const TxIOPair& txio,
   const BinaryData& addr)
{
   auto iter = txioMap.emplace(key, txio).first;
   auto addrIter = addrTxioMap.find(addr);
   if (addrIter == addrTxioMap.end()) {
      addrIter = addrTxioMap.emplace(addr, std::vector<TxIOPair*>{}).first;
   }
   addrIter->second.emplace_back(&iter->second);
}

////////////////////////////////////////////////////////////////////////////////
// ChainData
ChainData::ChainData(CacheResolveResult& data) :
   txioMap(std::move(data.txioMap))
{
   for (const auto& addr : data.addrTxioMap) {
      //tally address balance and count
      int64_t total = 0;
      int64_t spendable = 0;
      int64_t unconfirmed = 0;
      int64_t count = 0;
      for (const auto& txio : addr.second) {
         //+1 txio per output
         int64_t val = static_cast<int64_t>(txio->getValue());
         ++count;
         if (txio->hasTxIn()) {
            //+1 txio per input
            //spent txios do not affect balance
            ++count;
            if (txio->hasTxInZC()) {
               total -= val;
               spendable -= val;
               unconfirmed -= val;
            }
            continue;
         }

         //total tallies all unspent outputs indiscriminately
         total += val;

         //spendable only tracks mature outputs (cf mining reward maturity)
         if (txio->isSpendable(data.topBlock)) {
            spendable += val;
         }

         //unconfirmed adds up immature and unconfirmed outputs
         if (txio->isUnconfirmed(data.topBlock, MIN_CONFIRMATIONS)) {
            unconfirmed += val;
         }
      }

      //set address data
      balanceMap.emplace(addr.first,
         std::vector<int64_t>{total, spendable, unconfirmed});
      countMap.emplace(addr.first, count);

      //update wallet aggregate
      totalBalance        += total;
      spendableBalance    += spendable;
      unconfirmedBalance  += unconfirmed;
      txioCount           += count;
   }
}
