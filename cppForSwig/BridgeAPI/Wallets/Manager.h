////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2016-2025, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#ifndef _WALLET_MANAGER_H
#define _WALLET_MANAGER_H

#include <mutex>
#include <memory>
#include <string>
#include <map>
#include <iostream>
#include <filesystem>

#include "ReentrantLock.h"
#include "Loader.h"
#include "Container.h"

namespace Armory
{
   namespace Seeds
   {
      class WalletBackup;
   };

   namespace Accounts
   {
      class AddressAccount;
   }

   namespace Wallets
   {
      class WalletId;
      class AddressAccountId;
      class AssetWallet;
      class EncryptionKeyId;

      namespace IO
      {
         struct CreateWalletParams;
         struct ReadOnlyFileParams;
      }
   };

   ////////
   namespace Bridge
   {
      class Callback;

      class WalletManager : public Lockable
      {
      private:
         const std::filesystem::path path_;
         std::map<std::string, std::shared_ptr<WalletFileInfo>> walletFiles_;

         std::map<std::string, std::map<
            Wallets::AddressAccountId,
            std::shared_ptr<WalletContainer>>> wallets_;
         std::map<std::string, std::shared_ptr<WalletContainer>> walletsByDbId_;

         std::shared_ptr<Callback> callbackPtr_;
         std::shared_ptr<AsyncClient::BlockDataViewer> bdvPtr_;

      private:
         void initAfterLock(void) override {}
         void cleanUpBeforeUnlock(void) override {}
         std::shared_ptr<WalletContainer> addAccount(
            std::shared_ptr<Wallets::AssetWallet>,
            const Wallets::AddressAccountId&);
         void addAllAccounts(std::shared_ptr<Wallets::AssetWallet>);
         void loadAFile(const std::filesystem::path&);

      public:
         WalletManager(const std::filesystem::path&);

         /* pre wallets loading calls */
         std::map<std::string, std::shared_ptr<WalletFileInfo>> listWallets(void);
         void unlockControlHeader(const std::string&, const Passphrase::UnlockFunc&);
         const std::string& migrateWallet(const std::filesystem::path&,
            const Passphrase::UnlockFunc&,
            const Wallets::IO::CreateWalletParams&
         );
         bool stageWallet(const Wallets::WalletId&, bool);
         void loadWallets(void);
         std::shared_ptr<WalletFileInfo> importFile(const std::filesystem::path&);

         /* db setup */
         void registerWallets(void);
         void registerWallet(const Wallets::WalletId&,
            const Wallets::AddressAccountId&, bool);
         std::shared_ptr<Callback> setupBdvCallback(
            const std::function<void(BinaryData&)>&);
         void setBdvPtr(std::shared_ptr<AsyncClient::BlockDataViewer>);

         /* utils */
         const std::filesystem::path& getWalletDir(void) const;
         void updateStateFromDB(const std::function<void(void)>&);

         /* loaded wallet getters */
         bool hasWallet(const std::string&);
         std::shared_ptr<WalletContainer> getWalletContainer(
            const std::string&) const;
         std::shared_ptr<WalletContainer> getWalletContainer(
            const std::string&, const Wallets::AddressAccountId&) const;
         std::map<std::string, std::set<Wallets::AddressAccountId>>
            getAccountIdMap(void) const;

         /* wallet add/create/delete */
         void loadWallet(const Wallets::IO::ReadOnlyFileParams&);
         std::shared_ptr<WalletContainer> createNewWallet(
            const SecureBinaryData&, //extra entropy
            const Wallets::IO::CreateWalletParams&);

         std::filesystem::path unloadWallet(const std::string&);
         void deleteWallet(const std::string&);
      };
   } //namespace Bridge
} //namespace Armory

#endif
