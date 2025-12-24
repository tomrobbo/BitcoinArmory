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

#include <cstring>

#include "txio.h"
#include <Utils/BtcUtils.h>
#include <Utils/DBUtils.h>
#include <TxClasses.h>
#include "lmdb_wrapper.h"

using namespace Armory;

namespace
{
   uint16_t getTxIndex(const BinaryData& key)
   {
      auto sliceRef = key.getSliceRef(6, 2);
      uint16_t result = (uint16_t(sliceRef.getPtr()[0]) << 8) +
         sliceRef.getPtr()[1];
      return result;
   }
}

/////////////////////////////////////////////////////////////////////////////
// TxRef methods
TxRef::TxRef() :
   dbKey6B_{}
{}

TxRef::TxRef(BinaryDataRef bdr) :
   dbKey6B_{bdr.getSliceRef(0, 6)}
{}

////////
bool TxRef::isInitialized() const
{
   return !dbKey6B_.empty();
}

////////
bool TxRef::operator==(const BinaryData& dbkey) const
{
   return dbKey6B_ == dbkey;
}

bool TxRef::operator==(const TxRef& txr) const
{
   return dbKey6B_ == txr.dbKey6B_;
}

bool TxRef::operator>=(const BinaryData& dbkey) const
{
   return dbKey6B_ >= dbkey;
}

////////
uint32_t TxRef::getBlockHeight() const
{
   if (dbKey6B_.getSize() == 6 &&
      !dbKey6B_.startsWith(DBUtils::ZCPrefix)) {
      return DBUtils::hgtxToHeight(dbKey6B_.getSliceCopy(0, 4));
   } else {
      return UINT32_MAX;
   }
}

uint8_t TxRef::getDuplicateID() const
{
   if (dbKey6B_.getSize() == 6) {
      return DBUtils::hgtxToDupID(dbKey6B_.getSliceCopy(0, 4));
   } else {
      return UINT8_MAX;
   }
}

uint16_t TxRef::getBlockTxIndex() const
{
   if (dbKey6B_.getSize() == 6) {
      if (!dbKey6B_.startsWith(DBUtils::ZCPrefix)) {
         return READ_UINT16_BE(dbKey6B_.getPtr() + 4);
      } else {
         return READ_UINT32_BE(dbKey6B_.getPtr() + 2);
      }
   } else {
      return UINT16_MAX;
   }
}

////////
const BinaryData& TxRef::getDBKey() const
{
   return dbKey6B_;
}

BinaryDataRef TxRef::getDBKeyRef() const
{
   return dbKey6B_.getRef();
}

BinaryData TxRef::getDBKeyOfChild(uint16_t i) const
{
   return dbKey6B_ + WRITE_UINT16_BE(i);
}

////////
void TxRef::pprint(std::ostream& os, int) const
{
   os << "TxRef Information:" << std::endl;
   //os << "   Hash:      " << getThisHash().toHexStr() << endl;
   os << "   Height:    " << getBlockHeight() << std::endl;
   os << "   BlkIndex:  " << getBlockTxIndex() << std::endl;
   //os << "   FileIdx:   " << blkFilePtr_.getFileIndex() << endl;
   //os << "   FileStart: " << blkFilePtr_.getStartByte() << endl;
   //os << "   NumBytes:  " << blkFilePtr_.getNumBytes() << endl;
   os << "   ----- " << std::endl;
   os << "   Read from disk, full tx-info: " << std::endl;
   //getTxCopy().pprint(os, nIndent+1);
}

/////////////////////////////////////////////////////////////////////////////
// DBTxRef Methods
DBTxRef::DBTxRef(const TxRef& txref, const LMDBBlockDatabase* db)
   : TxRef(txref), db_(db)
{}

////////
BinaryData DBTxRef::serialize() const
{ 
   return db_->getFullTxCopy(dbKey6B_).serialize();
}

Tx DBTxRef::getTxCopy() const
{
   return db_->getFullTxCopy(dbKey6B_);
}

bool DBTxRef::isMainBranch() const
{
   if(dbKey6B_.getSize() != 6) {
      return false;
   } else {
      uint8_t dup8 = db_->getValidDupIDForHeight(getBlockHeight());
      return getDuplicateID() == dup8;
   }
}

BinaryData DBTxRef::getThisHash() const
{
   return db_->getTxHashForLdbKey(dbKey6B_);
}

TxIn  DBTxRef::getTxInCopy(uint32_t i)
{
   return db_->getTxInCopy(dbKey6B_, i);
}

TxOut DBTxRef::getTxOutCopy(uint32_t i)
{
   return db_->getTxOutCopy(dbKey6B_, i);
}

//////////////////////////////////////////////////////////////////////////////
// TxIOPair
TxIOPair::TxIOPair(const BinaryData& txOutKey8B, uint64_t val) :
   amount_(val),
   txRefOfOutput_{txOutKey8B.getSliceRef(0, 6)},
   indexOfOutput_(getTxIndex(txOutKey8B)),
   indexOfInput_(0),
   isTxOutFromSelf_(false),
   isFromCoinbase_(false),
   isMultisig_(false),
   txtime_(0),
   isUTXO_(false)
{}

TxIOPair::TxIOPair(const TxRef& txRef, uint16_t outputId, uint64_t val) :
   amount_(val),
   txRefOfOutput_{txRef},
   indexOfOutput_(outputId),
   indexOfInput_(0),
   isTxOutFromSelf_(false),
   isFromCoinbase_(false),
   isMultisig_(false),
   txtime_(0),
   isUTXO_(false)
{}

////////
bool TxIOPair::hasTxIn() const
{
   return txRefOfInput_.isInitialized();
}

////////
BinaryData TxIOPair::getTxHashOfOutput(const LMDBBlockDatabase *db) const
{
   if (txHashOfOutput_.getSize() == 32) {
      return txHashOfOutput_;
   } else if (db != nullptr) {
      DBTxRef dbTxRef(txRefOfOutput_, db);
      txHashOfOutput_ = dbTxRef.getThisHash();
      return txHashOfOutput_;
   }
   return {};
}

BinaryData TxIOPair::getTxHashOfInput(const LMDBBlockDatabase *db) const
{
   if (!hasTxIn()) {
      return BtcUtils::EmptyHash;
   } else if (txHashOfInput_.getSize() == 32) {
      return txHashOfInput_;
   } else if (db != nullptr) {
      DBTxRef dbTxRef(txRefOfInput_, db);
      txHashOfInput_ = dbTxRef.getThisHash();
      return txHashOfInput_;
   }
   return {};
}

void TxIOPair::setTxHashOfInput(const BinaryData& txHash)
{
   txHashOfInput_ = txHash;
}

void TxIOPair::setTxHashOfOutput(const BinaryData& txHash)
{
   txHashOfOutput_ = txHash;
}

////////
TxOut TxIOPair::getTxOutCopy(LMDBBlockDatabase* db) const
{
   // I actually want this to segfault when there is no TxOut...
   // we should't ever be trying to access it without checking it
   // first in the calling code (hasTxOut/hasTxOutZC)
   DBTxRef dbTxRef(txRefOfOutput_, db);
   return dbTxRef.getTxOutCopy(indexOfOutput_);
}

TxIn TxIOPair::getTxInCopy(LMDBBlockDatabase* db) const
{
   // I actually want this to segfault when there is no TxIn...
   // we should't ever be trying to access it without checking it
   // first in the calling code (hasTxIn/hasTxInZC)
   if (hasTxIn()) {
      DBTxRef dbTxRef(txRefOfInput_, db);
      return dbTxRef.getTxInCopy(indexOfInput_);
   }
   throw std::runtime_error("Has not TxInCopy");
}

////////
uint64_t TxIOPair::getValue() const
{
   return amount_;
}

const TxRef& TxIOPair::getTxRefOfOutput() const
{
   return txRefOfOutput_;
}

const TxRef& TxIOPair::getTxRefOfInput() const
{
   return txRefOfInput_;
}

BinaryData TxIOPair::getDBKeyOfOutput() const
{
   return txRefOfOutput_.getDBKeyOfChild(indexOfOutput_);
}

BinaryData TxIOPair::getDBKeyOfInput() const
{
   return txRefOfInput_.getDBKeyOfChild(indexOfInput_);
}

////////
uint32_t TxIOPair::getIndexOfOutput() const
{
   return indexOfOutput_;
}

uint32_t TxIOPair::getIndexOfInput() const
{
   return indexOfInput_;
}

Outpoint TxIOPair::getOutPoint(LMDBBlockDatabase* db) const
{
   return Outpoint{getTxHashOfOutput(db), indexOfOutput_};
}

////////
bool TxIOPair::setTxIn(const TxRef& txref, uint32_t index)
{
   txRefOfInput_ = txref;
   indexOfInput_ = index;
   return true;
}

bool TxIOPair::setTxIn(const BinaryData& dbKey8B)
{
   if (dbKey8B.getSize() == 8) {
      BinaryRefReader brr(dbKey8B);
      BinaryDataRef txKey6B = brr.get_BinaryDataRef(6);
      uint16_t      txInIdx = brr.get_uint16_t(BE);
      return setTxIn(TxRef{txKey6B}, (uint32_t)txInIdx);
   } else {
      //pass a 0 byte dbkey to reset the txin
      setTxIn({}, 0);
      return false;
   }
}

////////
std::pair<bool, bool> TxIOPair::reassessValidity(LMDBBlockDatabase* db)
{
   std::pair<bool, bool> result;
   result.first = hasTxOutInMain(db);
   result.second = hasTxInInMain(db);
   return result;
}

////////
bool TxIOPair::isTxOutFromSelf() const
{
   return isTxOutFromSelf_;
}

void TxIOPair::setTxOutFromSelf(bool isTrue)
{
   isTxOutFromSelf_ = isTrue;
}

bool TxIOPair::isFromCoinbase() const
{
   return isFromCoinbase_;
}

void TxIOPair::setFromCoinbase(bool isTrue)
{
   isFromCoinbase_ = isTrue;
}

bool TxIOPair::isMultisig() const
{
   return isMultisig_;
}

void TxIOPair::setMultisig(bool isTrue)
{
   isMultisig_ = isTrue;
}

bool TxIOPair::isRBF() const
{
   return isRBF_;
}

void TxIOPair::setRBF(bool isTrue)
{
   isRBF_ = isTrue;
}

void TxIOPair::setChained(bool isTrue)
{
   isZCChained_ = isTrue;
}

bool TxIOPair::isChainedZC() const
{
   return isZCChained_;
}

////////
bool TxIOPair::isSpent(LMDBBlockDatabase* db) const
{
   // Not sure whether we should verify hasTxOut.  It wouldn't make much 
   // sense to have TxIn but not TxOut, but there might be a preferred 
   // behavior in such awkward circumstances
   return hasTxInZC() || hasTxInInMain(db);
}

bool TxIOPair::isUnspent(LMDBBlockDatabase* db) const
{
   return (hasTxOutZC() || hasTxOutInMain(db)) && !isSpent(db);
}

bool TxIOPair::isSpendable(LMDBBlockDatabase* db, uint32_t currBlk) const
{
   // Spendable TxOuts are ones with at least 1 confirmation
   if (hasTxInZC() || hasTxInInMain(db)) {
      return false;
   }

   if (hasTxOutInMain(db)) {
      uint32_t nConf = currBlk - txRefOfOutput_.getBlockHeight() + 1;
      if (isFromCoinbase_ && nConf < COINBASE_MATURITY) {
         return false;
      } else {
         return true;
      }
   }

   if (hasTxOutZC()) {
      return false;
   }
   return false;
}

////////
bool TxIOPair::isMineButUnconfirmed(
   LMDBBlockDatabase* db, uint32_t currBlk, unsigned confTarget) const
{
   DBTxRef dbTxRef(txRefOfInput_, db);
   if (hasTxInZC() || (hasTxIn() && dbTxRef.isMainBranch())) {
      return false;
   }

   if (hasTxOutZC()) {
      return true;
   }

   if (hasTxOutInMain(db)) {
      uint32_t nConf = currBlk - txRefOfOutput_.getBlockHeight() + 1;
      if (isFromCoinbase_) {
         return (nConf<COINBASE_MATURITY);
      } else {
         return (nConf<confTarget);
      }
   }
   return false;
}

////////
void TxIOPair::setTxTime(uint32_t t)
{
   txtime_ = t;
}

uint32_t TxIOPair::getTxTime() const
{
   return txtime_;
}

bool TxIOPair::isUTXO() const
{
   return isUTXO_;
}

void TxIOPair::setUTXO(bool val)
{
   isUTXO_ = val;
}

////////
bool TxIOPair::hasTxOutInMain(LMDBBlockDatabase* db) const
{
   DBTxRef dbTxRef(txRefOfOutput_, db);
   return !hasTxOutZC() && dbTxRef.isMainBranch();
}

bool TxIOPair::hasTxInInMain(LMDBBlockDatabase* db) const
{
   DBTxRef dbTxRef(txRefOfInput_, db);
   return !hasTxInZC() && hasTxIn() && dbTxRef.isMainBranch();
}

bool TxIOPair::hasTxOutZC() const
{
   return txRefOfOutput_.getDBKey().startsWith(DBUtils::ZCPrefix);
}

bool TxIOPair::hasTxInZC() const
{
   return txRefOfInput_.getDBKey().startsWith(DBUtils::ZCPrefix);
}

////////
void TxIOPair::pprintOneLine(LMDBBlockDatabase* db) const
{
   printf("   Val:(%0.3f)\t  (STS, I, Omb,Imb, Oz,Iz)  %d  %d %d%d %d%d\n",
      (double)getValue() / 1e8,
      isTxOutFromSelf() ? 1 : 0,
      hasTxIn() ? 1 : 0,
      hasTxOutInMain(db) ? 1 : 0,
      hasTxInInMain(db) ? 1 : 0,
      hasTxOutZC() ? 1 : 0,
      hasTxInZC() ? 1 : 0
   );
}

////////
bool TxIOPair::operator<(const TxIOPair& t2) const
{
   auto check = std::memcmp(
      txRefOfOutput_.dbKey6B_.getPtr(),
      t2.txRefOfOutput_.dbKey6B_.getPtr(),
      6);
   if (check == 0) {
      return indexOfOutput_ < t2.indexOfOutput_;
   } else {
      return check < 0 ? true : false;
   }
}

bool TxIOPair::operator==(const TxIOPair& t2) const
{
   auto check = std::memcmp(
      txRefOfOutput_.dbKey6B_.getPtr(),
      t2.txRefOfOutput_.dbKey6B_.getPtr(),
      6);
   if (check != 0) {
      return false;
   } else {
      return indexOfOutput_ == t2.indexOfOutput_;
   }
}

bool TxIOPair::operator>=(const BinaryData& dbKey) const
{
   if (txRefOfOutput_ >= dbKey) {
      return true;
   }
   if (txRefOfInput_ >= dbKey) {
      return true;
   }
   return false;
}

void TxIOPair::merge(const TxIOPair& rhs)
{
   setTxIn(rhs.txRefOfInput_, rhs.indexOfInput_);

   txHashOfOutput_   = rhs.txHashOfOutput_;
   txHashOfInput_    = rhs.txHashOfInput_;

   isTxOutFromSelf_  = rhs.isTxOutFromSelf_;
   isFromCoinbase_   = rhs.isFromCoinbase_;
   isMultisig_       = rhs.isMultisig_;
   isRBF_            = rhs.isRBF_;
   isZCChained_      = rhs.isZCChained_;
   isUTXO_           = rhs.isUTXO_;

   txtime_           = rhs.txtime_;
   scrAddr_          = rhs.scrAddr_;
}

////////
void TxIOPair::setScrAddrRef(const BinaryDataRef& bdr)
{
   scrAddr_ = bdr;
}

const BinaryDataRef& TxIOPair::getScrAddr() const
{
   return scrAddr_;
}
