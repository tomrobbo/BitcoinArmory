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
#include <algorithm>
#include <cstring>

#include "LedgerEntry.h"
#include <Utils/DBUtils.h>
#include <Utils/BtcUtils.h>
#include <BlockchainDatabase/txio.h>
#include <BlockchainDatabase/lmdb_wrapper.h>
#include <ZeroConf/Utils.h>
#include <ZeroConf/Parser.h>
#include "BitcoinP2P.h"

using namespace Armory;

LedgerEntry LedgerEntry::EmptyLedger_;
std::map<BinaryData, LedgerEntry> LedgerEntry::EmptyLedgerMap_;
BinaryData LedgerEntry::EmptyID_ = BinaryData{};

////////////////////////////////////////////////////////////////////////////////
// LedgerEntry
LedgerEntry::LedgerEntry() :
   value_(0), blockNum_(UINT32_MAX),
   txHash_(BtcUtils::EmptyHash),
   index_(UINT32_MAX), txTime_(0),
   isCoinbase_(false), isSentToSelf_(false), isChangeBack_(false),
   isOptInRBF_(false), usesWitness_(false), isChainedZC_(false)
{}

LedgerEntry::LedgerEntry(const std::string& ID,
   int64_t val, uint32_t blkNum, const BinaryData& txHash,
   uint32_t idx, uint32_t txtime,
   bool isCoinbase, bool isToSelf, bool isChange,
   bool isOptInRBF, bool usesWitness, bool isChainedZC) :
   ID_(ID), value_(val), blockNum_(blkNum),
   txHash_(txHash), index_(idx), txTime_(txtime),
   isCoinbase_(isCoinbase), isSentToSelf_(isToSelf), isChangeBack_(isChange),
   isOptInRBF_(isOptInRBF), usesWitness_(usesWitness), isChainedZC_(isChainedZC)
{}

////////////////////////////////////////////////////////////////////////////////
std::string LedgerEntry::getWalletID() const
{
   return ID_;
}

void LedgerEntry::setWalletID(const std::string& str)
{
   ID_ = str;
}

void LedgerEntry::changeBlkNum(uint32_t newHgt)
{
   blockNum_ = newHgt;
}

const std::set<BinaryData>& LedgerEntry::getScrAddrList() const
{
   return scrAddrSet_;
}

////////////////////////////////////////////////////////////////////////////////
bool LedgerEntry::operator<(const LedgerEntry& le2) const
{
   if (blockNum_ != le2.blockNum_) {
      return blockNum_ < le2.blockNum_;
   } else if (index_ != le2.index_) {
      return index_ < le2.index_;
   } else {
      return false;
   }
}

bool LedgerEntry::operator==(const LedgerEntry& le2) const
{
   return (blockNum_ == le2.blockNum_ && index_ == le2.index_);
}

Armory::ScriptPrefix LedgerEntry::getScriptType() const
{
   return (Armory::ScriptPrefix)ID_[0];
}

//////////////////////////////////////////////////////////////////////////////
void LedgerEntry::pprint()
{
   std::cout << "LedgerEntry: " << std::endl;
   std::cout << "   ID      : " << getWalletID() << std::endl;
   std::cout << "   Value   : " << getValue()/1e8 << std::endl;
   std::cout << "   BlkNum  : " << getBlockNum() << std::endl;
   std::cout << "   TxHash  : " <<
      getTxHash().copySwapEndian().toHexStr() << std::endl;
   std::cout << "   TxIndex : " << getIndex() << std::endl;
   std::cout << "   Coinbase: " << (isCoinbase() ? 1 : 0) << std::endl;
   std::cout << "   sentSelf: " << (isSentToSelf() ? 1 : 0) << std::endl;
   std::cout << "   isChange: " << (isChangeBack() ? 1 : 0) << std::endl;
   std::cout << "   isOptInRBF: " << (isOptInRBF() ? 1 : 0) << std::endl;
   for (const auto& addr : scrAddrSet_) {
      std::cout << "   scrAddr: " << addr.toHexStr() << std::endl;
   }
   std::cout << std::endl;
}

void LedgerEntry::pprintOneLine() const
{
   printf("   Addr:%s Tx:%s:%02d   BTC:%0.3f   Blk:%06d\n",
      "   ",
      getTxHash().getSliceCopy(0,8).toHexStr().c_str(),
      getIndex(),
      getValue()/1e8,
      getBlockNum()
   );
}

//////////////////////////////////////////////////////////////////////////////
bool LedgerEntry::operator>(const LedgerEntry& le2) const
{
   if (blockNum_ != le2.blockNum_) {
      return blockNum_ > le2.blockNum_;
   } else if (index_ != le2.index_) {
      return index_ > le2.index_;
   } else {
      return false;
   }
}

//////////////////////////////////////////////////////////////////////////////
void LedgerEntry::purgeLedgerMapFromHeight(
   std::map<BinaryData, LedgerEntry>& leMap, uint32_t purgeFrom)
{
   //Remove all entries starting this height, included.
   BinaryData cutOffHeight(6);
   auto heightPtr = cutOffHeight.getPtr();

   uint8_t* purgeFromPtr = reinterpret_cast<uint8_t*>(&purgeFrom);
   memset(heightPtr, 0, 6);
   heightPtr[0] = purgeFromPtr[2];
   heightPtr[1] = purgeFromPtr[1];
   heightPtr[2] = purgeFromPtr[0];

   auto cutOffIterPair = leMap.equal_range(cutOffHeight);
   leMap.erase(cutOffIterPair.first, leMap.end());
}

//////////////////////////////////////////////////////////////////////////////
void LedgerEntry::purgeLedgerVectorFromHeight(
  std::vector<LedgerEntry>& leVec, uint32_t purgeFrom)
{
   //Remove all entries starting this height, included.
   uint32_t i = 0;
   std::sort(leVec.begin(), leVec.end());
   for (const auto& le : leVec) {
      if (le.getBlockNum() >= purgeFrom) {
         break;
      }
      i++;
   }
   leVec.erase(leVec.begin() + i, leVec.end());
}

//////////////////////////////////////////////////////////////////////////////
std::map<BinaryData, LedgerEntry> LedgerEntry::computeLedgerMap(
   const std::map<BinaryData, TxIOPair>& txioMap,
   uint32_t startBlock, uint32_t endBlock, const std::string& ID,
   const LMDBBlockDatabase* db, const Blockchain* bc,
   const Armory::ZeroConf::ZeroConfContainer* zc)
{
   std::map<BinaryData, LedgerEntry> leMap;

   //arrange txios by transaction
   std::map<BinaryData, std::deque<const TxIOPair*>> txnTxIOMap;

   for (const auto& txio : txioMap) {
      auto txOutDBKey = txio.second.getDBKeyOfOutput().getSliceCopy(0, 6);

      auto& txioVec = txnTxIOMap[txOutDBKey];
      txioVec.emplace_back(&txio.second);
      if (txio.second.hasTxIn()) {
         auto txInDBKey = txio.second.getDBKeyOfInput().getSliceCopy(0, 6);

         auto& _txioVec = txnTxIOMap[txInDBKey];
         _txioVec.emplace_back(&txio.second);
      }
   }

   //convert TxIO to ledgers
   auto ss = zc->getSnapshot();
   for (const auto& txioVec : txnTxIOMap) {
      //reset ledger variables
      BinaryData txHash;

      uint32_t blockNum;
      uint32_t txTime;
      uint16_t txIndex;

      std::set<BinaryData> scrAddrSet;

      bool isRBF = false;
      bool usesWitness = false;
      bool isChained = false;

      //grab iterator
      auto txioIter = txioVec.second.cbegin();

      //get txhash, block, txIndex and txtime
      bool isZc = txioVec.first.startsWith(DBUtils::ZCPrefix);
      if (!isZc) {
         blockNum = DBUtils::hgtxToHeight(txioVec.first.getSliceRef(0, 4));
         txIndex = READ_UINT16_BE(txioVec.first.getSliceRef(4, 2));
         txTime = bc->getHeaderByHeight(blockNum, 0xFF)->getTimestamp();
         txHash = db->getTxHashForLdbKey(txioVec.first);
      } else {
         blockNum = UINT32_MAX;
         txIndex = READ_UINT32_BE(txioVec.first.getSliceRef(2, 4));
         txTime = (*txioIter)->getTxTime();
         if (ss == nullptr) {
            LOGWARN << "zc txio without a snapshot!";
         } else {
            txHash = ss->getHashForKey(txioVec.first);
         }
      }

      if (blockNum < startBlock || blockNum > endBlock) {
         continue;
      }

      bool isCoinbase=false;
      int64_t value=0;
      int64_t valIn=0, valOut=0;
      uint32_t nTxInAreOurs = 0, nTxOutAreOurs = 0;

      while (txioIter != txioVec.second.cend()) {
         const auto& txio = *(*txioIter);
         if (blockNum == UINT32_MAX) {
            if (txio.isRBF()) {
               isRBF = true;
            }
            if (txio.getTxTime() > txTime) {
               txTime = txio.getTxTime();
            }
         }

         if (txio.getDBKeyOfOutput().startsWith(txioVec.first)) {
            isCoinbase |= txio.isFromCoinbase();
            valIn += txio.getValue();
            value += txio.getValue();

            nTxOutAreOurs++;
         }

         if (txio.hasTxIn() &&
            txio.getDBKeyOfInput().startsWith(txioVec.first)) {
            valOut -= txio.getValue();
            value -= txio.getValue();

            nTxInAreOurs++;

            if (txio.isChainedZC()) {
               isChained = true;
            }
         }

         scrAddrSet.insert(txio.getScrAddr());
         ++txioIter;
      }

      bool isSentToSelf = false;
      bool isChangeBack = false;
      std::shared_ptr<const Armory::ZeroConf::ParsedTx> ptx;
      if (nTxInAreOurs * nTxOutAreOurs > 0) {
         //if some of the txins AND some of the txouts are ours, this could be an STS
         //pull the txn and compare the txin and txout counts

         uint32_t nTxOutInTx = UINT32_MAX;
         if (!isZc) {
            nTxOutInTx = db->getStxoCountForTx(txioVec.first.getSliceRef(0, 6));
         } else if (ss!=nullptr) {
            //grab zc by key
            auto ptx = ss->getTxByKey(txioVec.first);
            if (ptx != nullptr) {
               nTxOutInTx = ptx->outputs.size();
            }
         }

         if (nTxOutInTx == nTxOutAreOurs) {
            value = valIn;
            isSentToSelf = true;
         }
      } else if (nTxInAreOurs != 0 && (valIn + valOut) < 0) {
         isChangeBack = true;
      }

      LedgerEntry le(ID,
         value,
         blockNum,
         txHash,
         txIndex,
         txTime,
         isCoinbase,
         isSentToSelf,
         isChangeBack,
         isRBF,
         usesWitness,
         isChained);

      /*
      When signing a tx online, the wallet knows the txhash, therefor it can register all
      comments on outgoing addresses under the txhash.

      When the tx is signed offline, there is no guarantee that the txhash will be known
      when the offline tx is crafted. Therefor the comments for each outgoing address are
      registered under that address only.

      In order for the GUI to be able to resolve outgoing address comments, the ledger entry
      needs to carry those for pay out transactions
      */

      if (value < 0) {
         if (!isZc) {
            try {
               //grab tx by key
               auto payout_tx = db->getFullTxCopy(txioVec.first);

               //get scrAddr for each txout
               for (unsigned i=0; i < payout_tx.getNumTxOut(); i++) {
                  auto txout = payout_tx.getTxOutCopy(i);
                  le.scrAddrSet_.emplace(txout.getScrAddressStr());
               }
            } catch (const std::exception&) {
               LOGWARN << "no tx on record for txio " << txioVec.first.toHexStr();
            }
         } else if (ss!=nullptr) {
            if (ptx == nullptr) {
               //grab zc by key if we haven't got it previously
               auto ptx = ss->getTxByKey(txioVec.first);
            }
            if (ptx == nullptr) {
               LOGWARN << "failed to get zc for ledger parsing";
            } else {
               for (const auto& txout : ptx->outputs) {
                  le.scrAddrSet_.emplace(txout.scrAddr);
               }
            }
         } else {
            LOGWARN << "we have a zc txio but no snapshot =(";
         }
      }

      le.scrAddrSet_ = std::move(scrAddrSet);
      leMap.emplace(txioVec.first, std::move(le));
   }
   return leMap;
}
