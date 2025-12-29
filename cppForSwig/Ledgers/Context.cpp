////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2025, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "Context.h"
#include <Utils/BinaryData.h>
#include <Utils/DBUtils.h>
#include <BlockchainDatabase/BlockObj.h>
#include <BlockchainDatabase/Blockchain.h>
#include <BlockchainDatabase/txio.h>
#include <BlockchainDatabase/lmdb_wrapper.h>
#include <ZeroConf/Utils.h>

using namespace Armory;
using namespace Armory::Ledgers;

////////////////////////////////////////////////////////////////////////////////
// Context
Context::Context(std::map<uint32_t, uint32_t>& timestamps,
   std::map<BinaryData, Tx>& txMap,
   std::shared_ptr<const ZeroConf::MempoolSnapshot> ss) :
   timestamps_(std::move(timestamps)), txMap_{std::move(txMap)}, ss_{ss}
{}

uint32_t Context::getTimestampForBlockHeight(uint32_t blockNum) const
{
   try {
      return timestamps_.at(blockNum);
   } catch (const std::out_of_range&) {
      LOGWARN << "no timestamp for block " << blockNum;
      return UINT32_MAX;
   }
}

const BinaryData& Context::getTxHash(BinaryDataRef key) const
{
   const auto& tx = getTx(key);
   return tx.getThisHash();
}

size_t Context::getTxOutCount(BinaryDataRef key) const
{
   const auto& tx = getTx(key);
   return tx.getNumTxOut();
}

const Tx& Context::getTx(BinaryDataRef key) const
{
   auto iter = txMap_.find(key);
   if (iter != txMap_.end()) {
      return iter->second;
   }
   auto ptx = ss_->getTxByKey(key);
   auto result = txMap_.emplace(key, ptx->getTxObj());
   return result.first->second;
}

////////////////////////////////////////////////////////////////////////////////
// namespace functions
Context Ledgers::prepareContext(
   const std::map<BinaryData, TxIOPair>& txioMap,
   const Blockchain& bc,
   LMDBBlockDatabase* db,
   std::shared_ptr<const ZeroConf::MempoolSnapshot> zcSs)
{
   std::map<uint32_t, uint32_t> timestamps;
   std::map<BinaryData, Tx> txMap;
   for (const auto& txioPair : txioMap) {
      /* output */
      const auto& txKeyOut = txioPair.second.getTxRefOfOutput().getDBKey();
      if (txKeyOut.startsWith(DBUtils::ZCPrefix)) {
         continue;
      }

      //block timestamp
      auto blockNum = DBUtils::hgtxToHeight(txKeyOut.getSliceRef(0, 4));
      if (timestamps.find(blockNum) == timestamps.end()) {
         try {
            auto headerPtr = bc.getHeaderByHeight(blockNum, 0xFF);
            timestamps.emplace(blockNum, headerPtr->getTimestamp());
         } catch (const std::range_error&) {
            LOGWARN << "no block for height " << blockNum;
            continue;
         }
      }

      //tx
      txMap.emplace(txKeyOut, db->getFullTxCopy(txKeyOut));

      /* input */
      if (!txioPair.second.hasTxIn()) {
         continue;
      }
      const auto& txKeyIn = txioPair.second.getTxRefOfInput().getDBKey();
      if (txKeyIn.startsWith(DBUtils::ZCPrefix)) {
         continue;
      }

      //block timestamp
      blockNum = DBUtils::hgtxToHeight(txKeyIn.getSliceRef(0, 4));
      if (timestamps.find(blockNum) == timestamps.end()) {
         try {
            auto headerPtr = bc.getHeaderByHeight(blockNum, 0xFF);
            timestamps.emplace(blockNum, headerPtr->getTimestamp());
         } catch (const std::range_error&) {
            LOGWARN << "no block for height " << blockNum;
            continue;
         }
      }

      //tx
      txMap.emplace(txKeyIn, db->getFullTxCopy(txKeyIn));
   }
   return Context{timestamps, txMap, zcSs};
}
