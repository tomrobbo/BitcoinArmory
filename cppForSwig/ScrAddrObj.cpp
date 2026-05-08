////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2011-2015, Armory Technologies, Inc.                        //
//  Distributed under the GNU Affero General Public License (AGPL v3)         //
//  See LICENSE-ATI or http://www.gnu.org/licenses/agpl.html                  //
//                                                                            //
//                                                                            //
//  Copyright (C) 2016-2025, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "ScrAddrObj.h"
#include <Utils/BitcoinSettings.h>
#include <Utils/BtcUtils.h>
#include <Utils/DBUtils.h>
#include <BlockchainDatabase/lmdb_wrapper.h>
#include <BlockchainDatabase/Blockchain.h>
#include <BlockchainDatabase/txio.h>
#include <BlockchainDatabase/BlockObj.h>
#include <BlockchainDatabase/StoredBlockObj.h>
#include <ZeroConf/Utils.h>
#include <ZeroConf/Parser.h>
#include <Ledgers/LedgerEntry.h>
#include <Ledgers/Context.h>
#include "BitcoinP2P.h"

using namespace Armory;

////////////////////////////////////////////////////////////////////////////////
// ScrAddrObj Methods
ScrAddrObj::ScrAddrObj(const Types::ScrAddr& addr,
   Types::ScrAddrId id, LMDBBlockDatabase *db) :
   scrAddr_(addr), id_(id), db_(db)
{}

////////
const Types::ScrAddr& ScrAddrObj::getScrAddr() const
{
   return scrAddr_;
}

////////
std::map<Types::TxIOKey, TxIOPairUint> ScrAddrObj::getTxios(
   Types::BlockId start, Types::BlockId end) const
{
   //TODO: cache keys for unspent txouts so that we
   //      don't have to fetch them every time

   //grab txio range
   auto txOutData = db_->getTxOutHistoryForScrAddrKey(id_, 0, UINT32_MAX);
   auto txInKeys = db_->getTxInHistoryForTxOutHistory(txOutData);

   std::map<Types::TxIOKey, TxIOPairUint> result;
   auto addTxio = [this, &result](
      const std::pair<Types::TxIOKey, TxOutData>& txopair,
      Types::TxIOKey txInKey)
   {
      auto emplaceResult = result.emplace(txopair.first,
         TxIOPairUint{scrAddr_, txopair.first, txopair.second.amount});
      emplaceResult.first->second.setTxIn(txInKey);
   };

   //create txios
   for (const auto& txopair : txOutData) {
      //check txout does not go over end range
      if (txopair.second.blockID > end) {
         continue;
      }

      //if we have a spender, check it vs start and end range
      Types::TxIOKey txInKey = Types::INVALID_TXIO_KEY;
      auto txInIter = txInKeys.find(txopair.first);
      if (txInIter != txInKeys.end()) {
         auto inBlockId = Types::getBlockIDFromTxKey(txInIter->second);
         if (inBlockId >= start && inBlockId <= end) {
            txInKey = txInIter->second;
         }
      }

      //finally, if txout is unspent, check it vs start range
      if (!Types::isTxIOKeyValid(txInKey)) {
         if (txopair.second.blockID < start) {
            continue;
         }
      }

      //if we got this far, this is an eligible txio
      addTxio(txopair, txInKey);
   }
   return result;
}
