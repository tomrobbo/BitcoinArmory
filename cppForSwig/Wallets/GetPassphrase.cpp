////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2025, goatpig                                               //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include "GetPassphrase.h"
#include <chrono>

using namespace Armory::Passphrase;
using namespace std::chrono_literals;

////////////////////////////////////////////////////////////////////////////////
//
//// Params
//
////////////////////////////////////////////////////////////////////////////////
Params::Params() :
   unlockMs(0ms)
{}

Params::Params(std::chrono::milliseconds target, uint32_t mb,
   SecureBinaryData pass) :
   unlockMs(target), memTargetMB(mb), passphrase{std::move(pass)}
{}

////////////////////////////////////////////////////////////////////////////////
//
//// SetNew
//
////////////////////////////////////////////////////////////////////////////////
SetNew::SetNew() :
   params_(std::make_unique<Params>(2000ms, 0, SecureBinaryData{})),
   setNewPassphrase_(nullptr)
{}

////
SetNew::SetNew(std::chrono::milliseconds kdfUnlockTime,
   uint32_t kdfMemTarget,
   SecureBinaryData passphrase) :
   params_(std::make_unique<Params>(
      kdfUnlockTime, kdfMemTarget, std::move(passphrase))),
   setNewPassphrase_(nullptr)
{}

////
SetNew::SetNew(const SetNewPassFunc& func) :
   setNewPassphrase_(func)
{}

////
SetNew::SetNew(const SetNew& rhs) :
   setNewPassphrase_(rhs.setNewPassphrase_)
{
   if (rhs.params_ != nullptr) {
      params_ = std::make_unique<Params>(
         rhs.params_->unlockMs,
         rhs.params_->memTargetMB,
         rhs.params_->passphrase);
   }
}

////////////////////////////////////////////////////////////////////////////////
const Params& SetNew::get() const
{
   if (params_ == nullptr) {
      if (setNewPassphrase_ == nullptr) {
         throw std::runtime_error("cannot get passphrase");
      }

      params_ = std::move(setNewPassphrase_());
      if (params_ == nullptr || params_->passphrase.empty()) {
         throw std::runtime_error("passphrase was not set");
      }
   }
   return *params_;
}

////
SetNew SetNew::copy() const
{
   //allows for a copy when the object is setup with
   //a lambda that wasnt called yet
   if (params_ != nullptr || setNewPassphrase_ == nullptr) {
      throw std::runtime_error("this SetNew is not copyable");
   }
   return SetNew(setNewPassphrase_);
}

////
std::unique_ptr<Params> SetNew::moveParams()
{
   if (params_->passphrase.empty()) {
      throw std::runtime_error("cannot get passphrase");
   }
   return std::move(params_);
}

////////////////////////////////////////////////////////////////////////////////
UnlockFunc SetNew::getUnlockFunc() const
{
   if (params_ == nullptr) {
      throw std::runtime_error("call get first");
   }

   return [&pass=params_->passphrase]
   (const std::set<Armory::Wallets::EncryptionKeyId>&)->SecureBinaryData {

      return pass;
   };
}