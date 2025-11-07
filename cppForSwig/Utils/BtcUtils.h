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

#ifndef _BTCUTILS_H_
#define _BTCUTILS_H_

#include <vector>
#include <list>
#include <string>
#include <map>
#include <stdexcept>
#include <stdint.h>

#define HEADER_SIZE 80
#define COIN 100000000ULL
#define NBLOCKS_REGARDED_AS_RESCAN 144
#define MIN_CONFIRMATIONS   6

#define COINBASE_MATURITY 100

#define TX_0_UNCONFIRMED    0
#define TX_NOT_EXIST       -1
#define TX_OFF_MAIN_BRANCH -2

#define HashString     BinaryData
#define HashStringRef  BinaryDataRef

#define HASH160PREFIX WRITE_UINT8_LE((uint8_t)SCRIPT_PREFIX_HASH160)
#define P2SHPREFIX    WRITE_UINT8_LE((uint8_t)SCRIPT_PREFIX_P2SH)
#define MSIGPREFIX    WRITE_UINT8_LE((uint8_t)SCRIPT_PREFIX_MULTISIG)
#define NONSTDPREFIX  WRITE_UINT8_LE((uint8_t)SCRIPT_PREFIX_NONSTD)

// Really, these defs are just for making it painfully clear in the
// code what you are attempting to compare.  I'm constantly messing
// up == and != when trying to read through the code.
#define  KEY_NOT_IN_MAP(KEY,MAP)  (MAP.find(KEY) == MAP.end())
#define      KEY_IN_MAP(KEY,MAP)  (MAP.find(KEY) != MAP.end())
#define ITER_NOT_IN_MAP(ITER,MAP) (ITER == MAP.end())
#define     ITER_IN_MAP(ITER,MAP) (ITER != MAP.end())

class BinaryData;
class BinaryDataRef;
class SecureBinaryData;
class BinaryRefReader;

typedef enum
{
   TXOUT_SCRIPT_STDHASH160=0,
   TXOUT_SCRIPT_STDPUBKEY65,
   TXOUT_SCRIPT_STDPUBKEY33,
   TXOUT_SCRIPT_MULTISIG,
   TXOUT_SCRIPT_P2SH,
   TXOUT_SCRIPT_NONSTANDARD,
   TXOUT_SCRIPT_P2WPKH,
   TXOUT_SCRIPT_P2WSH,
   TXOUT_SCRIPT_OPRETURN
}  TXOUT_SCRIPT_TYPE;

typedef enum
{
   TXIN_SCRIPT_STDUNCOMPR,
   TXIN_SCRIPT_STDCOMPR,
   TXIN_SCRIPT_COINBASE,
   TXIN_SCRIPT_SPENDPUBKEY,
   TXIN_SCRIPT_SPENDMULTI,
   TXIN_SCRIPT_SPENDP2SH,
   TXIN_SCRIPT_NONSTANDARD,
   TXIN_SCRIPT_WITNESS,
   TXIN_SCRIPT_P2WPKH_P2SH,
   TXIN_SCRIPT_P2WSH_P2SH
}  TXIN_SCRIPT_TYPE;

enum OPCODETYPE
{
   // push value
   OP_0=0,
   OP_FALSE=OP_0,
   OP_PUSHDATA1=76,
   OP_PUSHDATA2,
   OP_PUSHDATA4,
   OP_1NEGATE,
   OP_RESERVED,
   OP_1,
   OP_TRUE=OP_1,
   OP_2,
   OP_3,
   OP_4,
   OP_5,
   OP_6,
   OP_7,
   OP_8,
   OP_9,
   OP_10,
   OP_11,
   OP_12,
   OP_13,
   OP_14,
   OP_15,
   OP_16,

   // control
   OP_NOP,
   OP_VER,
   OP_IF,
   OP_NOTIF,
   OP_VERIF,
   OP_VERNOTIF,
   OP_ELSE,
   OP_ENDIF,
   OP_VERIFY,
   OP_RETURN,

   // stack ops
   OP_TOALTSTACK,
   OP_FROMALTSTACK,
   OP_2DROP,
   OP_2DUP,
   OP_3DUP,
   OP_2OVER,
   OP_2ROT,
   OP_2SWAP,
   OP_IFDUP,
   OP_DEPTH,
   OP_DROP,
   OP_DUP,
   OP_NIP,
   OP_OVER,
   OP_PICK,
   OP_ROLL,
   OP_ROT,
   OP_SWAP,
   OP_TUCK,

   // splice ops
   OP_CAT,
   OP_SUBSTR,
   OP_LEFT,
   OP_RIGHT,
   OP_SIZE,

   // bit logic
   OP_INVERT,
   OP_AND,
   OP_OR,
   OP_XOR,
   OP_EQUAL,
   OP_EQUALVERIFY,
   OP_RESERVED1,
   OP_RESERVED2,

   // numeric
   OP_1ADD,
   OP_1SUB,
   OP_2MUL,
   OP_2DIV,
   OP_NEGATE,
   OP_ABS,
   OP_NOT,
   OP_0NOTEQUAL,

   OP_ADD,
   OP_SUB,
   OP_MUL,
   OP_DIV,
   OP_MOD,
   OP_LSHIFT,
   OP_RSHIFT,

   OP_BOOLAND,
   OP_BOOLOR,
   OP_NUMEQUAL,
   OP_NUMEQUALVERIFY,
   OP_NUMNOTEQUAL,
   OP_LESSTHAN,
   OP_GREATERTHAN,
   OP_LESSTHANOREQUAL,
   OP_GREATERTHANOREQUAL,
   OP_MIN,
   OP_MAX,
   OP_WITHIN,

   // crypto
   OP_RIPEMD160,
   OP_SHA1,
   OP_SHA256,
   OP_HASH160,
   OP_HASH256,
   OP_CODESEPARATOR,
   OP_CHECKSIG,
   OP_CHECKSIGVERIFY,
   OP_CHECKMULTISIG,
   OP_CHECKMULTISIGVERIFY,

   // expansion
   OP_NOP1,
   OP_NOP2,
   OP_NOP3,
   OP_NOP4,
   OP_NOP5,
   OP_NOP6,
   OP_NOP7,
   OP_NOP8,
   OP_NOP9,
   OP_NOP10,

   // template matching params
   OP_PUBKEYHASH = 0xfd,
   OP_PUBKEY = 0xfe,

   OP_INVALIDOPCODE = 0xff,
};

class BlockDeserializingException : public std::runtime_error
{
public:
   BlockDeserializingException(const std::string& = "");
};

class VarIntException : public BlockDeserializingException
{
public:
   VarIntException(const std::string& = "");
};

class DERException : public std::runtime_error
{
public:
   DERException(const std::string& = "");
};

#define BIP32_SER_VERSION_MAIN_PRV 0x0488ADE4
#define BIP32_SER_VERSION_MAIN_PUB 0x0488B21E
#define BIP32_SER_VERSION_TEST_PRV 0x043587CF
#define BIP32_SER_VERSION_TEST_PUB 0x04358394

// This class holds only static methods.
// NOTE:  added default ctor and a few non-static, to support SWIG
//        (-classic SWIG doesn't support static methods)
struct TxOutScriptRef;

namespace BtcUtils
{
   extern const BinaryData BadAddress;
   extern const char base64Chars[];
   extern const std::map<char, uint8_t> base64Vals;
   extern const BinaryData EmptyHash;

   /////////////////////////////////////////////////////////////////////////////
   uint64_t readVarInt(const uint8_t*, size_t, uint8_t&);
   std::pair<uint64_t, uint8_t> readVarInt(BinaryRefReader&);
   uint8_t readVarIntLength(const uint8_t*);
   uint8_t calcVarIntSize(const uint64_t&);

   /////////////////////////////////////////////////////////////////////////////
   std::string numToStrWCommas(int64_t);
   BinaryData PackBits(const std::list<bool>&);
   std::list<bool> UnpackBits(const BinaryData&, uint32_t);

   /////////////////////////////////////////////////////////////////////////////
   void getSha256(const uint8_t*, size_t, BinaryData&);
   BinaryData getSha256(const BinaryData&);

   void getHash256(const uint8_t* , size_t, BinaryData&);
   BinaryData getHash256(const uint8_t*, size_t);
   void getHash256(const BinaryData&, BinaryData&);
   void getHash256(BinaryDataRef, BinaryData&);
   BinaryData getHash256(const BinaryData&);
   BinaryData getHash256(const BinaryDataRef&);

   void getHash160(const uint8_t*, size_t, BinaryData&);
   BinaryData getHash160(const uint8_t*, size_t);
   void getHash160(BinaryDataRef, BinaryData&);
   BinaryData getHash160(const BinaryDataRef&);
   BinaryData getHash160(const BinaryData&);
   BinaryData ripemd160(const BinaryData&);

   /////////////////////////////////////////////////////////////////////////////
   BinaryData calculateMerkleRoot(const std::vector<BinaryData>&);
   std::vector<BinaryData> calculateMerkleTree(const std::vector<BinaryData>&);

   /////////////////////////////////////////////////////////////////////////////
   // ALL THESE METHODS ASSUME THERE IS A FULL TX/TXIN/TXOUT BEHIND THE PTR
   // The point of these methods is to calculate the length of the object,
   // hence we don't know in advance how big the object actually will be, so
   // we can't provide it as an input for safety checking...
   void TxInCalcLength(const uint8_t*, size_t, std::vector<size_t>*);
   size_t TxInCalcLength(const uint8_t*, size_t);
   size_t TxOutCalcLength(const uint8_t*, size_t);
   size_t TxWitnessCalcLength(const uint8_t*, size_t);
   bool checkSwMarker(const uint8_t*);
   size_t TxCalcLength(const uint8_t*, size_t,
      std::vector<size_t>*, std::vector<size_t>*, std::vector<size_t>*);
   size_t StoredTxCalcLength(const uint8_t*, size_t, bool,
      std::vector<size_t>*, std::vector<size_t>*, std::vector<size_t>*);

   /////////////////////////////////////////////////////////////////////////////
   // TXOUT_SCRIPT_STDHASH160,
   // TXOUT_SCRIPT_STDPUBKEY65,
   // TXOUT_SCRIPT_STDPUBKEY33,
   // TXOUT_SCRIPT_MULTISIG,
   // TXOUT_SCRIPT_P2SH,
   // TXOUT_SCRIPT_NONSTANDARD,
   // TXOUT_SCRIPT_P2WPKH,
   // TXOUT_SCRIPT_P2WSH,
   // TXOUT_SCRIPT_OPRETURN
   TXOUT_SCRIPT_TYPE getTxOutScriptType(BinaryDataRef);

   /////////////////////////////////////////////////////////////////////////////
   // TXIN_SCRIPT_STDUNCOMPR
   // TXIN_SCRIPT_STDCOMPR
   // TXIN_SCRIPT_COINBASE
   // TXIN_SCRIPT_SPENDPUBKEY
   // TXIN_SCRIPT_SPENDMULTI
   // TXIN_SCRIPT_SPENDP2SH
   // TXIN_SCRIPT_NONSTANDARD
   TXIN_SCRIPT_TYPE getTxInScriptType(BinaryDataRef, BinaryDataRef);

   /////////////////////////////////////////////////////////////////////////////
   BinaryData getTxOutRecipientAddr(const BinaryDataRef&,
      TXOUT_SCRIPT_TYPE=TXOUT_SCRIPT_NONSTANDARD);

   // We use this for LevelDB keys, to return same key if the same priv/pub
   // pair is used, and also saving a few bytes for common script types
   BinaryData getTxOutScrAddr(BinaryDataRef,
      TXOUT_SCRIPT_TYPE=TXOUT_SCRIPT_NONSTANDARD);
   TxOutScriptRef getTxOutScrAddrNoCopy(BinaryDataRef);
   BinaryData getTxOutScriptForScrAddr(BinaryDataRef);
   TXOUT_SCRIPT_TYPE getScriptTypeForScrAddr(BinaryDataRef);
   std::string getAddressStrFromScrAddr(BinaryDataRef);
   BinaryData getScrAddrForAddrStr(const std::string&);

   /////////////////////////////////////////////////////////////////////////////
   //        "UniqueKey"=="ScrAddr" minus prefix
   // TODO:  Interesting exercise: is there a non-standard script that could
   //        look like the output of this function operating on a multisig
   //        script (doesn't matter if it's valid or not)?  In other words, is
   //        there a hole where someone could mine a script that would be
   //        forwarded by Bitcoin Core to this code, which would then produce
   //        a non-std-unique-key that would be indistinguishable from the
   //        output of this function? My guess is, no. And my guess is that
   //        it's not a very useful even if it did. But it would be good to
   //        rule it out.
   BinaryData getMultisigUniqueKey(const BinaryData&);
   bool isMultisigScript(BinaryDataRef);

   // Returns M in M-of-N.  Use addr160List.size() for N. Output is sorted.
   uint8_t getMultisigAddrList(const BinaryData&, std::vector<BinaryData>&);

   // Returns M in M-of-N.  Use pkList.size() for N. Output is sorted.
   uint8_t getMultisigPubKeyList(const BinaryData&, std::vector<BinaryData>&);

   /////////////////////////////////////////////////////////////////////////////
   // TXIN_SCRIPT_STDUNCOMPR,
   // TXIN_SCRIPT_STDCOMPR,
   // TXIN_SCRIPT_COINBASE,
   // TXIN_SCRIPT_SPENDPUBKEY,
   // TXIN_SCRIPT_SPENDMULTI,
   // TXIN_SCRIPT_SPENDP2SH,
   // TXIN_SCRIPT_NONSTANDARD
   BinaryData getTxInAddr(BinaryDataRef, BinaryDataRef,
      TXIN_SCRIPT_TYPE=TXIN_SCRIPT_NONSTANDARD);
   BinaryData getTxInAddrFromType(BinaryDataRef, TXIN_SCRIPT_TYPE);
   BinaryData getTxInAddrFromTypeInt(const BinaryData&, uint32_t);

   /////////////////////////////////////////////////////////////////////////////
   std::vector<BinaryDataRef> splitPushOnlyScriptRefs(BinaryDataRef);
   std::vector<BinaryData> splitPushOnlyScript(const BinaryData&);
   BinaryData getLastPushDataInScript(const BinaryData&);
   BinaryData getPushDataHeader(const BinaryData&);

   /////////////////////////////////////////////////////////////////////////////
   double convertDiffBitsToDouble(const BinaryData&);
   BinaryData convertDoubleToDiffBits(double);

   /////////////////////////////////////////////////////////////////////////////
   std::string getOpCodeName(OPCODETYPE);
   std::vector<std::string> convertScriptToOpStrings(const BinaryData&);
   void pprintScript(const BinaryData&);

   /////////////////////////////////////////////////////////////////////////////
   std::string scrAddrToBase58(const BinaryData&);
   BinaryData base58toScrAddr(const std::string&);

   std::string encodePrivKeyBase58(const SecureBinaryData&);
   SecureBinaryData decodePrivKeyBase58(const std::string&);

   std::string base58_encode(BinaryDataRef);
   BinaryData base58_decode(const std::string&);
   std::string base64_encode(const std::string&);
   std::string base64_decode(const std::string&);

   /////////////////////////////////////////////////////////////////////////////
   BinaryData getHMAC256(const SecureBinaryData&, const SecureBinaryData&);
   BinaryData getHMAC512(const SecureBinaryData&, const SecureBinaryData&);
   BinaryData getHMAC256(const BinaryData&, const std::string&);
   BinaryData getHMAC512(const BinaryData&, const std::string&);
   SecureBinaryData getHMAC512(const std::string&, const SecureBinaryData&);
   BinaryData getBotchedArmoryHMAC256(const BinaryData&, const BinaryData&);

   void getHMAC256(const uint8_t*, size_t, const char*, size_t, uint8_t*);
   void getHMAC512(const void*, size_t, const void*, size_t, void*);

   /////////////////////////////////////////////////////////////////////////////
   SecureBinaryData computeChainCode_ArmoryLegacy(const SecureBinaryData&);

   BinaryData computeDataId(const SecureBinaryData&, const std::string&);

   /////////////////////////////////////////////////////////////////////////////
   BinaryData getP2PKHScript(const BinaryData&);
   BinaryData getP2PKScript(const BinaryData&);
   BinaryData getP2SHScript(const BinaryData&);
   BinaryData getP2WPKHOutputScript(const BinaryData&);
   BinaryData getP2WPKHWitnessScript(const BinaryData&);
   BinaryData getP2WSHOutputScript(const BinaryData&);
   BinaryData getP2WSHWitnessScript(const BinaryData&);

   ////
   std::string scrAddrToSegWitAddress(const BinaryData&);
   std::pair<BinaryData, int> segWitAddressToScrAddr(const std::string&);

   BinaryData extractRSFromDERSig(BinaryDataRef);

   /////////////////////////////////////////////////////////////////////////////
   std::map<BinaryDataRef, BinaryDataRef> getPSBTDataPairs(BinaryRefReader&);
};

#endif
