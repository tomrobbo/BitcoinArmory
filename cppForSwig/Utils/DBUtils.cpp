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

#include "DBUtils.h"
#include "BinaryData.h"

using namespace Armory;
using namespace Armory::DBUtils;

const BinaryData DBUtils::ZCPrefix = BinaryData::CreateFromHex("FFFF");

////////////////////////////////////////////////////////////////////////////////
// DBUtils
BLKDATA_TYPE DBUtils::readBlkDataKey(BinaryRefReader& brr,
   uint32_t& height, uint8_t& dupID)
{
   uint16_t tempTxIdx;
   uint16_t tempTxOutIdx;
   return readBlkDataKey(brr, height, dupID, tempTxIdx, tempTxOutIdx);
}

BLKDATA_TYPE DBUtils::readBlkDataKey(BinaryRefReader& brr,
   uint32_t& height, uint8_t& dupID, uint16_t& txIdx)
{
   uint16_t tempTxOutIdx;
   return readBlkDataKey(brr, height, dupID, txIdx, tempTxOutIdx);
}

BLKDATA_TYPE DBUtils::readBlkDataKey(BinaryRefReader & brr,
   uint32_t& height, uint8_t& dupID, uint16_t& txIdx, uint16_t& txOutIdx)
{
   uint8_t prefix = brr.get_uint8_t();
   if (prefix != (uint8_t)DbPrefix::TXDATA) {
      height = 0xffffffff;
      dupID = 0xff;
      txIdx = 0xffff;
      txOutIdx = 0xffff;
      return BLKDATA_TYPE::Invalid;
   }
   return readBlkDataKeyNoPrefix(brr, height, dupID, txIdx, txOutIdx);
}

BLKDATA_TYPE DBUtils::readBlkDataKeyNoPrefix(BinaryRefReader& brr,
   uint32_t& height, uint8_t& dupID)
{
   uint16_t tempTxIdx;
   uint16_t tempTxOutIdx;
   return readBlkDataKeyNoPrefix(brr, height, dupID, tempTxIdx, tempTxOutIdx);
}

BLKDATA_TYPE DBUtils::readBlkDataKeyNoPrefix(
   BinaryRefReader& brr, uint32_t& height, uint8_t& dupID, uint16_t& txIdx)
{
   uint16_t tempTxOutIdx;
   return readBlkDataKeyNoPrefix(brr, height, dupID, txIdx, tempTxOutIdx);
}

BLKDATA_TYPE DBUtils::readBlkDataKeyNoPrefix(BinaryRefReader & brr,
   uint32_t& height, uint8_t& dupID, uint16_t& txIdx, uint16_t& txOutIdx)
{
   BinaryData hgtx = brr.get_BinaryData(4);
   height = hgtxToHeight(hgtx);
   dupID = hgtxToDupID(hgtx);

   if (brr.getSizeRemaining() == 0) {
      txIdx = 0xffff;
      txOutIdx = 0xffff;
      return BLKDATA_TYPE::Header;
   } else if (brr.getSizeRemaining() == 2) {
      txIdx = brr.get_uint16_t(BE);
      txOutIdx = 0xffff;
      return BLKDATA_TYPE::Tx;
   } else if (brr.getSizeRemaining() == 4) {
      txIdx = brr.get_uint16_t(BE);
      txOutIdx = brr.get_uint16_t(BE);
      return BLKDATA_TYPE::TxOut;
   } else {
      LOGERR << "Unexpected bytes remaining: " << brr.getSizeRemaining();
      return BLKDATA_TYPE::Invalid;
   }
}

////////////////////////////////////////////////////////////////////////////////
std::string DBUtils::getPrefixName(DbPrefix pref)
{
   switch (pref)
   {
      case DbPrefix::DBINFO:    return {"DBINFO"};
      case DbPrefix::TXDATA:    return {"TXDATA"};
      case DbPrefix::SCRIPT:    return {"SCRIPT"};
      case DbPrefix::TXHINTS:   return {"TXHINTS"};
      case DbPrefix::TRIENODES: return {"TRIENODES"};
      case DbPrefix::HEADHASH:  return {"HEADHASH"};
      case DbPrefix::HEADHGT:   return {"HEADHGT"};
      case DbPrefix::UNDODATA:  return {"UNDODATA"};
      default:                  return {"<unknown>"};
   }
}

bool DBUtils::checkPrefixByteWError(BinaryRefReader& brr,
   DbPrefix prefix, bool rewindWhenDone)
{
   auto oneByte = (DbPrefix)brr.get_uint8_t();
   bool out;
   if (oneByte == prefix) {
      out = true;
   } else {
      LOGERR << "Unexpected prefix byte: "
         << "Expected: " << getPrefixName(prefix)
         << "Received: " << getPrefixName(oneByte);
      out = false;
   }

   if (rewindWhenDone) {
      brr.rewind(1);
   }
   return out;
}

bool DBUtils::checkPrefixByte(BinaryRefReader& brr,
   DbPrefix prefix, bool rewindWhenDone)
{
   uint8_t oneByte = brr.get_uint8_t();
   bool out = (oneByte == (uint8_t)prefix);

   if (rewindWhenDone) {
      brr.rewind(1);
   }
   return out;
}

/////////////////////////////////////////////////////////////////////////////
BinaryData DBUtils::getBlkDataKey(uint32_t height, uint8_t dup)
{
   BinaryWriter bw(5);
   bw.put_uint8_t((uint8_t)DbPrefix::TXDATA);
   bw.put_BinaryData(heightAndDupToHgtx(height, dup));
   return bw.getData();
}

BinaryData DBUtils::getBlkDataKey(uint32_t height,
   uint8_t dup, uint16_t txIdx)
{
   BinaryWriter bw(7);
   bw.put_uint8_t((uint8_t)DbPrefix::TXDATA);
   bw.put_BinaryData(heightAndDupToHgtx(height, dup));
   bw.put_uint16_t(txIdx, BE);
   return bw.getData();
}

BinaryData DBUtils::getBlkDataKey(uint32_t height,
   uint8_t dup, uint16_t txIdx, uint16_t txOutIdx)
{
   BinaryWriter bw(9);
   bw.put_uint8_t((uint8_t)DbPrefix::TXDATA);
   bw.put_BinaryData(heightAndDupToHgtx(height, dup));
   bw.put_uint16_t(txIdx, BE);
   bw.put_uint16_t(txOutIdx, BE);
   return bw.getData();
}

BinaryData DBUtils::getBlkDataKeyNoPrefix(uint32_t height, uint8_t dup)
{
   return heightAndDupToHgtx(height, dup);
}

BinaryData DBUtils::getBlkDataKeyNoPrefix(uint32_t height,
   uint8_t dup, uint16_t txIdx)
{
   BinaryWriter bw(6);
   bw.put_BinaryData(heightAndDupToHgtx(height, dup));
   bw.put_uint16_t(txIdx, BE);
   return bw.getData();
}

BinaryData DBUtils::getBlkDataKeyNoPrefix(uint32_t height,
   uint8_t dup, uint16_t txIdx, uint16_t txOutIdx)
{
   BinaryWriter bw(8);
   bw.put_BinaryData(heightAndDupToHgtx(height, dup));
   bw.put_uint16_t(txIdx, BE);
   bw.put_uint16_t(txOutIdx, BE);
   return bw.getData();
}

BinaryData DBUtils::getDBSuperSpentnessKey(uint32_t height,
   uint8_t dup, uint16_t txIdx, uint16_t txOutIdx)
{
   return DBUtils::getBlkDataKeyNoPrefix(
      UINT32_MAX - height, dup, txIdx, txOutIdx);
}

/////////////////////////////////////////////////////////////////////////////
uint32_t DBUtils::hgtxToHeight(const BinaryData& hgtx)
{
   return (READ_UINT32_BE(hgtx) >> 8);
}

uint8_t DBUtils::hgtxToDupID(const BinaryData& hgtx)
{
   return (READ_UINT32_BE(hgtx) & 0x7f);
}

BinaryData DBUtils::heightAndDupToHgtx(uint32_t hgt, uint8_t dup)
{
   uint32_t hgtxInt = (hgt << 8) | (uint32_t)dup;
   return WRITE_UINT32_BE(hgtxInt);
}

bool DBUtils::keyIsZC(BinaryDataRef key)
{
   if (key.getSize() < 4) {
      return false;
   }
   uint16_t* keyInt = (uint16_t*)key.getPtr();
   return *keyInt == 0xFFFF;
}

/////////////////////////////////////////////////////////////////////////////
BinaryData DBUtils::getFilterPoolKey(uint32_t filenum)
{
   uint32_t bucketKey = (uint32_t(DbPrefix::POOL) << 24) | (uint32_t)filenum;
   return WRITE_UINT32_BE(bucketKey);
}

BinaryData DBUtils::getMissingHashesKey(uint32_t id)
{
   BinaryData bd;
   bd.resize(4);

   id &= 0x00FFFFFF; //24bit ids top
   id |= uint32_t(DbPrefix::MISSING_HASHES) << 24;

   auto keyPtr = (uint32_t*)bd.getPtr();
   *keyPtr = id;
   return bd;
}

////////////////////////////////////////////////////////////////////////////////
BinaryDataRef DBUtils::getDataRefForPacket(
   const BinaryDataRef& packet)
{
   BinaryRefReader brr(packet);
   auto len = brr.get_var_int();
   if (len != brr.getSizeRemaining()) {
      throw std::runtime_error("on disk data length mismatch");
   }
   return brr.get_BinaryDataRef(brr.getSizeRemaining());
}
