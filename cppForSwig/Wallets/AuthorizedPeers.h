////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2019-2026, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <memory>
#include <string>
#include <set>
#include <map>
#include <filesystem>

#include <Utils/SecureBinaryData.h>
#include <btc/ecc_key.h>

#define PEERS_WALLET_PASSWORD "password"
#define PEERS_WALLET_BIP32_ACCOUNT 0xFF005618

struct AuthPeersLambdas;

namespace Armory
{
   namespace Accounts
   {
      class MetaDataAccount;
      struct PeerAssetMap;
   }

   namespace Wallets
   {
      class AssetWallet;

      namespace IO
      {
         struct ReadOnlyFileParams;
         struct CreateFileParams;
      };
   }

   /////////////////////////////////////////////////////////////////////////////
   namespace NetworkPeers
   {
      struct FileMissing
      {
         FileMissing(void)
         {}
      };

      ////////
      struct Exception : public std::runtime_error
      {
         Exception(const std::string& err) :
            std::runtime_error(err)
         {}
      };

      ////////
      enum class PeerType : int {
         Client = 0,
         ServerOneWay,
         ServerTwoWay
      };

      class PeerKey
      {
      private:
         const SecureBinaryData pubkey_;
         const PeerType type_;

      public:
         PeerKey(BinaryDataRef, PeerType);
         PeerKey(const btc_pubkey&, PeerType);

         const SecureBinaryData& getKey(void) const;
         bool isServer(void) const;
         bool isOneWay(void) const;

         std::string toHumanReadable(void) const;
         static PeerKey fromHumanReadable(const std::string&);
      };

      class PeerMap
      {
      private:
         std::map<std::string, btc_pubkey> nameToKeyMap_;
         std::map<SecureBinaryData, std::string> keyMap_;
         std::map<SecureBinaryData, std::set<unsigned>> keyToAssetIndexMap_;

         const std::shared_ptr<Wallets::AssetWallet> wallet_;
         const bool oneWay_;

      public:
         PeerMap(std::shared_ptr<Wallets::AssetWallet>, const btc_pubkey&, bool);

         void setupFromAssetMap(const Accounts::PeerAssetMap&);
         void grabKeyIndexes(std::shared_ptr<Accounts::MetaDataAccount>);

         const std::map<std::string, btc_pubkey>& getPeerNameMap(void) const;
         const std::map<SecureBinaryData, std::string>& getPublicKeyMap(void) const;

         void addPeer(const SecureBinaryData&,
            const std::vector<std::string>&, const std::string&);
         void setLabel(const SecureBinaryData&, const std::string&);

         btc_pubkey eraseName(const std::string&);
         void eraseKey(const SecureBinaryData&);
      };

      ////////
      class PeerStore
      {
      protected:
         std::map<SecureBinaryData, SecureBinaryData> privateKeys_;
         std::shared_ptr<Wallets::AssetWallet> wallet_ = nullptr;

      protected:
         PeerStore(const Wallets::IO::ReadOnlyFileParams&);
         PeerStore(std::shared_ptr<Wallets::AssetWallet>);
         PeerStore(SecureBinaryData&);
         PeerStore(void);
         virtual ~PeerStore(void) = 0;

         virtual void initPeerMap(const SecureBinaryData&) = 0;
         SecureBinaryData setOwnPrivateKey(SecureBinaryData&);
         void loadWallet(const Wallets::IO::ReadOnlyFileParams&);
         void initFromWallet(void);
         void erasePeerRootKey(const SecureBinaryData&);

         virtual void setupFromAssetMap(const Accounts::PeerAssetMap&) = 0;
         virtual void grabKeyIndexes(std::shared_ptr<Accounts::MetaDataAccount>) = 0;

      public:
         const SecureBinaryData& getPrivateKey(const BinaryDataRef&) const;
         const btc_pubkey& getOwnPublicKey(void) const;

         virtual void addPeer(const PeerKey&,
            const std::vector<std::string>&, const std::string&) = 0;
         virtual void erasePeer(const PeerKey&) = 0;
         virtual void eraseName(const std::string&, bool) = 0;
         virtual void eraseKey(const btc_pubkey&, bool) = 0;
         virtual void eraseKey(BinaryDataRef, bool) = 0;

         virtual const std::string& getLabel(
            const SecureBinaryData&, bool) const = 0;
         virtual void setLabel(const PeerKey&, const std::string&) = 0;
   
         virtual const std::map<std::string, btc_pubkey>&
         getPeerNameMap(bool) const = 0;
         virtual const std::map<SecureBinaryData, std::string>&
         getPublicKeyMap(bool) const = 0;

         //takes path to peers db, passphrase lambdas are handled internally
         static void changeControlPassphrase(const std::filesystem::path&);
         static AuthPeersLambdas getAuthPeersLambdas(
            std::shared_ptr<PeerStore>, bool);
         static std::shared_ptr<Wallets::AssetWallet> initOnDisk(
            const Wallets::IO::CreateFileParams&);
      };

      ////
      class ServerStore : public PeerStore
      {
      private:
         std::unique_ptr<PeerMap> peerMap_;

         //public key of master ACL; a client that completes a 2-way
         //AEAD handshake with this key will receive master credentials
         SecureBinaryData masterKey_;

      private:
         void initPeerMap(const SecureBinaryData&) override;
         void setupFromAssetMap(const Accounts::PeerAssetMap&) override;
         void grabKeyIndexes(std::shared_ptr<Accounts::MetaDataAccount>) override;

      public:
         ServerStore(void);
         ServerStore(std::shared_ptr<Wallets::AssetWallet>);
         ServerStore(const Wallets::IO::ReadOnlyFileParams&);

         bool setMasterKey(const PeerKey&);
         void eraseMasterKey(void);
         bool isMasterKey(const btc_pubkey&) const;
         bool isMasterKey(const SecureBinaryData&) const;

         void addPeer(const PeerKey&,
            const std::vector<std::string>&, const std::string&) override;
         void erasePeer(const PeerKey&) override;
         void eraseName(const std::string&, bool) override;
         void eraseKey(const btc_pubkey&, bool) override;
         void eraseKey(BinaryDataRef, bool) override;

         const std::string& getLabel(
            const SecureBinaryData&, bool) const override;
         void setLabel(const PeerKey&, const std::string&) override;

         const std::map<std::string, btc_pubkey>& getPeerNameMap(bool) const override;
         const std::map<SecureBinaryData, std::string>& getPublicKeyMap(bool) const override;
      };

      ////
      class ClientStore : public PeerStore
      {
      private:
         std::unique_ptr<PeerMap> peerMapOneWay_;
         std::unique_ptr<PeerMap> peerMapTwoWay_;

      private:
         ClientStore(SecureBinaryData&);

         void initPeerMap(const SecureBinaryData&) override;
         void setupFromAssetMap(const Accounts::PeerAssetMap&) override;
         void grabKeyIndexes(std::shared_ptr<Accounts::MetaDataAccount>) override;

      public:
         ClientStore(void);
         ClientStore(std::shared_ptr<Wallets::AssetWallet>);
         ClientStore(const Wallets::IO::ReadOnlyFileParams&);

         std::shared_ptr<ClientStore> getNarrowSet(const PeerKey&) const;
         void addPeer(const PeerKey&,
            const std::vector<std::string>&, const std::string&) override;
         void erasePeer(const PeerKey&) override;
         void eraseName(const std::string&, bool) override;
         void eraseKey(const btc_pubkey&, bool) override;
         void eraseKey(BinaryDataRef, bool) override;

         const std::string& getLabel(
            const SecureBinaryData&, bool) const override;
         void setLabel(const PeerKey&, const std::string&) override;

         const std::map<std::string, btc_pubkey>& getPeerNameMap(bool) const override;
         const std::map<SecureBinaryData, std::string>& getPublicKeyMap(bool) const override;
      };

   } //namespace NetworkPeers
} //namespace Armory
