////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2019-2025, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#ifndef _CPPBRIDGE_H
#define _CPPBRIDGE_H

#include "../ArmoryConfig.h"
#include "Wallets/Manager.h"
#include "../DBClientClasses.h"
#include "btc/ecc.h"

namespace AsyncClient
{
   class BlockDataViewer;
   class LedgerDelegate;
}

namespace Armory
{
   namespace Seeds
   {
      struct PromptReply;
   };

   namespace Signing
   {
      class Signer;
      class TxEvalState;
   }

   namespace CoinSelection
   {
      class CoinSelectionInstance;
   }

   namespace Bridge
   {
      struct ServerPushWrapper;
      struct WritePayload_Bridge;

      //////////////////////////////////////////////////////////////////////////
      enum class BridgeNotifType : int
      {
         PUSH,
         UPDATE
      };

      struct BridgeNotifStruct
      {
         const BridgeNotifType type;

         //set when type is PUSH
         BinaryData packet;

         //set when type is UPDATE
         std::function<void(void)> lbd;
      };
      typedef std::function<void(BridgeNotifStruct)> notifLbd;

      ////
      using MessageId = uint64_t;

      ////
      class BridgeCallback : public RemoteCallback
      {
      private:
         //to push packets to the gui
         notifLbd pushNotifLbd_;

         //id members
         std::mutex idMutex_;
         std::unordered_map<std::string, std::function<void(void)>> idCallbacks_;

      private:
         void processRefreshCallbacks(std::set<std::string>&);

      public:
         BridgeCallback(const notifLbd& lbd) :
            RemoteCallback(), pushNotifLbd_(lbd)
         {}

         //virtuals
         void run(BdmNotification) override;
         void progress(
            BDMPhase phase,
            const std::vector<std::string> &walletIdVec,
            float progress, unsigned secondsRem,
            unsigned progressNumeric
         ) override;
         void disconnected(void) override;

         void notifySetupDone(void);
         void notifySetupRegistrationDone(void);
         void notifyRefresh(const std::set<std::string>&);
         void registerRefreshCallback(const std::string&,
            const std::function<void(void)>&);
      };

      //////////////////////////////////////////////////////////////////////////
      using WalletPtr = std::shared_ptr<Armory::Wallets::AssetWallet>;
      class CppBridgeSignerStruct
      {
      private:
         std::unique_ptr<Signing::TxEvalState> signState_{};
         const std::function<WalletPtr(const std::string&)> getWalletFunc_;
         const std::function<void(ServerPushWrapper)> writeFunc_;

      public:
         std::unique_ptr<Signing::Signer> signer;

      public:
         CppBridgeSignerStruct(std::function<WalletPtr(const std::string&)>,
            std::function<void(ServerPushWrapper)>);

         void signTx(const std::string&, const std::string&, MessageId);
         bool resolve(const std::string&);
         BinaryData getSignedStateForInput(unsigned, MessageId);
      };

      //////////////////////////////////////////////////////////////////////////
      using CallbackHandler = std::function<bool(const Seeds::PromptReply&)>;

      class CppBridge
      {
      private:
         PRNG_Fortuna fortuna_;

         //where wallets are loaded from
         const std::filesystem::path path_;

         //armorydb config
         const std::string dbAddr_;
         const std::string dbPort_;
         const bool dbOneWayAuth_;
         const bool dbOffline_;

         //to write to the bridge client
         std::function<void(std::unique_ptr<WritePayload_Bridge>)> writeLambda_;

         //these objects are the core of the bridge
         std::shared_ptr<WalletManager> wltManager_;
         std::shared_ptr<BridgeCallback> callbackPtr_;
         std::shared_ptr<AsyncClient::BlockDataViewer> bdvPtr_;

         //various states cache
         std::map<std::string, AsyncClient::LedgerDelegate> delegateMap_;
         std::map<std::string,
            std::shared_ptr<CoinSelection::CoinSelectionInstance>> csMap_;
         std::map<std::string,
            std::shared_ptr<CppBridgeSignerStruct>> signerMap_;

         //UI related ad hoc callbacks
         std::mutex callbackHandlerMu_;
         std::map<uint32_t, CallbackHandler> callbackHandlers_;

      public:
         std::shared_ptr<AsyncClient::BlockDataViewer> bdvPtr(void) const;
         void reset(void);

      public:
         //wallet manager init methods
         BinaryData listWallets(MessageId);
         void unlockControlHeader(const std::string&, const std::string&,
            MessageId);
         bool stageWallet(const std::string&, bool);
         BinaryData loadWallets(MessageId);

         //wallet setup
         const std::filesystem::path& getDataDir(void) const;
         BinaryData createWalletsPacket(MessageId);
         bool deleteWallet(const std::string&);
         BinaryData getWalletPacket(const std::string&,
            Wallets::AddressAccountId, MessageId) const;

         //AsyncClient::BlockDataViewer setup
         void setupDB(void);
         void registerWallets(void);
         void registerWallet(const std::string&,
            const Wallets::AddressAccountId&, bool isNew);
         BinaryData getNodeStatus(MessageId);

         //balance and counts
         BinaryData getBalanceAndCount(const std::string&,
            const Wallets::AddressAccountId&, MessageId);
         BinaryData getAddrCombinedList(const std::string&,
            const Wallets::AddressAccountId&, MessageId);
         BinaryData getHighestUsedIndex(const std::string&,
            const Wallets::AddressAccountId&, MessageId);

         //create/generate wallet & addresses
         void extendAddressPool(const std::string&,
            const Wallets::AddressAccountId&, unsigned,
            const std::string&, MessageId);
         BinaryData getAddress(const std::string&,
            const Wallets::AddressAccountId&, uint32_t,
            uint32_t, MessageId);
         void createWallet(
            SecureBinaryData, //extra entropy
            Wallets::IO::CreateWalletParams,
            const std::string&, //callbackId
            MessageId);
         void createBackupStringForWallet(const std::string&,
            const std::string&, SecureBinaryData, MessageId);
         void restoreWallet(
            const std::vector<std::string_view>&,
            const std::string_view&,
            const std::string_view&, MessageId);

         //ledgers
         const std::string& getLedgerDelegateId(void);
         const std::string& getLedgerDelegateIdForWallet(
            const std::string&, const Wallets::AddressAccountId&);
         const std::string& getLedgerDelegateIdForScrAddr(
            const std::string&, const Wallets::AddressAccountId&,
            const BinaryDataRef&);
         void getHistoryPageForDelegate(const std::string&,
            unsigned, unsigned, MessageId);
         void createAddressBook(const std::string&,
            const Wallets::AddressAccountId&, MessageId);
         void setComment(const std::string&,
            const std::string&, const std::string&);
         void setWalletLabels(const std::string&,
            const std::string&, const std::string&);

         //txs & headers
         void getTxsByHash(const std::set<BinaryData>&, MessageId);
         void getHeadersByHeight(const std::vector<unsigned>&, MessageId);

         //utxos
         void getUTXOs(const std::string&,
            const Wallets::AddressAccountId&,
            uint64_t, bool, bool, MessageId);

         //coin selection
         void setupNewCoinSelectionInstance(const std::string&,
            const Wallets::AddressAccountId&, unsigned, MessageId);
         void destroyCoinSelectionInstance(const std::string&);
         std::shared_ptr<CoinSelection::CoinSelectionInstance>
            coinSelectionInstance(const std::string&) const;

         //signer
         BinaryData initNewSigner(MessageId);
         void destroySigner(const std::string&);
         std::shared_ptr<CppBridgeSignerStruct> signerInstance(
            const std::string&) const;
         WalletPtr getWalletPtr(const std::string&) const;

         //script utils
         BinaryData getTxInScriptType(
            const BinaryData&, const BinaryData&, MessageId) const;
         BinaryData getTxOutScriptType(const BinaryData&, MessageId) const;
         BinaryData getScrAddrForScript(const BinaryData&, MessageId) const;
         BinaryData getScrAddrForAddrStr(const std::string&, MessageId) const;
         BinaryData getLastPushDataInScript(const BinaryData&, MessageId) const;

         //utils
         BinaryData getHash160(const BinaryDataRef&, MessageId) const;
         void broadcastTx(const std::vector<BinaryData>&);
         BinaryData getTxOutScriptForScrAddr(const BinaryData&, MessageId) const;
         BinaryData getAddrStrForScrAddr(const BinaryData&, MessageId) const;
         std::string getNameForAddrType(int) const;
         BinaryData setAddressTypeFor(const std::string&,
            const Wallets::AddressAccountId&, const BinaryDataRef&,
            uint32_t, MessageId) const;
         void getBlockTimeByHeight(uint32_t, MessageId) const;
         void getFeeSchedule(const std::string&, MessageId) const;

         //custom callback handlers
         void callbackWriter(ServerPushWrapper&);
         void setCallbackHandler(ServerPushWrapper&);
         CallbackHandler getCallbackHandler(uint32_t);

         //sanity checks
         bool isOffline(void) const;

         //wallet misc
         void getUnlockTime(const std::string&, MessageId);

      public:
         CppBridge(void);

         void writeToClient(BinaryData&) const;
         void setWriteLambda(
            const std::function<void(std::unique_ptr<WritePayload_Bridge>)>&);
         SecureBinaryData generateRandom(size_t) const;
      };
   }; //namespace Bridge
}; //namespace Armory

#endif