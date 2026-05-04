////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2026, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include <cstring>

#include "BlockchainData.h"
#include <Utils/ArmoryConfig.h>
#include <Utils/DBUtils.h>
#include <Utils/BCTX.h>
#include <Utils/varint.h>
#include <Utils/BinaryData.h>
#include <TxClasses.h>

#include "Blockchain.h"
#include "BlockDataMap.h"

using namespace Armory;

////////////////////////////////////////////////////////////////////////////////
// exceptions
BlockchainDataException::BlockchainDataException(const std::string& err) :
   std::runtime_error{err}
{}

////////////////////////////////////////////////////////////////////////////////
// BlockchainData
BlockchainData::BlockchainData(std::shared_ptr<Blockchain> bcPtr) :
   blockchain_{bcPtr}
{}

Tx BlockchainData::getTx(const Types::TxKey& key) const
{
   auto blockID = Types::getBlockIDFromTxKey(key);
   auto txId = Types::getTxIndexFromTxKey(key);
   return getTx(blockID, txId);
}

Tx BlockchainData::getTx(const Types::BlockId& blockID, Types::TxId txId) const
{
   //grab header
   auto header = blockchain_->getHeaderById(blockID);
   if (header == nullptr) {
      throw BlockchainDataException{
         std::format("invalid block key: {}", blockID)};
   }

   if (txId >= header->getNumTx()) {
      throw BlockchainDataException(std::format(
         "[blockID {}] txid > numTx ({}, {})",
         blockID, txId, header->getNumTx()));
   }

   //open block file
   auto path = FileUtils::getBlkFilename(
      Config::Pathing::blkFilePath(), header->getBlockFileNum());
   auto fileMap = FileUtils::FileMap(path, false);
   try {
      std::vector<uint64_t> xoredData;
      std::shared_ptr<BlockData> block;
      if (!Config::DBSettings::isXored()) {
         block = BlockData::deserialize(
            fileMap.ptr() + header->getOffset(),
            header->getBlockSize(), header,
            BlockData::CheckHashes::NoChecks);
      } else {
         /*
         XOR chunks are 8 bytes aligned. Block data is packed tight,
         therefor the start of a block is not 8 aligned.

         Copy 8 bytes aligned data around the block, xor it, then read
         from the block start offset (ignore the bytes preceeding the block
         that were carried over for alignement purposes)
         */
         size_t prepad = header->getOffset() % 8;
         xoredData.resize((prepad + header->getBlockSize() + 7) / 8);
         std::memcpy((uint8_t*)&xoredData[0],
            fileMap.ptr() + header->getOffset() - prepad,
            header->getBlockSize() + prepad);

         auto xorkey = Config::DBSettings::getXorKey();
         for (auto& chunk : xoredData) {
            chunk ^= xorkey;
         }

         block = BlockData::deserialize(
            (const uint8_t*)&xoredData[0] + prepad,
            header->getBlockSize(), header,
            BlockData::CheckHashes::NoChecks);
      }

      const auto& bctx = block->getTxns()[txId];
      Tx tx{bctx->data_, bctx->size_};
      tx.setTxKey(Types::constructTxKey(blockID, txId));
      return tx;
   } catch (const BtcUtils::BlockDeserializingException&) {
      throw BlockchainDataException(std::format(
         "failed to grab tx {}|{}", blockID, txId));
   }
}

////////
Hash32 BlockchainData::getTxHashForTxKey(const Types::TxKey& txKey) const
{
   auto tx = getTx(txKey);
   return Hash32{tx.getThisHash()};
}
