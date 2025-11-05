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

#include "BtcUtils.h"
#include "BinaryData.h"
#include "EncryptionUtils.h"
#include "ArmoryConfig.h"
#include "btc/segwit_addr.h"
#include "TxOutScrRef.h"
#include "btc/base58.h"

using namespace std::string_view_literals;

////////////////////////////////////////////////////////////////////////////////
// static members
const BinaryData BtcUtils::BadAddress = BinaryData::CreateFromHex(
   "0000000000000000000000000000000000000000");
const BinaryData BtcUtils::EmptyHash  = BinaryData::CreateFromHex(
   "0000000000000000000000000000000000000000000000000000000000000000");

constexpr char BtcUtils::base64Chars[]{
   "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};

const std::map<char, uint8_t> BtcUtils::base64Vals = {
   { 'A', 0 }, { 'B', 1 }, { 'C', 2 }, { 'D', 3 }, { 'E', 4 }, { 'F', 5 },
   { 'G', 6 }, { 'H', 7 }, { 'I', 8 }, { 'J', 9 }, { 'K', 10 }, { 'L', 11 },
   { 'M', 12 }, { 'N', 13 }, { 'O', 14 }, { 'P', 15 }, { 'Q', 16 }, { 'R', 17 },
   { 'S', 18 }, { 'T', 19 }, { 'U', 20 }, { 'V', 21 }, { 'W', 22 }, { 'X', 23 },
   { 'Y', 24 }, { 'Z', 25 }, { 'a', 26 }, { 'b', 27 }, { 'c', 28 }, { 'd', 29 },
   { 'e', 30 }, { 'f', 31 }, { 'g', 32 }, { 'h', 33 }, { 'i', 34 }, { 'j', 35 },
   { 'k', 36 }, { 'l', 37 }, { 'm', 38 }, { 'n', 39 }, { 'o', 40 }, { 'p', 41 },
   { 'q', 42 }, { 'r', 43 }, { 's', 44 }, { 't', 45 }, { 'u', 46 }, { 'v', 47 },
   { 'w', 48 }, { 'x', 49 }, { 'y', 50 }, { 'z', 51 }, { '0', 52 }, { '1', 53 },
   { '2', 54 }, { '3', 55 }, { '4', 56 }, { '5', 57 }, { '6', 58 }, { '7', 59 },
   { '8', 60 }, { '9', 61 }, { '+', 62 }, { '/', 63 }
};

////////////////////////////////////////////////////////////////////////////////
// exceptions
BlockDeserializingException::BlockDeserializingException(
   const std::string& what) :
   std::runtime_error(what)
{}

VarIntException::VarIntException(const std::string& what) :
   BlockDeserializingException(what)
{}

DERException::DERException(const std::string& what) :
   std::runtime_error(what)
{}

////////////////////////////////////////////////////////////////////////////////
// varint
uint64_t BtcUtils::readVarInt(const uint8_t* strmPtr, size_t remaining,
   uint8_t& lenOut)
{
   if (remaining < 1) {
      throw VarIntException("invalid varint");
   }
   uint8_t firstByte = strmPtr[0];

   if (firstByte < 0xfd) {
      lenOut = 1;
      return firstByte;
   }

   if (firstByte == 0xfd) {
      if (remaining < 3) {
         throw VarIntException("invalid varint");
      }
      lenOut = 3;
      return READ_UINT16_LE(strmPtr+1);
   } else if(firstByte == 0xfe) {
      if (remaining < 5) {
         throw VarIntException("invalid varint");
      }
      lenOut = 5;
      return READ_UINT32_LE(strmPtr+1);
   } else {
      if (remaining < 9) {
         throw VarIntException("invalid varint");
      }
      lenOut = 9;
      return READ_UINT64_LE(strmPtr+1);
   }
}

std::pair<uint64_t, uint8_t> BtcUtils::readVarInt(BinaryRefReader& brr)
{
   uint64_t outVal;
   uint8_t outLen;
   outVal = readVarInt(brr.getCurrPtr(), brr.getSizeRemaining(), outLen);
   brr.advance(outLen);
   return std::make_pair(outVal, outLen);
}

uint8_t BtcUtils::readVarIntLength(const uint8_t* strmPtr)
{
   switch (strmPtr[0])
   {
      case 0xfd: return 3;
      case 0xfe: return 5;
      case 0xff: return 9;
      default:
         return 1;
   }
}

uint8_t BtcUtils::calcVarIntSize(const uint64_t& val)
{
   if (val < 0xfd) {
      return 1;
   } else if (val <= 0xffff) {
      return 3;
   } else if (val <= 0xffffffff) {
      return 5;
   } else {
      return 9;
   }
}

////////////////////////////////////////////////////////////////////////////////
// hashes
void BtcUtils::getSha256(const uint8_t* data, size_t len,
   BinaryData& hashOutput)
{
   if (hashOutput.getSize() != 32) {
      hashOutput.resize(32);
   }

   BinaryDataRef dataBdr(data, len);
   CryptoSHA2::getSha256(dataBdr, hashOutput.getPtr());
}

BinaryData BtcUtils::getSha256(const BinaryData& bd)
{
   BinaryData hashOutput;
   getSha256(bd.getPtr(), bd.getSize(), hashOutput);
   return hashOutput;
}

BinaryData BtcUtils::getHMAC256(const SecureBinaryData& key,
   const SecureBinaryData& message)
{
   BinaryData digest;
   digest.resize(32);
   
   getHMAC256(key.getPtr(), key.getSize(), 
      message.getCharPtr(), message.getSize(),
      digest.getPtr());

   return digest;
}

//// hash256
void BtcUtils::getHash256(const uint8_t* strToHash, size_t nBytes,
   BinaryData& hashOutput)
{
   if (hashOutput.getSize() != 32) {
         hashOutput.resize(32);
   }

   BinaryDataRef dataBdr(strToHash, nBytes);
   CryptoSHA2::getHash256(dataBdr, hashOutput.getPtr());
}

BinaryData BtcUtils::getHash256(const uint8_t* strToHash, size_t nBytes)
{
   BinaryData hashOutput(32);
   BinaryDataRef dataBdr(strToHash, nBytes);

   CryptoSHA2::getHash256(dataBdr, hashOutput.getPtr());
   return hashOutput;
}

void BtcUtils::getHash256(const BinaryData& strToHash, BinaryData& hashOutput)
{
   getHash256(strToHash.getPtr(), strToHash.getSize(), hashOutput);
}

void BtcUtils::getHash256(BinaryDataRef strToHash, BinaryData& hashOutput)
{
   getHash256(strToHash.getPtr(), strToHash.getSize(), hashOutput);
}

BinaryData BtcUtils::getHash256(const BinaryData& strToHash)
{
   BinaryData hashOutput(32);
   getHash256(strToHash.getPtr(), strToHash.getSize(), hashOutput);
   return hashOutput;
}

BinaryData BtcUtils::getHash256(const BinaryDataRef& strToHash)
{
   BinaryData hashOutput(32);
   getHash256(strToHash.getPtr(), strToHash.getSize(), hashOutput);
   return hashOutput;
}

//// hash160
void BtcUtils::getHash160(const uint8_t* strToHash, size_t nBytes,
   BinaryData& hashOutput)
{
   if (hashOutput.getSize() != 20) {
      hashOutput.resize(20);
   }

   BinaryDataRef bdr(strToHash, nBytes);
   BinaryData sha2_digest(32);

   CryptoSHA2::getSha256(bdr, sha2_digest.getPtr());
   CryptoHASH160::getHash160(sha2_digest.getRef(), hashOutput.getPtr());
}

BinaryData BtcUtils::getHash160(const uint8_t* strToHash, size_t nBytes)
{
   BinaryData hashOutput(20);
   getHash160(strToHash, nBytes, hashOutput);
   return hashOutput;
}

void BtcUtils::getHash160(BinaryDataRef strToHash, BinaryData& hashOutput)
{
   getHash160(strToHash.getPtr(), strToHash.getSize(), hashOutput);
}

BinaryData BtcUtils::getHash160(const BinaryDataRef& strToHash)
{
   BinaryData hashOutput(20);
   getHash160(strToHash.getPtr(), strToHash.getSize(), hashOutput);
   return hashOutput;
}

BinaryData BtcUtils::getHash160(const BinaryData& strToHash)
{
   BinaryData hashOutput(20);
   getHash160(strToHash.getPtr(), strToHash.getSize(), hashOutput);
   return hashOutput;
}

BinaryData BtcUtils::ripemd160(const BinaryData& strToHash)
{
   BinaryData bd(20);
   CryptoHASH160::getHash160(strToHash.getRef(), bd.getPtr());
   return bd;
}

//// HMACs
BinaryData BtcUtils::getHMAC512(const SecureBinaryData& key,
   const SecureBinaryData& message)
{
   BinaryData digest;
   digest.resize(64);

   getHMAC512(key.getPtr(), key.getSize(),
      message.getCharPtr(), message.getSize(),
      digest.getPtr());
   return digest;
}

BinaryData BtcUtils::getHMAC256(const BinaryData& key,
   const std::string& message)
{
   BinaryData digest;
   digest.resize(32);

   getHMAC256(key.getPtr(), key.getSize(),
      message.c_str(), message.size(),
      digest.getPtr());
   return digest;
}

BinaryData BtcUtils::getHMAC512(const BinaryData& key,
   const std::string& message)
{
   BinaryData digest;
   digest.resize(64);

   getHMAC512(key.getPtr(), key.getSize(),
      message.c_str(), message.size(),
      digest.getPtr());
   return digest;
}

SecureBinaryData BtcUtils::getHMAC512(const std::string& key,
   const SecureBinaryData& message)
{
   SecureBinaryData digest;
   digest.resize(64);

   getHMAC512(key.c_str(), key.size(),
      message.getPtr(), message.getSize(),
      digest.getPtr());
   return digest;
}


void BtcUtils::getHMAC256(const uint8_t* keyptr, size_t keylen,
   const char* msgptr, size_t msglen, uint8_t* digest)
{
   BinaryDataRef key_bdr(keyptr, keylen);
   BinaryDataRef msg_bdr((uint8_t*)msgptr, msglen);
   CryptoSHA2::getHMAC256(key_bdr, msg_bdr, digest);
}

void BtcUtils::getHMAC512(const void* keyptr, size_t keylen,
   const void* msgptr, size_t msglen, void* digest)
{
   BinaryDataRef key_bdr((uint8_t*)keyptr, keylen);
   BinaryDataRef msg_bdr((uint8_t*)msgptr, msglen);
   CryptoSHA2::getHMAC512(key_bdr, msg_bdr, (uint8_t*)digest);
}

BinaryData BtcUtils::getBotchedArmoryHMAC256(
   const BinaryData& key, const BinaryData& msg)
{
   BinaryData hmacKey;
   if (key.getSize() > 32) {
      hmacKey = BtcUtils::getSha256(key);
   } else if (key.getSize() <= 32) {
      hmacKey.resize(32);
      memcpy(hmacKey.getPtr(), key.getPtr(), key.getSize());
      memset(hmacKey.getPtr() + key.getSize(), 0, 32 - key.getSize());
   }

   BinaryData oxor(32), ixor(32);
   for (unsigned i=0; i<32; i++) {
      oxor.getPtr()[i] = hmacKey.getPtr()[i] ^ 0x5c;
      ixor.getPtr()[i] = hmacKey.getPtr()[i] ^ 0x36;
   }

   ixor.append(msg);
   auto iHash = BtcUtils::getSha256(ixor);

   BinaryWriter bw;
   bw.put_BinaryData(oxor);
   bw.put_BinaryData(iHash);

   return BtcUtils::getSha256(bw.getData());
}

////////////////////////////////////////////////////////////////////////////////
// merkle tree
std::vector<BinaryData> BtcUtils::calculateMerkleTree(
   const std::vector<BinaryData>& txhashlist)
{
   // Don't know in advance how big this list will be, make a list too big
   // and copy the result to the right size list afterwards
   size_t numTx = txhashlist.size();
   std::vector<BinaryData> merkleTree(3*numTx);
   BinaryData hashInput(64);

   for (unsigned i=0; i<numTx; i++) {
      merkleTree[i] = txhashlist[i];
   }

   size_t thisLevelStart = 0;
   size_t nextLevelStart = numTx;
   size_t levelSize = numTx;
   BinaryData hashOutput(32);
   while (levelSize>1) {
      for (unsigned j=0; j<(levelSize+1)/2; j++) {
         uint8_t* half1Ptr = hashInput.getPtr();
         uint8_t* half2Ptr = hashInput.getPtr()+32;

         if (j < levelSize/2) {
            merkleTree[thisLevelStart+(2*j)  ].copyTo(half1Ptr, 32);
            merkleTree[thisLevelStart+(2*j)+1].copyTo(half2Ptr, 32);
         } else {
            merkleTree[nextLevelStart-1].copyTo(half1Ptr, 32);
            merkleTree[nextLevelStart-1].copyTo(half2Ptr, 32);
         }

         CryptoSHA2::getHash256(hashInput.getRef(), hashOutput.getPtr());
         merkleTree[nextLevelStart+j] = hashOutput;
      }
      levelSize = (levelSize+1)/2;
      thisLevelStart = nextLevelStart;
      nextLevelStart = nextLevelStart+levelSize;
   }

   // nextLevelStart is the size of the merkle tree
   merkleTree.erase(merkleTree.begin()+nextLevelStart, merkleTree.end());
   return merkleTree;
}

BinaryData BtcUtils::calculateMerkleRoot(
   const std::vector<BinaryData>& txhashlist)
{
   std::vector<BinaryData> mtree = calculateMerkleTree(txhashlist);
   return mtree[mtree.size()-1];
}

////////////////////////////////////////////////////////////////////////////////
// tx length parsing
void BtcUtils::TxInCalcLength(const uint8_t* ptr, size_t size,
   std::vector<size_t>* offsetsIn)
{
   BinaryRefReader brr(ptr, size);
   if (brr.getSizeRemaining() < 4) {
      throw BlockDeserializingException();
   }

   // Tx Version
   brr.advance(4);

   // TxIn List
   auto nIn = brr.get_var_int();
   if (offsetsIn != nullptr) {
      offsetsIn->resize(nIn + 1);
      for (auto i = 0; i<nIn; i++) {
         (*offsetsIn)[i] = brr.getPosition();
         brr.advance(TxInCalcLength(brr.getCurrPtr(), brr.getSizeRemaining()));
      }

      // add in end of the last txin
      (*offsetsIn)[nIn] = brr.getPosition();
   }
}

size_t BtcUtils::TxInCalcLength(const uint8_t* ptr, size_t size)
{
   if (size < 37) {
      throw BlockDeserializingException();
   }
   uint8_t viLen;
   size_t scrLen = (size_t)readVarInt(ptr+36, size-36, viLen);
   return (36 + viLen + scrLen + 4);
}

size_t BtcUtils::TxOutCalcLength(const uint8_t* ptr, size_t size)
{
   if (size < 9) {
      throw BlockDeserializingException();
   }

   uint8_t viLen;
   size_t scrLen = (size_t)readVarInt(ptr+8, size-8, viLen);
   return (8 + viLen + scrLen);
}

size_t BtcUtils::TxWitnessCalcLength(const uint8_t* ptr, size_t size)
{
   if (size < 1) {
      throw BlockDeserializingException();
   }

   size_t witLen = 0;
   uint8_t viStackLen;
   size_t stackLen = readVarInt(ptr, size, viStackLen);
   witLen += viStackLen;
   for (auto i = 0; i < stackLen; i++) {
      if (witLen >= size) {
         throw BlockDeserializingException();
      }
      uint8_t viLen;
      witLen += readVarInt(ptr + witLen, size - witLen, viLen);
      witLen += viLen;
      if (witLen > size) {
         throw BlockDeserializingException();
      }
   }
   return witLen;
}

bool BtcUtils::checkSwMarker(const uint8_t* ptr)
{
   return ptr[0] == 0x00 && ptr[1] == 0x01;
}

size_t BtcUtils::TxCalcLength(const uint8_t* ptr, size_t size,
   std::vector<size_t>* offsetsIn,
   std::vector<size_t>* offsetsOut,
   std::vector<size_t>* offsetsWitness)
{
   BinaryRefReader brr(ptr, size);

   if (brr.getSizeRemaining() < 4) {
      throw BlockDeserializingException();
   }

   // Tx Version;
   brr.advance(4);

   // Get marker and flag if transaction uses segwit
   auto usesWitness = checkSwMarker(brr.getCurrPtr());
   if (usesWitness) {
      brr.advance(2);
   }

   // TxIn List
   size_t nIn = brr.get_var_int();
   if (offsetsIn != nullptr) {
      offsetsIn->resize(nIn+1);
      for (auto i=0; i < nIn; i++) {
         (*offsetsIn)[i] = brr.getPosition();
         brr.advance(TxInCalcLength(
            brr.getCurrPtr(),
            brr.getSizeRemaining())
         );
      }
      (*offsetsIn)[nIn] = brr.getPosition(); // Get the end of the last
   } else {
      // Don't need to track the offsets, just leap over everything
      for (auto i=0; i < nIn; i++) {
         brr.advance(TxInCalcLength(
            brr.getCurrPtr(),
            brr.getSizeRemaining())
         );
      }
   }

   // Now extract the TxOut list
   size_t nOut = brr.get_var_int();
   if (offsetsOut != nullptr) {
      offsetsOut->resize(nOut+1);
      for (auto i=0; i<nOut; i++) {
         (*offsetsOut)[i] = brr.getPosition();
         brr.advance(TxOutCalcLength(
            brr.getCurrPtr(),
            brr.getSizeRemaining())
         );
      }
      (*offsetsOut)[nOut] = brr.getPosition();
   } else {
      for (size_t i=0; i<nOut; i++) {
         brr.advance(TxOutCalcLength(
            brr.getCurrPtr(),
            brr.getSizeRemaining())
         );
      }
   }

   // Now extract the witnesses
   if (usesWitness) {
      if (offsetsWitness != nullptr) {
         offsetsWitness->resize(nIn + 1);
         for (uint32_t i = 0; i < nIn; i++) {
            (*offsetsWitness)[i] = brr.getPosition();
            brr.advance(TxWitnessCalcLength(
               brr.getCurrPtr(),
               brr.getSizeRemaining())
            );
         }
         (*offsetsWitness)[nIn] = brr.getPosition();
      } else {
         for (uint32_t i = 0; i < nIn; i++) {
            brr.advance(TxWitnessCalcLength(
               brr.getCurrPtr(),
               brr.getSizeRemaining())
            );
         }
      }
   } else {
      if (offsetsWitness != nullptr) {
         offsetsWitness->resize(1);
         (*offsetsWitness)[0] = brr.getPosition();
      }
   }

   brr.advance(4);
   return brr.getPosition();
}

size_t BtcUtils::StoredTxCalcLength(const uint8_t* ptr,
   size_t len, bool fragged,
   std::vector<size_t>* offsetsIn,
   std::vector<size_t>* offsetsOut,
   std::vector<size_t>* offsetsWitness)
{
   BinaryRefReader brr(ptr, len);

   // Tx Version;
   brr.advance(4);

   // Get marker and flag if transaction uses segwit
   auto usesWitness = checkSwMarker(brr.getCurrPtr());
   if (usesWitness) {
      brr.advance(2);
   }

   // TxIn List
   size_t nIn = brr.get_var_int();
   if (offsetsIn != nullptr) {
      offsetsIn->resize(nIn+1);
      for (auto i=0; i<nIn; i++) {
         (*offsetsIn)[i] = brr.getPosition();
         brr.advance(TxInCalcLength(
            brr.getCurrPtr(),
            brr.getSizeRemaining())
         );
      }
      (*offsetsIn)[nIn] = brr.getPosition(); // Get the end of the last
   } else {
      // Don't need to track the offsets, just leap over everything
      for (auto i=0; i<nIn; i++) {
         brr.advance(TxInCalcLength(
            brr.getCurrPtr(),
            brr.getSizeRemaining())
         );
      }
   }

   // Now extract the TxOut list
   size_t nOut = brr.get_var_int();
   if (fragged) {
      offsetsOut->resize(nOut+1);
      for (uint32_t i=0; i<nOut+1; i++) {
         (*offsetsOut)[i] = brr.getPosition();
      }
   } else {
      // Now extract the TxOut list
      if (offsetsOut != nullptr) {
         offsetsOut->resize(nOut+1);
         for (auto i=0; i<nOut; i++) {
            (*offsetsOut)[i] = brr.getPosition();
            brr.advance(TxOutCalcLength(
               brr.getCurrPtr(),
               brr.getSizeRemaining())
            );
         }
         (*offsetsOut)[nOut] = brr.getPosition();
      } else {
         for (auto i=0; i<nOut; i++) {
            brr.advance(TxOutCalcLength(
               brr.getCurrPtr(),
               brr.getSizeRemaining())
            );
         }
      }
   }

   // Now extract the witnesses
   if (usesWitness) {
      if (offsetsWitness != nullptr) {
         offsetsWitness->resize(nIn + 1);
         for (auto i = 0; i < nIn; i++) {
            (*offsetsWitness)[i] = brr.getPosition();
            brr.advance(TxWitnessCalcLength(brr.getCurrPtr(), brr.getSizeRemaining()));
         }
         (*offsetsWitness)[nIn] = brr.getPosition();
      } else {
         for (auto i = 0; i < nIn; i++) {
            brr.advance(TxWitnessCalcLength(brr.getCurrPtr(), brr.getSizeRemaining()));
         }
      }
   } else {
      if (offsetsWitness != nullptr) {
         offsetsWitness->resize(1);
         (*offsetsWitness)[0] = brr.getPosition();
      }
   }

   brr.advance(4);
   return brr.getPosition();
}

////////////////////////////////////////////////////////////////////////////////
// script type parsing
TXOUT_SCRIPT_TYPE BtcUtils::getTxOutScriptType(BinaryDataRef s)
{
   size_t sz = s.getSize();
   if (sz > 0 && sz < 81 && s[0] == 0x6a) {
      return TXOUT_SCRIPT_OPRETURN;
   } else if (sz < 21) {
      return TXOUT_SCRIPT_NONSTANDARD;
   } else if (sz == 22 && s[0] == 0x00 && s[1] == 0x14) {
      return TXOUT_SCRIPT_P2WPKH;
   } else if (sz == 34 && s[0] == 0x00 && s[1] == 0x20) {
      return TXOUT_SCRIPT_P2WSH;
   } else if (sz == 25 &&
      s[0] == 0x76 &&
      s[1] == 0xa9 &&
      s[2] == 0x14 &&
      s[-2] == 0x88 &&
      s[-1] == 0xac) {
      return TXOUT_SCRIPT_STDHASH160;
   } else if (sz == 67 && s[0] == 0x41 && s[1] == 0x04 && s[-1] == 0xac) {
      return TXOUT_SCRIPT_STDPUBKEY65;
   } else if (sz == 35 &&
      s[0] == 0x21 &&
      (s[1] == 0x02 || s[1] == 0x03) &&
      s[-1] == 0xac) {
      return TXOUT_SCRIPT_STDPUBKEY33;
   } else if (sz == 23 && s[0] == 0xa9 && s[1] == 0x14 && s[-1] == 0x87) {
      return TXOUT_SCRIPT_P2SH;
   } else if (s[-1] == 0xae && isMultisigScript(s)) {
      return TXOUT_SCRIPT_MULTISIG;
   } else {
      return TXOUT_SCRIPT_NONSTANDARD;
   }
}

TXIN_SCRIPT_TYPE BtcUtils::getTxInScriptType(BinaryDataRef script,
   BinaryDataRef prevTxHash)
{
   if (prevTxHash == EmptyHash) {
      return TXIN_SCRIPT_COINBASE;
   }

   if (script.empty()) {
      return TXIN_SCRIPT_WITNESS;
   }
   if (script.getSize() == 23 && script[1] == 0x00 && script[2] == 0x14) {
      return TXIN_SCRIPT_P2WPKH_P2SH;
   }
   if (script.getSize() == 35 && script[1] == 0x00 && script[2] == 0x20) {
      return TXIN_SCRIPT_P2WSH_P2SH;
   }

   // Technically, this doesn't recognize all P2SH spends. Only
   // spends of P2SH scripts that are, themselves, standard
   BinaryData lastPush = getLastPushDataInScript(script);
   if (getTxOutScriptType(lastPush) != TXOUT_SCRIPT_NONSTANDARD) {
      return TXIN_SCRIPT_SPENDP2SH;
   }

   if (script[0]==0x00) {
      // TODO: All this complexity to check TxIn type may be too slow when
      //       scanning the blockchain...will need to investigate later
      std::vector<BinaryDataRef> splitScr = splitPushOnlyScriptRefs(script);

      if (splitScr.empty()) {
         return TXIN_SCRIPT_NONSTANDARD;
      }

      // TODO: Maybe should identify whether the other pushed data
      //       in the script is a potential solution for the
      //       subscript... meh?
      if (script[2]==0x30 && script[4]==0x02) {
         return TXIN_SCRIPT_SPENDMULTI;
      }
   }

   if (!(script[1]==0x30 && script[3]==0x02)) {
      return TXIN_SCRIPT_NONSTANDARD;
   }

   uint32_t sigSize = script[2] + 4;
   if (script.getSize() == sigSize) {
      return TXIN_SCRIPT_SPENDPUBKEY;
   }

   uint32_t keySizeFull = 66;  // \x41 \x04 [X32] [Y32]
   uint32_t keySizeCompr= 34;  // \x41 \x02 [X32]

   if (script.getSize() == sigSize + keySizeFull) {
      return TXIN_SCRIPT_STDUNCOMPR;
   } else if (script.getSize() == sigSize + keySizeCompr) {
      return TXIN_SCRIPT_STDCOMPR;
   }
   return TXIN_SCRIPT_NONSTANDARD;
}

////////////////////////////////////////////////////////////////////////////////
// txin address helpers
BinaryData BtcUtils::getTxInAddr(BinaryDataRef script,
   BinaryDataRef prevTxHash, TXIN_SCRIPT_TYPE type)
{
   if (type==TXIN_SCRIPT_NONSTANDARD) {
      type = getTxInScriptType(script, prevTxHash);
   }
   return getTxInAddrFromType(script, type);
}

BinaryData BtcUtils::getTxInAddrFromType(BinaryDataRef script,
   TXIN_SCRIPT_TYPE type)
{
   switch(type)
   {
      case TXIN_SCRIPT_STDUNCOMPR:
      {
         if (script.getSize() < 65) {
               throw BlockDeserializingException();
         }
         return getHash160(script.getSliceRef(-65, 65));
      }

      case TXIN_SCRIPT_STDCOMPR:
      {
         if (script.getSize() < 33) {
            throw BlockDeserializingException();
         }
         return getHash160(script.getSliceRef(-33, 33));
      }

      case TXIN_SCRIPT_SPENDP2SH:
      {
         auto pushVect = splitPushOnlyScriptRefs(script);
         return getHash160(pushVect[pushVect.size()-1]);
      }

      case TXIN_SCRIPT_COINBASE:
      case TXIN_SCRIPT_SPENDPUBKEY:
      case TXIN_SCRIPT_SPENDMULTI:
      case TXIN_SCRIPT_NONSTANDARD:
         return BadAddress;

      default:
         LOGERR << "What kind of TxIn script did we get?";
         return BadAddress;
   }
}

BinaryData BtcUtils::getTxInAddrFromTypeInt(const BinaryData& script,
   uint32_t typeInt)
{
   return getTxInAddrFromType(script.getRef(), (TXIN_SCRIPT_TYPE)typeInt);
}

////////////////////////////////////////////////////////////////////////////////
// multisig helpers
uint8_t BtcUtils::getMultisigPubKeyList(const BinaryData& script,
   std::vector<BinaryData>& pkList)
{
   if (script[-1] != 0xae) {
      return 0;
   }

   uint8_t M = script[0];
   uint8_t N = script[-2];
   if (M<81 || M>96|| N<81 || N>96) {
      return 0;
   }

   M -= 80;
   N -= 80;

   BinaryRefReader brr(script);
   brr.advance(1); // Skip over M-value
   pkList.resize(N);
   for (uint8_t i=0; i<N; i++) {
      uint8_t nextSz = brr.get_uint8_t();
      if (nextSz != 0x41 && nextSz != 0x21) {
         return 0;
      }

      try {
         pkList[i] = brr.get_BinaryDataRef(nextSz);
      } catch (const std::exception& e) {
         LOGERR << "Failed to decode pub keys for multisig script," <<
            " with error: " << e.what();
         LOGERR << script.toHexStr();
         return 0;
      }
   }
   return M;
}

uint8_t BtcUtils::getMultisigAddrList(const BinaryData& script,
   std::vector<BinaryData>& addr160List)
{
   std::vector<BinaryData> pkList;
   uint32_t M = getMultisigPubKeyList(script, pkList);
   size_t   N = pkList.size();

   if (M==0) {
      return 0;
   }

   addr160List.resize(N);
   for (uint32_t i=0; i<N; i++) {
      addr160List[i] = getHash160(pkList[i]);
   }
   return M;
}

BinaryData BtcUtils::getMultisigUniqueKey(const BinaryData& script)
{
   std::vector<BinaryData> a160List;
   uint8_t M = getMultisigAddrList(script, a160List);
   size_t  N = a160List.size();

   if (M==0) {
      return {};
   }

   BinaryWriter bw(2 + N*20); // reserve enough space for header + N addr
   bw.put_uint8_t((uint8_t)M);
   bw.put_uint8_t((uint8_t)N);

   std::sort(a160List.begin(), a160List.end());
   for (auto i=0; i<a160List.size(); i++) {
      bw.put_BinaryData(a160List[i]);
   }
   return bw.getData();
}

bool BtcUtils::isMultisigScript(BinaryDataRef script)
{
   std::vector<BinaryData> a160List;
   return getMultisigPubKeyList(script, a160List) != 0;
}

////////////////////////////////////////////////////////////////////////////////
// push data
std::vector<BinaryDataRef> BtcUtils::splitPushOnlyScriptRefs(
   BinaryDataRef script)
{
   std::vector<BinaryDataRef> opList;
   opList.reserve(4);

   BinaryRefReader brr(script);
   uint8_t nextOp;
   while (brr.getSizeRemaining() > 0) {
      nextOp = brr.get_uint8_t();
      if(nextOp == 0) {
         // Implicit pushdata
         brr.rewind(1);
         opList.emplace_back(brr.get_BinaryDataRef(1));
      } else if(nextOp < 76) {
         // Implicit pushdata
         opList.emplace_back(brr.get_BinaryDataRef(nextOp));
      } else if(nextOp == 76) {
         uint8_t nb = brr.get_uint8_t();
         opList.emplace_back( brr.get_BinaryDataRef(nb));
      } else if(nextOp == 77) {
         uint16_t nb = brr.get_uint16_t();
         opList.emplace_back( brr.get_BinaryDataRef(nb));
      } else if(nextOp == 78) {
         uint16_t nb = brr.get_uint32_t();
         opList.push_back( brr.get_BinaryDataRef(nb));
      }
      else if(nextOp > 78 && nextOp < 97 && nextOp !=80) {
         brr.rewind(1);
         opList.push_back( brr.get_BinaryDataRef(1));
      } else {
         return {};
      }
   }
   return opList;
}

std::vector<BinaryData> BtcUtils::splitPushOnlyScript(const BinaryData& script)
{
   auto refs = splitPushOnlyScriptRefs(script);
   std::vector<BinaryData> out(refs.size());
   for (uint32_t i=0; i<refs.size(); i++) {
      out[i].copyFrom(refs[i]);
   }
   return out;
}

BinaryData BtcUtils::getLastPushDataInScript(const BinaryData& script)
{
   auto refs = splitPushOnlyScriptRefs(script);
   if (refs.empty()) {
      return {};
   }
   return refs[refs.size() - 1];
}

BinaryData BtcUtils::getPushDataHeader(const BinaryData& data)
{
   BinaryWriter bw;

   if (data.getSize() <= 75) {
       bw.put_uint8_t((uint8_t)data.getSize());
   } else if (data.getSize() < UINT8_MAX) {
      bw.put_uint8_t(OP_PUSHDATA1);
      bw.put_uint8_t((uint8_t)data.getSize());
   } else if (data.getSize() < UINT16_MAX) {
      bw.put_uint8_t(OP_PUSHDATA2);
      bw.put_uint16_t((uint16_t)data.getSize());
   } else if (data.getSize() < UINT32_MAX) {
      bw.put_uint8_t(OP_PUSHDATA4);
      bw.put_uint32_t((uint32_t)data.getSize());
   } else {
      throw std::runtime_error("pushdata exceeds size limit");
   }
   return bw.getData();
}

////////////////////////////////////////////////////////////////////////////////
// txout script helpers
BinaryData BtcUtils::getP2PKHScript(const BinaryData& scriptHash)
{
   if (scriptHash.getSize() != 20) {
      throw std::runtime_error("invalid P2PKH hash size");
   }

   BinaryWriter bw;
   bw.put_uint8_t(OP_DUP);
   bw.put_uint8_t(OP_HASH160);
   bw.put_uint8_t(20);
   bw.put_BinaryData(scriptHash);
   bw.put_uint8_t(OP_EQUALVERIFY);
   bw.put_uint8_t(OP_CHECKSIG);
   return bw.getData();
}

BinaryData BtcUtils::getP2PKScript(const BinaryData& pubkey)
{
   if (pubkey.getSize() != 33 && pubkey.getSize() != 65) {
      throw std::runtime_error("invalid pubkey size");
   }

   BinaryWriter bw;
   bw.put_var_int(pubkey.getSize());
   bw.put_BinaryData(pubkey);
   bw.put_uint8_t(OP_CHECKSIG);
   return bw.getData();
}

BinaryData BtcUtils::getP2SHScript(const BinaryData& scriptHash)
{
   if (scriptHash.getSize() != 20) {
      throw std::runtime_error("invalid P2SH hash size");
   }

   BinaryWriter bw;
   bw.put_uint8_t(OP_HASH160);
   bw.put_uint8_t(20);
   bw.put_BinaryData(scriptHash);
   bw.put_uint8_t(OP_EQUAL);
   return bw.getData();
}

BinaryData BtcUtils::getP2WPKHOutputScript(const BinaryData& scriptHash)
{
   if (scriptHash.getSize() != 20) {
      throw std::runtime_error("invalid P2WPKH hash size");
   }

   BinaryWriter bw;
   bw.put_uint8_t(0);
   bw.put_uint8_t(20);
   bw.put_BinaryData(scriptHash);
   return bw.getData();
}

BinaryData BtcUtils::getP2WPKHWitnessScript(const BinaryData& scriptHash)
{
   if (scriptHash.getSize() != 20) {
      throw std::runtime_error("invalid P2WPKH hash size");
   }

   BinaryWriter bw;
   bw.put_uint8_t(OP_DUP);
   bw.put_uint8_t(OP_HASH160);
   bw.put_uint8_t(20);
   bw.put_BinaryData(scriptHash);
   bw.put_uint8_t(OP_EQUALVERIFY);
   bw.put_uint8_t(OP_CHECKSIG);
   return bw.getData();
}

BinaryData BtcUtils::getP2WSHOutputScript(const BinaryData& scriptHash)
{
   if (scriptHash.getSize() != 32) {
      throw std::runtime_error("invalid P2WSH hash size");
   }

   BinaryWriter bw;
   bw.put_uint8_t(0);
   bw.put_uint8_t(32);
   bw.put_BinaryData(scriptHash);
   return bw.getData();
}

BinaryData BtcUtils::getP2WSHWitnessScript(const BinaryData& scriptHash)
{
   if (scriptHash.getSize() != 32) {
      throw std::runtime_error("invalid P2WSH hash size");
   }

   BinaryWriter bw;
   bw.put_uint8_t(OP_SHA256);
   bw.put_uint8_t(32);
   bw.put_BinaryData(scriptHash);
   bw.put_uint8_t(OP_EQUAL);
   return bw.getData();
}

////////////////////////////////////////////////////////////////////////////////
SecureBinaryData BtcUtils::computeChainCode_ArmoryLegacy(
   const SecureBinaryData& privateRoot)
{
   /*
   Armory 1.35c defines the chaincode as HMAC<SHA256> with:
   key: double SHA256 of the root key
   message: 'Derive Chaincode from Root Key'

   TODO: The Armory Python code uses a botched self implemented HMAC256,
   reproduce it here.
   */

   auto hmacKey = getHash256(privateRoot);
   auto hmacMsg = BinaryData::fromString("Derive Chaincode from Root Key"sv);

   //use key as is for invalid armory hmac256: armory erroneously uses the
   //output size for sha256 (32 bytes) instead of the block size (64 bytes)
   return getBotchedArmoryHMAC256(hmacKey, hmacMsg);
}

////////////////////////////////////////////////////////////////////////////////
BinaryData BtcUtils::computeDataId(const SecureBinaryData& data,
   const std::string& message)
{
   if (data.empty()) {
      throw std::runtime_error("cannot compute id for empty data");
   }

   if (message.empty()) {
      throw std::runtime_error("cannot compute id for empty message");
   }

   //hmac the hash256 of the data with message
   auto hmacKey = getHash256(data);
   BinaryData id(32);

   getHMAC256(hmacKey.getPtr(), hmacKey.getSize(),
      message.c_str(), message.size(), id.getPtr());

   //return last 16 bytes
   return id.getSliceCopy(16, 16);
}

////////////////////////////////////////////////////////////////////////////////
// address helpers
BinaryData BtcUtils::getScrAddrForAddrStr(const std::string& addrStr)
{
   BinaryData scrAddr;
   try {
      scrAddr = base58toScrAddr(addrStr);
   } catch (const std::exception&) {
      auto scrAddrPair = BtcUtils::segWitAddressToScrAddr(addrStr);
      if (scrAddrPair.second != 0) {
         throw std::runtime_error(
            "[getScrAddrForAddrStr] unsupported sw version");
      }

      switch (scrAddrPair.first.getSize())
      {
         case 20:
         {
            scrAddr.resize(21);
            memset(scrAddr.getPtr(), SCRIPT_PREFIX_P2WPKH, 1);
            memcpy(scrAddr.getPtr() + 1, scrAddrPair.first.getPtr(), 20);
            break;
         }

         case 32:
         {
            scrAddr.resize(33);
            memset(scrAddr.getPtr(), SCRIPT_PREFIX_P2WSH, 1);
            memcpy(scrAddr.getPtr() + 1, scrAddrPair.first.getPtr(), 32);
            break;
         }

         default:
            break;
      }
   }

   if (scrAddr.empty()) {
      throw std::runtime_error(
         "[getScrAddrForAddrStr] failed to create recipient");
   }
   return scrAddr;
}

BinaryData BtcUtils::getTxOutScrAddr(BinaryDataRef script,
   TXOUT_SCRIPT_TYPE type)
{
   BinaryWriter bw;
   if (type == TXOUT_SCRIPT_NONSTANDARD) {
      type = getTxOutScriptType(script);
   }

   auto h160Prefix = Armory::Config::BitcoinSettings::getPubkeyHashPrefix();
   auto scriptPrefix = Armory::Config::BitcoinSettings::getScriptHashPrefix();

   switch (type)
   {
      case TXOUT_SCRIPT_STDHASH160:
      {
         bw.put_uint8_t(h160Prefix);
         bw.put_BinaryData(script.getSliceCopy(3, 20));
         return bw.getData();
      }

      case TXOUT_SCRIPT_P2WPKH:
      {
         bw.put_uint8_t(SCRIPT_PREFIX_P2WPKH);
         bw.put_BinaryData(script.getSliceCopy(2, 20));
         return bw.getData();
      }

      case TXOUT_SCRIPT_P2WSH:
      {
         bw.put_uint8_t(SCRIPT_PREFIX_P2WSH);
         bw.put_BinaryData(script.getSliceCopy(2, 32));
         return bw.getData();
      }

      case TXOUT_SCRIPT_STDPUBKEY65:
      {
         bw.put_uint8_t(h160Prefix);
         bw.put_BinaryData(getHash160(script.getSliceRef(1, 65)));
         return bw.getData();
      }

      case TXOUT_SCRIPT_STDPUBKEY33:
      {
         bw.put_uint8_t(h160Prefix);
         bw.put_BinaryData(getHash160(script.getSliceRef(1, 33)));
         return bw.getData();
      }

      case TXOUT_SCRIPT_P2SH:
      {
         bw.put_uint8_t(scriptPrefix);
         bw.put_BinaryData(script.getSliceCopy(2, 20));
         return bw.getData();
      }

      case TXOUT_SCRIPT_NONSTANDARD:
      {
         bw.put_uint8_t(SCRIPT_PREFIX_NONSTD);
         bw.put_BinaryData(getHash160(script));
         return bw.getData();
      }

      case TXOUT_SCRIPT_MULTISIG:
      {
         bw.put_uint8_t(SCRIPT_PREFIX_MULTISIG);
         bw.put_BinaryData(getMultisigUniqueKey(script));
         return bw.getData();
      }

      case TXOUT_SCRIPT_OPRETURN:
      {
         bw.put_uint8_t(SCRIPT_PREFIX_NONSTD);
         unsigned msg_pos = 1;
         if (script.getSize() > 77) {
            msg_pos += 2;
         } else if (script.getSize() > 1) {
            ++msg_pos;
         }

         bw.put_BinaryData(
            script.getSliceRef(msg_pos, script.getSize() - msg_pos));
         return bw.getData();
      }

      default:
         LOGERR << "What kind of TxOutScript did we get?";
         return {};
   }
}

BinaryData BtcUtils::getTxOutScriptForScrAddr(BinaryDataRef scrAddr)
{
   if (scrAddr.empty()) {
      throw std::runtime_error("invalid scrAddr size");
   }

   BinaryRefReader brr(scrAddr);
   auto prefix = brr.get_uint8_t();

   switch (prefix)
   {
      case SCRIPT_PREFIX_HASH160:
      case SCRIPT_PREFIX_HASH160_TESTNET:
         return getP2PKHScript(brr.get_BinaryData(brr.getSizeRemaining()));

      case SCRIPT_PREFIX_P2SH:
      case SCRIPT_PREFIX_P2SH_TESTNET:
         return getP2SHScript(brr.get_BinaryData(brr.getSizeRemaining()));

      case SCRIPT_PREFIX_P2WPKH:
         return getP2WPKHOutputScript(brr.get_BinaryData(brr.getSizeRemaining()));
      
      case SCRIPT_PREFIX_P2WSH:
         return getP2WSHOutputScript(brr.get_BinaryData(brr.getSizeRemaining()));

      default:
         throw std::runtime_error("unsupported scrAddr");
   }
}

TXOUT_SCRIPT_TYPE BtcUtils::getScriptTypeForScrAddr(BinaryDataRef scrAddr)
{
   if (scrAddr.getSize() == 21) {
      auto h160Prefix = Armory::Config::BitcoinSettings::getPubkeyHashPrefix();
      auto scriptPrefix = Armory::Config::BitcoinSettings::getScriptHashPrefix();

      auto prefix = *scrAddr.getPtr();
      if (prefix == h160Prefix) {
         return TXOUT_SCRIPT_STDHASH160;
      } else if (prefix == SCRIPT_PREFIX_P2WPKH) {
         return TXOUT_SCRIPT_P2WPKH;
      } else if (prefix == scriptPrefix) {
         return TXOUT_SCRIPT_P2SH;
      }
   } else if (scrAddr.getSize() == 32) {
      auto prefix = *scrAddr.getPtr();
      if (prefix == SCRIPT_PREFIX_P2WSH) {
         return TXOUT_SCRIPT_P2WSH;
      }
   }
   return TXOUT_SCRIPT_NONSTANDARD;
}

std::string BtcUtils::getAddressStrFromScrAddr(BinaryDataRef scrAddrRef)
{
   auto scrType = getScriptTypeForScrAddr(scrAddrRef);
   switch (scrType)
   {
      case TXOUT_SCRIPT_P2WPKH:
      case TXOUT_SCRIPT_P2WSH:
      {
         auto scrAddrNoPrefix =
            scrAddrRef.getSliceRef(1, scrAddrRef.getSize() -1);
         return BtcUtils::scrAddrToSegWitAddress(scrAddrNoPrefix);
      }

      case TXOUT_SCRIPT_STDHASH160:
      case TXOUT_SCRIPT_P2SH:
      {
         return BtcUtils::scrAddrToBase58(scrAddrRef);
      }

      default:
         throw std::runtime_error("unsupported address type");
   }
}

//no copy version, the regular one is too slow for scanning operations
TxOutScriptRef BtcUtils::getTxOutScrAddrNoCopy(BinaryDataRef script)
{
   TxOutScriptRef outputRef;

   auto p2pkh_prefix = SCRIPT_PREFIX(
      Armory::Config::BitcoinSettings::getPubkeyHashPrefix());
   auto p2sh_prefix = SCRIPT_PREFIX(
      Armory::Config::BitcoinSettings::getScriptHashPrefix());

   auto type = getTxOutScriptType(script);
   switch (type)
   {
      case TXOUT_SCRIPT_STDHASH160:
      {
         outputRef.type_ = p2pkh_prefix;
         outputRef.scriptRef_ = script.getSliceRef(3, 20);
         break;
      }

      case TXOUT_SCRIPT_P2WPKH:
      {
         outputRef.type_ = SCRIPT_PREFIX_P2WPKH;
         outputRef.scriptRef_ = script.getSliceRef(2, 20);
         break;
      }

      case TXOUT_SCRIPT_P2WSH:
      {
         outputRef.type_ = SCRIPT_PREFIX_P2WSH;
         outputRef.scriptRef_ = script.getSliceRef(2, 32);
         break;
      }

      case TXOUT_SCRIPT_STDPUBKEY65:
      {
         outputRef.type_ = p2pkh_prefix;
         outputRef.scriptCopy_ = getHash160(script.getSliceRef(1, 65));
         outputRef.scriptRef_.setRef(outputRef.scriptCopy_);
         break;
      }

      case TXOUT_SCRIPT_STDPUBKEY33:
      {
         outputRef.type_ = p2pkh_prefix;
         outputRef.scriptCopy_ = getHash160(script.getSliceRef(1, 33));
         outputRef.scriptRef_.setRef(outputRef.scriptCopy_);
         break;
      }

      case TXOUT_SCRIPT_P2SH:
      {
         outputRef.type_ = p2sh_prefix;
         outputRef.scriptRef_ = script.getSliceRef(2, 20);
         break;
      }

      case TXOUT_SCRIPT_NONSTANDARD:
      {
         outputRef.type_ = SCRIPT_PREFIX_NONSTD;
         outputRef.scriptCopy_ = getHash160(script);
         outputRef.scriptRef_.setRef(outputRef.scriptCopy_);
         break;
      }

      case TXOUT_SCRIPT_MULTISIG:
      {
         outputRef.type_ = SCRIPT_PREFIX_MULTISIG;
         outputRef.scriptCopy_ = getMultisigUniqueKey(script);
         outputRef.scriptRef_.setRef(outputRef.scriptCopy_);
         break;
      }

      case TXOUT_SCRIPT_OPRETURN:
      {
         outputRef.type_ = SCRIPT_PREFIX_OPRETURN;
         auto size = script.getSize();
         size_t pos = 1;
         if (size > 77) {
            pos += 2;
         }
         if (size > 1) {
            ++pos;
         }
         outputRef.scriptRef_ = script.getSliceRef(pos, size - pos);
         break;
      }

      default:
         LOGERR << "What kind of TxOutScript did we get?";
   }
   return outputRef;
}

BinaryData BtcUtils::getTxOutRecipientAddr(const BinaryDataRef& script,
   TXOUT_SCRIPT_TYPE type)
{
   if (type==TXOUT_SCRIPT_NONSTANDARD) {
      type = getTxOutScriptType(script);
   }
   switch(type)
   {
      case TXOUT_SCRIPT_STDHASH160:    return script.getSliceCopy(3,20);
      case TXOUT_SCRIPT_STDPUBKEY65:   return getHash160(script.getSliceRef(1,65));
      case TXOUT_SCRIPT_STDPUBKEY33:   return getHash160(script.getSliceRef(1,33));
      case TXOUT_SCRIPT_P2SH:          return script.getSliceCopy(2,20);
      case TXOUT_SCRIPT_P2WSH:         return script.getSliceCopy(2,32);
      case TXOUT_SCRIPT_P2WPKH:        return script.getSliceCopy(2,20);
      case TXOUT_SCRIPT_MULTISIG:      return BadAddress;
      case TXOUT_SCRIPT_NONSTANDARD:   return BadAddress;
      default:                         return BadAddress;
   }
}

////////////////////////////////////////////////////////////////////////////////
// base64
std::string BtcUtils::base64_encode(const std::string& in)
{
   size_t main_count = in.size() / 3;
   std::string result;
   result.reserve(main_count * 4 + 5);

   auto ptr = (const uint8_t*)in.c_str();
   for (unsigned i = 0; i < main_count; i++) {
      uint32_t bits24 = ptr[i * 3] << 24 | ptr[i*3+1] << 16 | ptr[i*3+2] << 8;
      for (unsigned y = 0; y < 4; y++) {
         unsigned val = (bits24 & 0xFC000000) >> 26;
         result.append(1, base64Chars[val]);
         bits24 <<= 6;
      }
   }

   //padding
   size_t left_over = in.size() - main_count * 3;
   if (left_over == 0) {
      return result;
   }

   uint32_t bits24;
   if (left_over == 1) {
      bits24 = ptr[main_count * 3] << 24;
   } else {
      bits24 = ptr[main_count * 3] << 24 | ptr[main_count * 3 + 1] << 16;
   }

   for (unsigned i = 0; i <= left_over; i++) {
      unsigned val = (bits24 & 0xFC000000) >> 26;
      result.append(1, base64Chars[val]);
      bits24 <<= 6;
   }

   result.append(3 - left_over, '=');
   return result;
}

std::string BtcUtils::base64_decode(const std::string& in)
{
   size_t count = (in.size() + 3) / 4;
   std::string result;
   result.resize(count * 3);
   auto ptr = in.c_str();
   auto result_ptr = (uint8_t*)result.c_str();

   unsigned y=0;
   {
      unsigned i=0;
      uint32_t val = 0;
      for (; y < in.size(); y++) {
         if (y % 4 == 0 && y != 0) {
            result_ptr[i * 3] = (val & 0xFF000000) >> 24;
            result_ptr[i * 3 + 1] = (val & 0x00FF0000) >> 16;
            result_ptr[i * 3 + 2] = (val & 0x0000FF00) >> 8;
            val = 0;
            ++i;
         }

         auto val8 = ptr[y];
         auto iter = base64Vals.find(val8);
         if (iter == base64Vals.end()) {
            if (val8 == '=') {
               break;
            }
            throw std::runtime_error("invalid b64 character");
         }

         uint32_t bits = iter->second << (26 - (6 * (y % 4)));
         val |= bits;
      }

      result_ptr[i * 3] = (val & 0xFF000000) >> 24;
      result_ptr[i * 3 + 1] = (val & 0x00FF0000) >> 16;
      result_ptr[i * 3 + 2] = (val & 0x0000FF00) >> 8;
   }

   y *= 3;
   auto len = y / 4;
   result.resize(len);
   return result;
}

////////////////////////////////////////////////////////////////////////////////
// base58
std::string BtcUtils::scrAddrToBase58(const BinaryData& scrAddr)
{
   /***
   caller has to make sure the scrAddr is prepended with the version byte
   ***/

   //hash payload
   auto checksum = getHash256(scrAddr);

   //append first 4 bytes of hash to payload
   auto scriptNhash = scrAddr;
   scriptNhash.append(checksum.getSliceRef(0, 4));
   return base58_encode(scriptNhash);
}

BinaryData BtcUtils::base58toScrAddr(const std::string& b58Addr)
{
   //decode
   auto scriptNhash = base58_decode(b58Addr);

   //should be at least 4 bytes checksum + 1 version byte
   if (scriptNhash.getSize() <= 5) {
      throw std::range_error("invalid b58 decoded address length");
   }

   //split last 4 bytes
   auto len = scriptNhash.getSize();
   auto scriptRef = scriptNhash.getSliceRef(0, len - 4);

   auto checksumRef = scriptNhash.getSliceRef(len - 4, 4);
   auto scriptHash = getHash256(scriptRef);
   auto hash4First = scriptHash.getSliceRef(0, 4);

   if (checksumRef != hash4First) {
      throw std::runtime_error("invalid checksum in b58 address");
   }
   return BinaryData{scriptRef};
}

std::string BtcUtils::base58_encode(BinaryDataRef payload)
{
   size_t size = payload.getSize() * 138 / 100 + 2;
   std::string b58_str;
   b58_str.resize(size);
   size_t return_size = b58_str.size();
   if (!btc_base58_encode(
      &b58_str[0], &return_size,
      payload.getPtr(), payload.getSize()) || 
      return_size > size) {
      throw std::runtime_error("failed to encode b58 string");
   }

   if (return_size == 0) {
      throw std::runtime_error("failed to encode b58 string");
   }

   if (return_size - 1 < size) {
      b58_str.resize(return_size - 1);
   }
   return b58_str;
}

BinaryData BtcUtils::base58_decode(const std::string& b58)
{
   //sanity checks
   if (b58.empty()) {
      throw std::range_error("empty b58 string");
   }

   size_t size = b58.size();
   BinaryData result(size);

   if (!btc_base58_decode(result.getPtr(), &size, b58.c_str()) ||
      size > b58.size()) {
      throw std::runtime_error("failed to decode b58 string");
   }
   return result.getSliceCopy(b58.size() - size, size);
}

////
std::string BtcUtils::encodePrivKeyBase58(const SecureBinaryData& privKey)
{
   BinaryWriter bwPrivKey;
   bwPrivKey.put_uint8_t(Armory::Config::BitcoinSettings::getPrivKeyPrefix());
   bwPrivKey.put_BinaryData(privKey);
   bwPrivKey.put_uint8_t(0x01);

   auto checksum = getHash256(bwPrivKey.getData());
   bwPrivKey.put_BinaryDataRef(checksum.getSliceRef(0, 4));
   return base58_encode(bwPrivKey.getData());
}

SecureBinaryData BtcUtils::decodePrivKeyBase58(const std::string& strPrivKey)
{
   SecureBinaryData decodedKey(base58_decode(strPrivKey));
   BinaryRefReader brr(decodedKey.getRef());

   //prefix
   auto prefix = brr.get_uint8_t();
   if (prefix != Armory::Config::BitcoinSettings::getPrivKeyPrefix()) {
      throw std::runtime_error("network prefix mismatch");
   }
   brr.rewind(1);

   //key
   auto keyRef = brr.get_BinaryDataRef(brr.getSize() - 4);

   //checksum
   auto checksum = brr.get_BinaryDataRef(4);
   auto hash = BtcUtils::getHash256(keyRef);
   if (hash.getSliceRef(0, 4) != checksum) {
      throw std::runtime_error("privkey checksum mismatch");
   }
   return SecureBinaryData{keyRef.getSliceRef(1, 32)};
}

////////////////////////////////////////////////////////////////////////////////
// sw addresses
std::string BtcUtils::scrAddrToSegWitAddress(const BinaryData& scrAddr)
{
   //hardcoded for version 0 witness programs for now
   const char* headerPtr;
   if (Armory::Config::BitcoinSettings::getPubkeyHashPrefix() ==
      SCRIPT_PREFIX_HASH160) {
      headerPtr = SEGWIT_ADDRESS_MAINNET_HEADER;
   } else if (Armory::Config::BitcoinSettings::getPubkeyHashPrefix() ==
      SCRIPT_PREFIX_HASH160_TESTNET) {
      headerPtr = SEGWIT_ADDRESS_TESTNET_HEADER;
   } else {
      throw std::runtime_error("invalid network for segwit address");
   }

   //73 + header size
   std::string result; result.resize(75);
   if (segwit_addr_encode(
      (char*)result.c_str(), headerPtr, 0,
      scrAddr.getPtr(), scrAddr.getSize()) == 0) {
      throw std::runtime_error("failed to encode to sw address!");
   }

   //adjust result size by looking for the null terminator
   auto len = strnlen(result.c_str(), result.size());
   if (len == 0 || len == result.size()) {
      throw std::runtime_error("failed to encode to sw address!");
   }
   result.resize(len);
   return result;
}

std::pair<BinaryData, int> BtcUtils::segWitAddressToScrAddr(
   const std::string& swAddr)
{
   //hardcoded for version 0 witness programs for now
   const char* headerPtr;
   if (Armory::Config::BitcoinSettings::getPubkeyHashPrefix() ==
      SCRIPT_PREFIX_HASH160) {
      headerPtr = SEGWIT_ADDRESS_MAINNET_HEADER;
   } else if (Armory::Config::BitcoinSettings::getPubkeyHashPrefix() ==
      SCRIPT_PREFIX_HASH160_TESTNET) {
      headerPtr = SEGWIT_ADDRESS_TESTNET_HEADER;
   } else {
      throw std::runtime_error("invalid network for segwit address");
   }

   int ver;
   size_t len;
   BinaryData result(40);
   if (segwit_addr_decode(&ver, result.getPtr(), &len,
      headerPtr, swAddr.c_str()) == 0) {
      throw std::runtime_error("failed to decode sw address!");
   }

   if (len == 0) {
      throw std::runtime_error("empty sw program buffer");
   }
   if (ver != 0) {
      throw std::runtime_error("only supporting sw version 0 for now");
   }

   //resize result
   result.resize(len);
   return std::make_pair(result, ver);
}

/////////////////////////////////////////////////////////////////////////////
// misc
std::string BtcUtils::numToStrWCommas(int64_t fullNum)
{
   uint64_t num = fullNum;
   num *= (fullNum < 0 ? -1 : 1);
   std::vector<uint32_t> triplets;
   do {
      int bottom3 = (num % 1000);
      triplets.emplace_back(bottom3);
      num = (num - bottom3) / 1000;
   } while(num >= 1);

   std::stringstream out;
   out << (fullNum < 0 ? "-" : "");
   size_t nt = triplets.size()-1;
   char t[4];
   for (uint32_t i=0; i<=nt; i++) {
      if (i==0) {
         sprintf(t, "%d", triplets[nt-i]);
      } else {
         sprintf(t, "%03d", triplets[nt-i]);
      }
      out << t;

      if (i != nt) {
         out << ",";
      }
   }
   return out.str();
}

////
BinaryData BtcUtils::PackBits(const std::list<bool>& boolVec)
{
   BinaryData out((boolVec.size()+7) / 8);
   memset(out.getPtr(), 0, out.getSize());

   unsigned i=0;
   for (const auto& entry : boolVec) {
      if (entry == true) {
         out[i/8] |= (1<<(7-i%8));
      }
      ++i;
   }
   return out;
}

std::list<bool> BtcUtils::UnpackBits(const BinaryData& bits, uint32_t nBits)
{
   std::list<bool> out;
   for (unsigned i=0; i<nBits; i++) {
      uint8_t bit = bits[i/8] & (1 << (7-i%8));
      out.emplace_back(bit>0);
   }
   return out;
}

////
double BtcUtils::convertDiffBitsToDouble(const BinaryData& diffBitsBinary)
{
   uint32_t diffBits = READ_UINT32_LE(diffBitsBinary);
   int nShift = (diffBits >> 24) & 0xff;
   double dDiff = (double)0x0000ffff / (double)(diffBits & 0x00ffffff);

   while (nShift < 29) {
      dDiff *= 256.0;
      nShift++;
   }
   while (nShift > 29) {
      dDiff /= 256.0;
      nShift--;
   }
   return dDiff;
}

BinaryData BtcUtils::convertDoubleToDiffBits(double diff)
{
   //quick and dirty, for unit test reorg purposes
   unsigned nShift = 29;
   while (diff > 16777215.0) {
      diff /= 256.0;
      --nShift;
   }

   BinaryData diffBits(4);
   auto ptr = diffBits.getPtr();

   auto val = 65535.0 / diff;
   unsigned* bits = (unsigned*)ptr;
   *bits = ((unsigned)val);
   ptr[3] = nShift;

   return diffBits;
}

////
std::string BtcUtils::getOpCodeName(OPCODETYPE opcode)
{
   switch (opcode)
   {
      // push value
      case OP_0                     : return "OP_0";
      case OP_PUSHDATA1             : return "OP_PUSHDATA1";
      case OP_PUSHDATA2             : return "OP_PUSHDATA2";
      case OP_PUSHDATA4             : return "OP_PUSHDATA4";
      case OP_1NEGATE               : return "OP_1NEGATE";
      case OP_RESERVED              : return "OP_RESERVED";
      case OP_1                     : return "OP_1";
      case OP_2                     : return "OP_2";
      case OP_3                     : return "OP_3";
      case OP_4                     : return "OP_4";
      case OP_5                     : return "OP_5";
      case OP_6                     : return "OP_6";
      case OP_7                     : return "OP_7";
      case OP_8                     : return "OP_8";
      case OP_9                     : return "OP_9";
      case OP_10                    : return "OP_10";
      case OP_11                    : return "OP_11";
      case OP_12                    : return "OP_12";
      case OP_13                    : return "OP_13";
      case OP_14                    : return "OP_14";
      case OP_15                    : return "OP_15";
      case OP_16                    : return "OP_16";

      // control
      case OP_NOP                   : return "OP_NOP";
      case OP_VER                   : return "OP_VER";
      case OP_IF                    : return "OP_IF";
      case OP_NOTIF                 : return "OP_NOTIF";
      case OP_VERIF                 : return "OP_VERIF";
      case OP_VERNOTIF              : return "OP_VERNOTIF";
      case OP_ELSE                  : return "OP_ELSE";
      case OP_ENDIF                 : return "OP_ENDIF";
      case OP_VERIFY                : return "OP_VERIFY";
      case OP_RETURN                : return "OP_RETURN";

      // stack ops
      case OP_TOALTSTACK            : return "OP_TOALTSTACK";
      case OP_FROMALTSTACK          : return "OP_FROMALTSTACK";
      case OP_2DROP                 : return "OP_2DROP";
      case OP_2DUP                  : return "OP_2DUP";
      case OP_3DUP                  : return "OP_3DUP";
      case OP_2OVER                 : return "OP_2OVER";
      case OP_2ROT                  : return "OP_2ROT";
      case OP_2SWAP                 : return "OP_2SWAP";
      case OP_IFDUP                 : return "OP_IFDUP";
      case OP_DEPTH                 : return "OP_DEPTH";
      case OP_DROP                  : return "OP_DROP";
      case OP_DUP                   : return "OP_DUP";
      case OP_NIP                   : return "OP_NIP";
      case OP_OVER                  : return "OP_OVER";
      case OP_PICK                  : return "OP_PICK";
      case OP_ROLL                  : return "OP_ROLL";
      case OP_ROT                   : return "OP_ROT";
      case OP_SWAP                  : return "OP_SWAP";
      case OP_TUCK                  : return "OP_TUCK";

      // splice ops
      case OP_CAT                   : return "OP_CAT";
      case OP_SUBSTR                : return "OP_SUBSTR";
      case OP_LEFT                  : return "OP_LEFT";
      case OP_RIGHT                 : return "OP_RIGHT";
      case OP_SIZE                  : return "OP_SIZE";

      // bit logic
      case OP_INVERT                : return "OP_INVERT";
      case OP_AND                   : return "OP_AND";
      case OP_OR                    : return "OP_OR";
      case OP_XOR                   : return "OP_XOR";
      case OP_EQUAL                 : return "OP_EQUAL";
      case OP_EQUALVERIFY           : return "OP_EQUALVERIFY";
      case OP_RESERVED1             : return "OP_RESERVED1";
      case OP_RESERVED2             : return "OP_RESERVED2";

      // numeric
      case OP_1ADD                  : return "OP_1ADD";
      case OP_1SUB                  : return "OP_1SUB";
      case OP_2MUL                  : return "OP_2MUL";
      case OP_2DIV                  : return "OP_2DIV";
      case OP_NEGATE                : return "OP_NEGATE";
      case OP_ABS                   : return "OP_ABS";
      case OP_NOT                   : return "OP_NOT";
      case OP_0NOTEQUAL             : return "OP_0NOTEQUAL";
      case OP_ADD                   : return "OP_ADD";
      case OP_SUB                   : return "OP_SUB";
      case OP_MUL                   : return "OP_MUL";
      case OP_DIV                   : return "OP_DIV";
      case OP_MOD                   : return "OP_MOD";
      case OP_LSHIFT                : return "OP_LSHIFT";
      case OP_RSHIFT                : return "OP_RSHIFT";
      case OP_BOOLAND               : return "OP_BOOLAND";
      case OP_BOOLOR                : return "OP_BOOLOR";
      case OP_NUMEQUAL              : return "OP_NUMEQUAL";
      case OP_NUMEQUALVERIFY        : return "OP_NUMEQUALVERIFY";
      case OP_NUMNOTEQUAL           : return "OP_NUMNOTEQUAL";
      case OP_LESSTHAN              : return "OP_LESSTHAN";
      case OP_GREATERTHAN           : return "OP_GREATERTHAN";
      case OP_LESSTHANOREQUAL       : return "OP_LESSTHANOREQUAL";
      case OP_GREATERTHANOREQUAL    : return "OP_GREATERTHANOREQUAL";
      case OP_MIN                   : return "OP_MIN";
      case OP_MAX                   : return "OP_MAX";
      case OP_WITHIN                : return "OP_WITHIN";

      // crypto
      case OP_RIPEMD160             : return "OP_RIPEMD160";
      case OP_SHA1                  : return "OP_SHA1";
      case OP_SHA256                : return "OP_SHA256";
      case OP_HASH160               : return "OP_HASH160";
      case OP_HASH256               : return "OP_HASH256";
      case OP_CODESEPARATOR         : return "OP_CODESEPARATOR";
      case OP_CHECKSIG              : return "OP_CHECKSIG";
      case OP_CHECKSIGVERIFY        : return "OP_CHECKSIGVERIFY";
      case OP_CHECKMULTISIG         : return "OP_CHECKMULTISIG";
      case OP_CHECKMULTISIGVERIFY   : return "OP_CHECKMULTISIGVERIFY";
   
      // expanson
      case OP_NOP1                  : return "OP_NOP1";
      case OP_NOP2                  : return "OP_NOP2";
      case OP_NOP3                  : return "OP_NOP3";
      case OP_NOP4                  : return "OP_NOP4";
      case OP_NOP5                  : return "OP_NOP5";
      case OP_NOP6                  : return "OP_NOP6";
      case OP_NOP7                  : return "OP_NOP7";
      case OP_NOP8                  : return "OP_NOP8";
      case OP_NOP9                  : return "OP_NOP9";
      case OP_NOP10                 : return "OP_NOP10";

      // template matching params
      case OP_PUBKEYHASH            : return "OP_PUBKEYHASH";
      case OP_PUBKEY                : return "OP_PUBKEY";

      case OP_INVALIDOPCODE         : return "OP_INVALIDOPCODE";
      default:
         return "OP_UNKNOWN";
   }
}

std::vector<std::string> BtcUtils::convertScriptToOpStrings(
   const BinaryData& script)
{
   std::list<std::string> opList;

   uint32_t i = 0;
   size_t sz=script.getSize();
   bool error=false;
   while (i < sz) {
      uint8_t nextOp = script[i];
      if (nextOp == 0) {
         opList.push_back("OP_0");
         i++;
      } else if (nextOp < 76) {
         opList.push_back("[PUSHDATA -- " + std::to_string(nextOp) + " BYTES:]");
         opList.push_back(script.getSliceCopy(i+1, nextOp).toHexStr());
         i += nextOp+1;
      } else if (nextOp == 76) {
         uint8_t nb = READ_UINT8_LE(script.getPtr() + i+1);
         if (i+1+1+nb > sz) {
            error=true;
            break;
         }
         BinaryData binObj = script.getSliceCopy(i+2, nb);
         opList.push_back("[OP_PUSHDATA1 -- " + std::to_string(nb) + " BYTES:]");
         opList.push_back(binObj.toHexStr());
         i += nb+2;
      } else if (nextOp == 77) {
         uint16_t nb = READ_UINT16_LE(script.getPtr() + i+1);
         if (i+1+2+nb > sz) {
            error=true; break;
         }
         BinaryData binObj = script.getSliceCopy(i+3, std::min((int)nb,256));
         opList.push_back("[OP_PUSHDATA2 -- " + std::to_string(nb) + " BYTES:]");
         opList.push_back(binObj.toHexStr() + "...");
         i += nb+3;
      } else if (nextOp == 78) {
         uint32_t nb = READ_UINT32_LE(script.getPtr() + i+1);
         if (i+1+4+nb > sz) {
            error=true;
            break;
         }
         BinaryData binObj = script.getSliceCopy(i+5, std::min((int)nb,256));
         opList.push_back("[OP_PUSHDATA4 -- " + std::to_string(nb) + " BYTES:]");
         opList.push_back(binObj.toHexStr() + "...");
         i += nb+5;
      } else {
         opList.push_back(getOpCodeName((OPCODETYPE)nextOp));
         i++;
      }
   }

   if (error) {
      opList.clear();
      opList.push_back("ERROR PROCESSING SCRIPT");
   }

   size_t nops = opList.size();
   std::vector<std::string> vectOut(nops);
   std::list<std::string>::iterator iter;
   uint32_t op=0;
   for (iter = opList.begin(); iter != opList.end(); iter++) {
      vectOut[op] = *iter;
      op++;
   }
   return vectOut;
}

void BtcUtils::pprintScript(const BinaryData& script)
{
   std::vector<std::string> oplist = convertScriptToOpStrings(script);
   for (uint32_t i=0; i < oplist.size(); i++) {
      std::cout << "   " << oplist[i] << std::endl;
   }
}

////
BinaryData BtcUtils::extractRSFromDERSig(BinaryDataRef bdr)
{
   auto forceTo32Bytes = [](BinaryDataRef data, BinaryWriter& output)->void
   {
      auto len = data.getSize();
      if (len > 32) {
         output.put_BinaryData(data.getSliceRef(len - 32, 32));
      } else {
         int zeroCount = 32 - len;
         while (zeroCount-- > 0) {
            output.put_uint8_t(0);
         }
         output.put_BinaryData(data);
      }
   };

   BinaryWriter output;
   BinaryRefReader brr(bdr);

   //check code byte
   auto codeByte = brr.get_uint8_t();
   if (codeByte != 0x30) {
      throw DERException("unexpected code byte in DER sig");
   }

   auto len = brr.get_uint8_t();

   //onto R, again check code byte
   codeByte = brr.get_uint8_t();
   len = brr.get_uint8_t();
   if (codeByte != 0x02) {
      throw DERException("unexpected code byte in DER sig");
   }

   //grab R
   auto rRef = brr.get_BinaryDataRef(len);

   //force to 32 bytes length
   forceTo32Bytes(rRef, output);

   //S
   codeByte = brr.get_uint8_t();
   len = brr.get_uint8_t();
   if (codeByte != 0x02) {
      throw DERException("unexpected code byte in DER sig");
   }

   //grab S
   auto sRef = brr.get_BinaryDataRef(len);

   //force to 32 bytes length
   forceTo32Bytes(sRef, output);

   return output.getData();
}

////
std::map<BinaryDataRef, BinaryDataRef> BtcUtils::getPSBTDataPairs(
   BinaryRefReader& brr)
{
   std::map<BinaryDataRef, BinaryDataRef> result;
   while (true) {
      auto keylen = brr.get_var_int();
      if (keylen == 0) {
         break;
      }
      auto key = brr.get_BinaryDataRef(keylen);

      auto vallen = brr.get_var_int();
      auto val = brr.get_BinaryDataRef(vallen);

      result.emplace(key, val);
   }
   return result;
}
