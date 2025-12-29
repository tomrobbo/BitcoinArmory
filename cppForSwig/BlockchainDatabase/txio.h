////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2011-2015, Armory Technologies, Inc.                        //
//  Distributed under the GNU Affero General Public License (AGPL v3)         //
//  See LICENSE-ATI or http://www.gnu.org/licenses/agpl.html                  //
//                                                                            //
//                                                                            //
//  Copyright (C) 2016, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <Utils/BinaryData.h>

class LMDBBlockDatabase;
class Outpoint;
class TxIn;
class TxOut;
class Tx;

////////////////////////////////////////////////////////////////////////////////
class TxRef
{
   friend class BlockDataManager;
   friend class Tx;
   friend class TxIOPair;

public:
   TxRef(void);
   TxRef(BinaryDataRef);

   bool isInitialized(void) const;

   const BinaryData& getDBKey(void) const;
   BinaryDataRef getDBKeyRef(void) const;
   BinaryData getDBKeyOfChild(uint16_t) const;
   uint16_t getBlockTxIndex(void) const;
   uint32_t getBlockHeight(void) const;
   uint8_t getDuplicateID(void) const;
   void pprint(std::ostream& = std::cout, int = 0) const;

   bool operator==(const BinaryData&) const;
   bool operator==(const TxRef&) const;
   bool operator>=(const BinaryData&) const;

protected:
   BinaryData dbKey6B_;
};

class DBTxRef : public TxRef
{
public:
   DBTxRef(const TxRef&, const LMDBBlockDatabase*);

   BinaryData serialize(void) const;
   BinaryData getThisHash(void) const;
   Tx getTxCopy(void) const;
   bool isMainBranch(void) const;

   /////////////////////////////////////////////////////////////////////////////
   // This as fast as you can get a single TxIn or TxOut from the DB.  But if 
   // need multiple of them from the same Tx, you should getTxCopy() and then
   // iterate over them in the Tx object
   TxIn  getTxInCopy(uint32_t);
   TxOut getTxOutCopy(uint32_t);

private:
   const LMDBBlockDatabase* db_;
};

////////////////////////////////////////////////////////////////////////////////
class TxIOPair
{
public:
   explicit TxIOPair(const BinaryData&, uint64_t);
   explicit TxIOPair(const TxRef&, uint16_t, uint64_t);

   // Lots of accessors
   bool      hasTxIn(void) const;
   bool      hasTxOutInMain(LMDBBlockDatabase*) const;
   bool      hasTxInInMain(LMDBBlockDatabase*) const;
   bool      hasTxOutZC(void) const;
   bool      hasTxInZC(void) const;
   uint64_t  getValue(void) const;

   ////
   const TxRef& getTxRefOfOutput(void) const;
   const TxRef& getTxRefOfInput(void) const;
   BinaryData getDBKeyOfOutput(void) const;
   BinaryData getDBKeyOfInput(void) const;

   uint32_t  getIndexOfOutput(void) const;
   uint32_t  getIndexOfInput(void) const;
   Outpoint  getOutPoint(LMDBBlockDatabase*) const;

   std::pair<bool, bool> reassessValidity(LMDBBlockDatabase*);
   bool  isTxOutFromSelf(void) const;
   void setTxOutFromSelf(bool = true);
   bool  isFromCoinbase(void) const;
   void setFromCoinbase(bool = true);
   bool  isMultisig(void) const;
   void setMultisig(bool = true);
   bool isRBF(void) const;
   void setRBF(bool);
   void setChained(bool);
   bool isChainedZC(void) const;

   ////
   BinaryData getTxHashOfInput(const LMDBBlockDatabase* = nullptr) const;
   BinaryData getTxHashOfOutput(const LMDBBlockDatabase* = nullptr) const;
   void setTxHashOfInput(const BinaryData&);
   void setTxHashOfOutput(const BinaryData&);

   TxOut getTxOutCopy(LMDBBlockDatabase*) const;
   TxIn  getTxInCopy(LMDBBlockDatabase*) const;

   bool setTxIn(const TxRef&, uint32_t);
   bool setTxIn(const BinaryData&);
   void merge(const TxIOPair&);

   ////
   bool isSpent(LMDBBlockDatabase*) const;
   bool isUnspent(LMDBBlockDatabase*) const;
   bool isSpendable(LMDBBlockDatabase*, uint32_t) const;
   bool isMineButUnconfirmed(
      LMDBBlockDatabase*, uint32_t, unsigned) const;
   void pprintOneLine(LMDBBlockDatabase*) const;

   bool operator<(const TxIOPair&) const;
   bool operator==(const TxIOPair&) const;
   bool operator>=(const BinaryData&) const;

   void setTxTime(uint32_t);
   uint32_t getTxTime(void) const;

   bool isUTXO(void) const;
   void setUTXO(bool);

   void setScrAddrRef(const BinaryDataRef&);
   const BinaryDataRef& getScrAddr(void) const;

public:
   bool flagged = false;

private:
   const uint64_t amount_;
   const TxRef txRefOfOutput_;
   const uint32_t indexOfOutput_;

   TxRef    txRefOfInput_;
   uint32_t indexOfInput_;

   mutable  BinaryData txHashOfOutput_;
   mutable  BinaryData txHashOfInput_;

   // Zero-conf data isn't on disk, yet, so can't use TxRef
   bool     isTxOutFromSelf_ = false;
   bool     isFromCoinbase_;
   bool     isMultisig_;
   bool     isRBF_ = false;
   bool     isZCChained_ = false;

   //mainly for ZC ledgers. Could replace the need for a blockchain 
   //object to build scrAddrObj ledgers.
   uint32_t txtime_;

   /***marks txio as spent for serialize/deserialize operations. It signifies
   whether a subSSH entry with only a TxOut DBkey is spent.

   To allow for partial parsing of ssh history, all txouts need to be visible at
   the height they appeared, amd spent txouts need to be visible at the
   spending txin's height as well.

   While spent txouts at txin height are unique, spent txouts at txout height
   need to be differenciated from UTXOs.
   ***/
   bool     isUTXO_ = false;

   //used to get a relevant scrAddr from a txio
   BinaryDataRef scrAddr_;
};
