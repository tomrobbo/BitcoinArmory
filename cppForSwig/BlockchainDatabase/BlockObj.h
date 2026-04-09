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

#include <string>
#include <Utils/BinaryData.h>

namespace Armory
{
   struct Hash32
   {
      uint64_t data[4];
      explicit Hash32(void);
      explicit Hash32(const BinaryData&);
      explicit Hash32(const BinaryDataRef&);

      bool operator==(const Hash32&) const;
      bool operator==(const BinaryData&) const;
      bool operator<(const Hash32&) const;

      BinaryData toBinaryData(void) const;
      BinaryDataRef getRef(void) const;
      std::string toHexStr(bool=false) const;

      struct Hasher
      {
         using is_transparent = void;
         std::size_t operator()(const Hash32&) const;
         std::size_t operator()(const BinaryData&) const;
         std::size_t operator()(const BinaryDataRef&) const;
      };

      struct Comparator
      {
         using is_transparent = void;
         bool operator()(const Hash32&, const Hash32&) const;
         bool operator()(const Hash32&, const BinaryData&) const;
         bool operator()(const BinaryData&, const Hash32&) const;
         bool operator()(const Hash32&, const BinaryDataRef&) const;
         bool operator()(const BinaryDataRef&, const Hash32&) const;
      };
   };

   ////////
   class BlockHeader;
   using HeaderPtr = std::shared_ptr<BlockHeader>;

   ////////
   class BlockHeader
   {
      friend class Blockchain;

   private:
      BlockHeader(Hash32&, Hash32&, Hash32&, double, uint32_t, uint32_t);
      static BlockHeader unserialize(const uint8_t*, uint32_t, BinaryData={});

   public:
      struct Hasher
      {
         using is_transparent = void;
         std::size_t operator()(const HeaderPtr&) const;
         std::size_t operator()(const Hash32&) const;
         std::size_t operator()(const BinaryData&) const;
         std::size_t operator()(const BinaryDataRef&) const;
      };

      struct Comparator
      {
         using is_transparent = void;
         bool operator()(const HeaderPtr&, const HeaderPtr&) const;
         bool operator()(const HeaderPtr&, const Hash32&) const;
         bool operator()(const Hash32&, const HeaderPtr&) const;
         bool operator()(const HeaderPtr&, const BinaryData&) const;
         bool operator()(const BinaryData&, const HeaderPtr&) const;
         bool operator()(const HeaderPtr&, const BinaryDataRef&) const;
         bool operator()(const BinaryDataRef&, const HeaderPtr&) const;
      };

   public:
      explicit BlockHeader(const uint8_t*, uint32_t);
      explicit BlockHeader(BinaryDataRef);
      BlockHeader(const uint8_t*, uint32_t, const BinaryData&);

      uint32_t getVersion(void) const;
      const Hash32& getThisHash(void) const;
      const Hash32* getNextHash(void) const;
      const Hash32& getPrevHash(void) const;
      const Hash32& getMerkleRoot(void) const;

      uint32_t getTimestamp(void) const;
      uint32_t getBlockHeight(void) const;
      void setBlockHeight(unsigned);
      bool isMainBranch(void) const;
      bool isOrphan(void) const;
      double getDifficulty(void) const;
      double getDifficultySum(void) const;

      uint32_t getNumTx(void) const;
      uint64_t getOffset(void) const;
      uint32_t getBlockFileNum(void) const;

      uint32_t getBlockSize(void) const;
      void setBlockSize(uint32_t);
      void setNumTx(uint32_t);

      void setBlockFile(std::string);
      void setBlockFileNum(uint32_t);
      void setBlockFileOffset(uint64_t);

      void setRawData(BinaryData);
      const BinaryData& getRawData(void) const;

      uint8_t getDuplicateID(void) const;
      void setDuplicateID(uint8_t);
      uint32_t getUniqueID(void) const;
      void setUniqueID(uint32_t);

      void pprintAlot(std::ostream& = std::cout);

   private:
      const Hash32      thisHash_;
      const Hash32      prevHash_;
      const Hash32      merkleRoot_;
      const double      difficultyDbl_;
      const uint32_t    timestamp_;
      const uint32_t    version_;

      //only useful to write header on disk the one time
      BinaryData     rawData_;

      // Specific to the DB storage
      double         difficultySum_ = -1.0;
      uint64_t       blkFileOffset_ = SIZE_MAX;
      uint32_t       blkFileNum_ = UINT32_MAX;
      uint32_t       blockHeight_ = UINT32_MAX;
      uint32_t       uniqueID_ = UINT32_MAX;

      uint32_t       numTx_ = UINT32_MAX;
      uint32_t       numBlockBytes_; // includes header + nTx + sum(Tx)

      // Need to compute these later
      const Hash32*  nextHash_ = nullptr;
      std::shared_ptr<BlockHeader> nextPtr_ = nullptr;

      uint8_t        duplicateID_ = 0xFF; // ID of this blk rel to others at same height
      bool           isMainBranch_ = false;
      bool           isOrphan_ = true;
      bool           isFinishedCalc_ = false;
   };
}
