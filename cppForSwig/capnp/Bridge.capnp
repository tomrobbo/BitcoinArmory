@0x98fa84da458428ed;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("Armory::Codec::Bridge");

using Types = import "Types.capnp";

###############################
# WalletData
###############################

struct WalletData {
   struct AddressData {
      index             @0 : Int32;
      addrType          @1 : UInt32;
      isUsed            @2 : Bool;
      isChange          @3 : Bool;
      assetId           @4 : Data;
      hasPrivKey        @5 : Bool;

      prefixedHash      @6 : Types.Hash;
      publicKey         @7 : Data;
      precursorScript   @8 : Data;

      addressString     @9 : Text;
   }

   struct Comment {
      key @0 : Data;
      val @1 : Text;
   }

   ##
   walletId             @0 : Types.WalletId;
   accountId            @1 : Types.AccountId;
   masterId             @15: Text;
   dbId                 @2 : Text;
   useCount             @3 : Int64;
   lookupCount          @4 : Int64;
   watchingOnly         @5 : Bool;
   addressTypes         @6 : List(UInt32);
   defaultAddressType   @7 : UInt32;
   usesEncryption       @8 : Bool;
   kdfMemReq            @9 : UInt32;
   path                 @14: Text;

   label                @10: Text;
   desc                 @11: Text;

   addressData          @12: List(AddressData);
   comments             @13: List(Comment);
}

###############################
# Notifications
###############################

struct Notification {
   ## Wallet creation progress notifs
   struct WalletProgress {
      union {
         unset          @0 : Void;

         createFile     @1 : Text;
         initFile       @2 : Types.WalletId;
         readFile       @3 : Types.WalletId;
         createAccount  @4 : Text;

         extendChain : group {
            total       @5 : UInt32;
            current     @6 : UInt32;
         }
      }
   }

   ## RestoreWallet notifs
   struct RestorePrompt {
      struct WalletMeta {
         walletId          @0 : Text;
         backupType        @1 : UInt32;
      }

      struct ChecksumResult {
         lineId            @0 : UInt32;
         value             @1 : Int32;
      }

      union {
         unset             @0 : Void;
         checkWalletId     @1 : WalletMeta;
         getPassphrases    @2 : Void;
         decryptError      @3 : Void;
         failure           @4 : Text; #error verbose
         typeError         @5 : Text;
         checksumError     @6 : List(ChecksumResult);
         checksumMismatch  @7 : List(ChecksumResult);
      }
   }

   #callbackId is set if this notification is the result
   #of a RPC request that provided said id
   callbackId        @0 : Text;
   counter           @1 : UInt32;

   union {
      unset          @2 : Void;
      ready          @3 : Types.Height;
      setupDone      @4 : Void;
      registerDone   @5 : Void;
      refresh        @6 : List(Text);
      newBlock       @7 : Types.Height;
      disconnected   @8 : Void;
      scanProgress   @9 : Types.ScanProgress;
      nodeStatus     @10: Types.NodeStatus;
      zeroConfs      @11: Types.TxLedger;
      error          @12: Text;
      cleanup        @13: Void;
      unlockRequest  @14: List(Text);
      walletProgress @15: WalletProgress;
      restore        @16: RestorePrompt;
   }
}

struct NotificationReply
{
   enum RestoreMode {
      overwrite   @0;
      merge       @1;
   }

   success        @0 : Bool;
   counter        @1 : UInt32;

   union {
      restore     @2 : RestoreMode;
      passphrases @3 : List(Text);
   }
}

###############################
# Blockchain Service
###############################

struct BlockchainServiceRequest {
   struct RegisterWallet {
      walletId    @0 : Types.WalletId;
      accountId   @1 : Types.AccountId;
      isNew       @2 : Bool;
   }

   union {
      unset                         @0 : Void;

      shutdown                      @1 : Void;
      setupDb                       @2 : Void;
      goOnline                      @3 : Void;
      getNodeStatus                 @4 : Void;
      registerWallets               @5 : Void;

      registerWallet                @6 : RegisterWallet;
      broadcastTx                   @7 : List(Data);
      getTxsByHash                  @8 : List(Types.Hash);
      getHeadersByHeight            @9: List(Types.Height);
      getBlockTimeByHeight          @10: UInt32;
      getFeeSchedule                @11: Text;

      getLedgerDelegateId           @12: Void;
      updateWalletsLedgerFilter     @13: List(Types.WalletId);
   }
}

struct BlockchainServiceReply {
   struct TxData {
      raw         @0 : Data;
      hash        @1 : Types.Hash;
      height      @2 : Types.Height;
      txIndex     @3 : UInt32;
      rbf         @4 : Bool;
      chainedZc   @5 : Bool;
   }

   # reply
   union {
      unset                         @0 : Void;

      getNodeStatus                 @1 : Types.NodeStatus;
      getTxsByHash                  @2 : List(TxData);
      getHeadersByHeight            @3 : List(Types.Header);
      getBlockTimeByHeight          @4 : UInt32;
      getFeeSchedule                @5 : List(Types.FeeSchedule);
      getLedgerDelegateId           @6 : Types.DelegateId;
   }
}

###############################
# WalletManager
###############################
struct WalletManagerRequest {
   struct StageWalletStruct {
      walletId @0 : Types.WalletId;
      stage    @1 : Bool;
   }

   struct UnlockRequest {
      walletPath @0 : Text;
      callbackId @1 : Text;
   }

   union {
      unset                   @0 : Void;

      #list wallets in datadir
      listWallets             @1 : Void;

      #migrate a legacy armory .wallet file to the new format
      migrateLegacyWallet     @2 : Types.WalletId;

      #public data in wallets with an encrypted control header cannot
      #be read, it needs unlocked first
      unlockControlHeader     @3 : UnlockRequest;

      #flag wallet to be loaded or not
      stageWallet             @4 : StageWalletStruct;

      #load staged wallets
      loadWallets             @5 : Void;
   }
}

struct WalletManagerReply {
   enum WalletLoadState {
      unknown     @0;
      legacy      @1;
      migrated    @2;
      encrypted   @3;
      ready       @4;
      loaded      @5;
   }

   struct WalletFileData {
      state    @0 : WalletLoadState;
      path     @1 : Text;
      walletId @2 : Text;
      staged   @3 : Bool;
   }

   union {
      unset                @0 : Void;
      listWallets          @1 : List(WalletFileData);
      migrateLegacyWallet  @2 : Bool;
      loadWallets          @3 : List(WalletData);
   }
}

###############################
# Wallet
###############################
struct WalletRequest {
   struct AddressRequest {
      type           @0 : UInt32;
      union {
         new         @1 : Void;
         change      @2 : Void;
         peekChange  @3 : Void;
      }
   }

   struct ExtendAddressPool {
      count       @0 : UInt32;
      callbackId  @1 : Types.CallbackId;
   }

   struct SetAddressTypeFor {
      assetId       @0 : Data;
      addressType   @1 : UInt32;
   }

   struct OutputRequest {
      union {
         value @0 : Types.CoinAmount;
         zc    @1 : Void;
         rbf   @2 : Void;
      }
   }

   struct SetComment {
      key      @0 : Text;
      comment  @1 : Text;
   }

   struct SetLabels {
      title        @0 : Text;
      description  @1 : Text;
   }

   walletId                         @0 : Types.WalletId;
   accountId                        @1 : Types.AccountId;
   union {
      unset                         @2 : Void;

      getAddress                    @3 : AddressRequest;
      getHighestUsedIndex           @4 : Void;
      extendAddressPool             @5 : ExtendAddressPool;

      createBackupString            @6 : Types.CallbackId;
      deleteWallet                  @7 : Void;
      getData                       @8 : Void;

      getAddrCombinedList           @9 : Void;
      setAddressTypeFor             @10: SetAddressTypeFor;

      getLedgerDelegateId           @11: Void;
      getLedgerDelegateIdForScrAddr @12: Types.ScrAddr;
      getBalanceAndCount            @13: Void;

      setupNewCoinSelectionInstance @14: Types.Height;
      getUtxos                      @15: OutputRequest;

      createAddressBook             @16: Void;
      setComment                    @17: SetComment;
      setLabels                     @18: SetLabels;
   }
}

####
struct UTXO {
   output   @0 : Types.Output;
   scrAddr  @1 : Types.ScrAddr;
}

struct WalletReply {
   struct BackupString {
      rootClear   @0 : List(Text);
      chainClear  @1 : List(Text);

      rootEncr    @2 : List(Text);
      chainEncr   @3 : List(Text);

      spPass      @4 : Text;
   }

   # Address Balance
   struct AddressBalanceData {
      scrAddr  @0 : Types.ScrAddr;
      balances @1 : Types.BalanceAndCount;
   }

   struct AddressAndBalanceData {
      balances       @0 : List(AddressBalanceData);
      updatedAssets  @1 : List(WalletData.AddressData);
   }


   # reply
   union {
      unset                         @0 : Void;

      getAddress                    @1 : WalletData.AddressData;
      getHighestUsedIndex           @2 : Int32;
      extendAddressPool             @3 : WalletData;
      createBackupString            @4 : BackupString;
      getData                       @5 : WalletData;
      getAddrCombinedList           @6 : AddressAndBalanceData;
      setAddressTypeFor             @7 : WalletData.AddressData;
      getLedgerDelegateId           @8 : Types.DelegateId;
      getLedgerDelegateIdForScrAddr @9 : Types.DelegateId;
      getBalanceAndCount            @10: Types.BalanceAndCount;
      setupNewCoinSelectionInstance @11: Text;
      getUtxos                      @12: List(UTXO);
      createAddressBook             @13: Types.AddressBook;
   }
}

###############################
# Coin Selection
###############################

struct CoinSelectionRequest {
   struct SetRecipient {
      address  @0 : Text;
      value    @1 : UInt64;
      id       @2 : UInt32;
   }

   struct SelectUTXOs {
      flags       @0 : UInt32;
      union {
         flatFee  @1 : UInt64;
         feeByte  @2 : Float32;
      }
   }

   struct CustomUtxoList {
      utxos       @0 : List(Types.Output);
      flags       @1 : UInt32;
      union {
         flatFee  @2 : UInt64;
         feeByte  @3 : Float32;
      }
   }

   struct FeeForMaxVal {
      utxos    @0 : List(Types.Output);
      feeByte  @1 : Float32;
   }

   id                         @0 : Text;
   union {
      unset                   @1 : Void;

      cleanup                 @2 : Void;
      reset                   @3 : Void;

      setRecipient            @4 : SetRecipient;
      selectUtxos             @5 : SelectUTXOs;

      getUtxoSelection        @6 : Void;
      getFlatFee              @7 : Void;
      getFeeByte              @8 : Void;
      getSizeEstimate         @9 : Void;

      processCustomUtxoList   @10: CustomUtxoList;
      getFeeForMaxVal         @11: FeeForMaxVal;
   }
}

struct CoinSelectionReply {
   union {
      unset             @0 : Void;

      getUtxoSelection  @1 : List(UTXO);
      getFlatFee        @2 : Types.CoinAmount;
      getFeeByte        @3 : Float32;
      getSizeEstimate   @4 : UInt32;
      getFeeForMaxVal   @5 : Types.CoinAmount;
   }
}

###############################
# Signer
###############################

struct SignerRequest {
   struct AddSpenderByOutpoint {
      hash     @0 : Types.Hash;
      txOutId  @1 : UInt16;
      sequence @2 : UInt32;
   }

   struct PopulateUtxo {
      hash     @0 : Types.Hash;
      script   @1 : Data;
      txOutId  @2 : UInt16;
      value    @3 : Types.CoinAmount;
   }

   struct AddRecipient{
      script   @0 : Data;
      value    @1 : Types.CoinAmount;
   }

   struct SignTx {
      walletId    @0 : Types.WalletId;
      callbackId  @1 : Text;
   }

   id                         @0 : Text;
   union {
      unset                   @1 : Void;

      getNew                  @2 : Void;
      cleanup                 @3 : Void;

      setVersion              @4 : UInt32;
      setLockTime             @5 : UInt32;

      addSpenderByOutpoint    @6 : AddSpenderByOutpoint;
      populateUtxo            @7 : PopulateUtxo;
      addRecipient            @8 : AddRecipient;

      toTxSigCollect          @9 : UInt32;
      fromTxSigCollect        @10: Text;

      signTx                  @11: SignTx;
      getSignedTx             @12: Void;
      getUnsignedTx           @13: Void;
      getSignedStateForInput  @14: UInt32;

      resolve                 @15: Types.WalletId;
      addSupportingTx         @16: Data;

      fromType                @17: Void;
      canLegacySerialize      @18: Void;
   }
}

struct SignerReply {
   struct InputSignedState {
      struct PubKeySignatureState {
         pubKey   @0 : Data;
         hasSig   @1 : Bool;
      }

      isValid     @0 : Bool;
      mCount      @1 : UInt32;
      nCount      @2 : UInt32;

      sigCount    @3 : UInt32;
      signStates  @4 : List(PubKeySignatureState);
   }

   union {
      unset                   @0 : Void;

      getNew                  @1 : Text;
      toTxSigCollect          @2 : Text;
      getSignedTx             @3 : Data;
      getUnsignedTx           @4 : Data;
      getSignedStateForInput  @5 : InputSignedState;
      fromType                @6 : UInt32;
      canLegacySerialize      @7 : Bool;
   }
}

###############################
# Utils
###############################

struct UtilsRequest {
   struct CreateWalletStruct
   {
      callbackId        @0 : Text;
      lookup            @1 : UInt32;
      extraEntropy      @2 : Data;

      label             @3 : Text;
      description       @4 : Text;

      privPassphrase    @5 : Text;
      privKdfTargetMs   @6 : UInt32;
      privKdfTargetMB   @7 : UInt32;

      ctrlPassphrase    @8 : Text;
      ctrlKdfTargetMs   @9 : UInt32;
      ctrlKdfTargetMB   @10: UInt32;
   }

   struct RestoreWalletStruct
   {
      root              @0 : List(Text);
      chaincode         @1 : List(Text);
      spPass            @2 : Text;
      callbackId        @3 : Text;

      privKdfTargetMs   @4 : UInt32;
      privKdfTargetMB   @5 : UInt32;

      ctrlKdfTargetMs   @6 : UInt32;
      ctrlKdfTargetMB   @7 : UInt32;
   }

   union {
      unset                @0 : Void;

      generateRandomHex    @1 : UInt32;
      getHash160           @2 : Data;
      getNameForAddrType   @3 : Int32;
      createWallet         @4 : CreateWalletStruct;
      restoreWallet        @5 : RestoreWalletStruct;
   }
}

struct UtilsReply {
   union {
      unset                @0 : Void;

      generateRandomHex    @1 : Text;
      getHash160           @2 : Types.Hash;
      getNameForAddrType   @3 : Text;
      createWallet         @4 : Types.WalletId;
   }
}

###############################
# Script Utils
###############################

struct ScriptUtilsRequest {
   script                        @0 : Data;

   union {
      unset                      @1 : Void;

      getTxInScriptType          @2 : Types.Hash;
      getTxOutScriptType         @3 : Void;
      getScrAddrForScript        @4 : Void;
      getLastPushDataInScript    @5 : Void;
      getTxOutScriptForScrAddr   @6 : Void;
      getAddrStrForScrAddr       @7 : Void;
      getScrAddrForAddrStr       @8 : Text;
   }
}

struct ScriptUtilsReply {
   union {
      unset                      @0 : Void;

      getTxInScriptType          @1 : UInt32;
      getTxOutScriptType         @2 : UInt32;
      getScrAddrForScript        @3 : Types.ScrAddr;
      getLastPushDataInScript    @4 : Data;
      getTxOutScriptForScrAddr   @5 : Data;
      getAddrStrForScrAddr       @6 : Text;
      getScrAddrForAddrStr       @7 : Types.ScrAddr;
   }
}

###############################
# Ledger Delegates
###############################
struct LedgerDelegateRequest {
   id                @0 : Types.DelegateId;

   union {
      unset          @1 : Void;

      getPageCount   @2 : Void;
      getPages       @3 : Types.PageRequest;
   }
}

struct LedgerDelegateReply {
   union {
      unset          @0 : Void;

      getPageCount   @1 : UInt32;
      getPages       @2 : List(Types.TxLedger);
   }
}

###############################
# Request/Reply
###############################

struct ToBridge {
   referenceId       @0 : UInt64;

   # method
   union {
      unset          @1 : Void;

      service        @2 : BlockchainServiceRequest;
      walletManager  @3 : WalletManagerRequest;
      wallet         @4 : WalletRequest;
      coinSelection  @5 : CoinSelectionRequest;
      signer         @6 : SignerRequest;
      utils          @7 : UtilsRequest;
      scriptUtils    @8 : ScriptUtilsRequest;
      delegate       @9 : LedgerDelegateRequest;
      notification   @10: NotificationReply;
   }
}

struct RpcReply {
   success           @0 : Bool;
   referenceId       @1 : UInt64;
   error             @2 : Text;

   # replyPayload
   union {
      unset          @3 : Void;

      service        @4 : BlockchainServiceReply;
      walletManager  @5 : WalletManagerReply;
      wallet         @6 : WalletReply;
      coinSelection  @7 : CoinSelectionReply;
      signer         @8 : SignerReply;
      utils          @9 : UtilsReply;
      scriptUtils    @10: ScriptUtilsReply;
      delegate       @11: LedgerDelegateReply;
   }
}

struct FromBridge {
   union {
      unset          @0 : Void;

      reply          @1 : RpcReply;
      notification   @2 : Notification;
   }
}