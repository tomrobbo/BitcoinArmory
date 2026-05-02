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

#pragma once

#include <Utils/Types.h>
#include <Utils/BinaryData.h>

////////////////////////////////////////////////////////////////////////////////
class TxRef
{
private:
   BinaryData dbKey6B_;

public:
   TxRef(void);
   TxRef(BinaryDataRef);

   bool isInitialized(void) const;
   const BinaryData& getDBKey(void) const;
   BinaryDataRef getDBKeyRef(void) const;
   BinaryData getDBKeyOfChild(uint16_t) const;
   uint16_t getTxIndex(void) const;
   uint32_t getBlockHeight(void) const;
   uint8_t getDuplicateID(void) const;

   bool operator==(const BinaryData&) const;
   bool operator==(const TxRef&) const;
   bool operator>=(const BinaryData&) const;
};

////////////////////////////////////////////////////////////////////////////////
class TxIOPair
{
public:
   explicit TxIOPair(const BinaryData&, uint64_t);
   explicit TxIOPair(const TxRef&, uint16_t, uint64_t);

   // Lots of accessors
   bool      hasTxIn(void) const;
   bool      hasTxOutZC(void) const;
   bool      hasTxInZC(void) const;
   uint64_t  getValue(void) const;

   ////
   const TxRef& getTxRefOfOutput(void) const;
   const TxRef& getTxRefOfInput(void) const;
   BinaryData getDBKeyOfOutput(void) const;
   BinaryData getDBKeyOfInput(void) const;

   uint32_t getIndexOfOutput(void) const;
   uint32_t getIndexOfInput(void) const;

   bool isTxOutFromSelf(void) const;
   void setTxOutFromSelf(bool = true);
   bool isFromCoinbase(void) const;
   void setFromCoinbase(bool = true);
   bool isMultisig(void) const;
   void setMultisig(bool = true);
   bool isRBF(void) const;
   void setRBF(bool);
   void setChained(bool);
   bool isChainedZC(void) const;

   ////
   bool setTxIn(const TxRef&, uint32_t);
   bool setTxIn(const BinaryData&);
   void merge(const TxIOPair&);

   ////
   bool isSpendable(uint32_t) const;
   bool isUnconfirmed(uint32_t, unsigned) const;

   bool operator<(const TxIOPair&) const;
   bool operator==(const TxIOPair&) const;
   bool operator>=(const BinaryData&) const;

   void setTxTime(uint32_t);
   uint32_t getTxTime(void) const;

   bool isUTXO(void) const;
   void setUTXO(bool);
   void pprint(void) const;

public:
   bool flagged = false;

private:
   const uint64_t amount_;
   const TxRef txRefOfOutput_;
   const uint32_t indexOfOutput_;

   TxRef txRefOfInput_;
   uint32_t indexOfInput_;

   bool isTxOutFromSelf_ = false;
   bool isFromCoinbase_ = false;
   bool isMultisig_ = false;
   bool isRBF_ = false;
   bool isZCChained_ = false;
   uint32_t txtime_ = 0;

   /***marks txio as spent for serialize/deserialize operations. It signifies
   whether a subSSH entry with only a TxOut DBkey is spent.

   To allow for partial parsing of ssh history, all txouts need to be visible at
   the height they appeared, and spent txouts need to be visible at the
   spending txin's height as well.

   While spent txouts at txin height are unique, spent txouts at txout height
   need to be differenciated from UTXOs.
   ***/
   bool isUTXO_ = false;
};

class TxIOPairUint
{
public:
   TxIOPairUint(const Armory::Types::ScrAddr&,
      Armory::Types::TxKey, Armory::Types::TxId,
      Armory::Types::Amount);
   TxIOPairUint(const Armory::Types::ScrAddr&,
      Armory::Types::TxIOKey,
      Armory::Types::Amount);

   bool hasTxIn(void) const;
   bool hasTxOutZC(void) const;
   bool hasTxInZC(void) const;

   Armory::Types::Amount getAmount(void) const;
   uint32_t getTxTime(void) const;
   const Armory::Types::ScrAddr& getScrAddr(void) const;
   Armory::Types::TxKey getTxKeyOfOutput(void) const;
   Armory::Types::TxKey getTxKeyOfInput(void) const;
   Armory::Types::TxIOKey getTxIOKeyOfOutput(void) const;
   Armory::Types::TxIOKey getTxIOKeyOfInput(void) const;
   Armory::Types::TxIOId getIndexOfOutput(void) const;
   Armory::Types::TxIOId getIndexOfInput(void) const;

   void setTxIn(Armory::Types::TxKey, Armory::Types::TxId);
   void setTxIn(Armory::Types::TxIOKey);
   void merge(const TxIOPairUint&);

   void setTxTime(uint32_t);
   void setRBF(bool);
   void setChained(bool);

   bool isRBF(void) const;
   bool isChained(void) const;

private:
   const Armory::Types::ScrAddr scrAddr_;
   const Armory::Types::Amount amount_;
   const Armory::Types::TxIOKey txIOKeyOfOutput_;

   Armory::Types::TxIOKey txIOKeyOfInput_ = Armory::Types::INVALID_TXIO_KEY;
   uint32_t txTime_ = UINT32_MAX;

   bool isRBF_ = false;
   bool isZCChained_ = false;
};
