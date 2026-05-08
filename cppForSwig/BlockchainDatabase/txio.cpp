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

uint16_t TxRef::getTxIndex() const
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

//////////////////////////////////////////////////////////////////////////////
// TxIOPair
TxIOPair::TxIOPair(const BinaryData& txOutKey8B, uint64_t val) :
   amount_(val),
   txRefOfOutput_{txOutKey8B.getSliceRef(0, 6)},
   indexOfOutput_(getTxIndex(txOutKey8B)),
   indexOfInput_(0)
{}

TxIOPair::TxIOPair(const TxRef& txRef, uint16_t outputId, uint64_t val) :
   amount_(val),
   txRefOfOutput_{txRef},
   indexOfOutput_(outputId),
   indexOfInput_(0)
{}

////////
bool TxIOPair::hasTxIn() const
{
   return txRefOfInput_.isInitialized();
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
bool TxIOPair::isSpendable(uint32_t currBlk) const
{
   // spendable TxOuts are ones with at least 1 confirmation
   if (hasTxIn() || hasTxOutZC()) {
      return false;
   }

   uint32_t nConf = currBlk - txRefOfOutput_.getBlockHeight() + 1;
   if (isFromCoinbase_ && nConf < COINBASE_MATURITY) {
      return false;
   } else {
      return true;
   }
}

bool TxIOPair::isUnconfirmed(uint32_t currBlk, unsigned confTarget) const
{
   if (hasTxOutZC()) {
      return true;
   }

   uint32_t nConf = currBlk - txRefOfOutput_.getBlockHeight() + 1;
   if (isFromCoinbase_) {
      return nConf < COINBASE_MATURITY;
   } else {
      return nConf < confTarget;
   }
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
bool TxIOPair::operator<(const TxIOPair& t2) const
{
   auto check = std::memcmp(
      txRefOfOutput_.getDBKey().getPtr(),
      t2.txRefOfOutput_.getDBKey().getPtr(),
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
      txRefOfOutput_.getDBKey().getPtr(),
      t2.txRefOfOutput_.getDBKey().getPtr(),
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

void TxIOPair::merge(const TxIOPair& rhs)
{
   setTxIn(rhs.txRefOfInput_, rhs.indexOfInput_);

   isTxOutFromSelf_  = rhs.isTxOutFromSelf_;
   isFromCoinbase_   = rhs.isFromCoinbase_;
   isMultisig_       = rhs.isMultisig_;
   isRBF_            = rhs.isRBF_;
   isZCChained_      = rhs.isZCChained_;
   isUTXO_           = rhs.isUTXO_;
   txtime_           = rhs.txtime_;
}

////////
void TxIOPair::pprint() const
{
   std::cout << "  TxOut: " << getDBKeyOfOutput().toHexStr() << std::endl;
   if (hasTxIn()) {
      std::cout << "  TxIn: " << getDBKeyOfInput().toHexStr() << std::endl;
   }
   std::cout << "  amount: " << amount_ << std::endl;
}

////////////////////////////////////////////////////////////////////////////////
// TxIOPairUint
TxIOPairUint::TxIOPairUint(Types::TxIOKey txOutKey, uint64_t amount,
   const Types::ScrAddr& scrAddr) :
   txIOKeyOfOutput_(txOutKey), amount_{amount}, scrAddr_{scrAddr}
{}

TxIOPairUint::TxIOPairUint(Types::TxIOKey txOutKey, uint64_t amount,
   const Types::ScrAddr& scrAddr, Types::TxIOKey txInKey) :
   txIOKeyOfOutput_(txOutKey), amount_{amount}, scrAddr_{scrAddr},
   txIOKeyOfInput_(txInKey)
{}

////////
Types::Amount TxIOPairUint::getAmount() const
{
   return amount_;
}

uint32_t TxIOPairUint::getTxTime() const
{
   return txTime_;
}

const Types::ScrAddr& TxIOPairUint::getScrAddr() const
{
   return scrAddr_;
}

////////
bool TxIOPairUint::hasTxIn() const
{
   return Types::isTxKeyValid(txIOKeyOfInput_);
}

bool TxIOPairUint::hasTxOutZC() const
{
   return Types::isThisAZCKey(txIOKeyOfOutput_);
}

bool TxIOPairUint::hasTxInZC() const
{
   if (!hasTxIn()) {
      return false;
   }
   return Types::isThisAZCKey(txIOKeyOfInput_);
}

////////
Types::TxKey TxIOPairUint::getTxKeyOfOutput() const
{
   return Types::getTxKeyFromTxIOKey(txIOKeyOfOutput_);
}

Types::TxKey TxIOPairUint::getTxKeyOfInput() const
{
   if (!hasTxIn()) {
      return Types::INVALID_TX_KEY;
   }
   return Types::getTxKeyFromTxIOKey(txIOKeyOfInput_);
}

////////
Types::TxIOKey TxIOPairUint::getTxIOKeyOfOutput() const
{
   return txIOKeyOfOutput_;
}

Types::TxIOKey TxIOPairUint::getTxIOKeyOfInput() const
{
   return txIOKeyOfInput_;
}

Types::TxIOId TxIOPairUint::getIndexOfOutput() const
{
   return Types::getTxIOIndexFromTxIOKey(txIOKeyOfOutput_);
}

Types::TxIOId TxIOPairUint::getIndexOfInput() const
{
   if (!hasTxIn()) {
      return UINT16_MAX;
   }
   return Types::getTxIOIndexFromTxIOKey(txIOKeyOfInput_);
}

////////
void TxIOPairUint::setTxIn(
   Types::TxKey keyOfInput, Types::TxId indexOfInput)
{
   txIOKeyOfInput_ = Types::constructTxIOKeyFromTxKey(
      keyOfInput, indexOfInput);
}

void TxIOPairUint::setTxIn(Types::TxIOKey txInKey)
{
   txIOKeyOfInput_ = txInKey;
}

////////
void TxIOPairUint::setTxTime(uint32_t txtime)
{
   txTime_ = txtime;
}

void TxIOPairUint::setRBF(bool rbf)
{
   isRBF_ = rbf;
}

void TxIOPairUint::setChained(bool chained)
{
   isZCChained_ = chained;
}

bool TxIOPairUint::isRBF() const
{
   return isRBF_;
}

bool TxIOPairUint::isChained() const
{
   return isZCChained_;
}

////////
void TxIOPairUint::merge(const TxIOPairUint& rhs)
{
   setTxIn(rhs.txIOKeyOfInput_);

   isRBF_         = rhs.isRBF_;
   isZCChained_   = rhs.isZCChained_;
   txTime_        = rhs.txTime_;
}
