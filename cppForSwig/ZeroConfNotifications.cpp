////////////////////////////////////////////////////////////////////////////////
//                                                                            //
//  Copyright (C) 2020-2021, goatpig                                          //
//  Distributed under the MIT license                                         //
//  See LICENSE-MIT or https://opensource.org/licenses/MIT                    //
//                                                                            //
////////////////////////////////////////////////////////////////////////////////

#include <list>

#include "ZeroConfNotifications.h"
#include "ZeroConfUtils.h"
#include "ZeroConf.h"
#include "BDM_Server.h"
#include "LedgerEntry.h"
#include "BlockchainDatabase/txio.h"

using namespace std;

///////////////////////////////////////////////////////////////////////////////
//
// ZeroConfCallbacks
//
///////////////////////////////////////////////////////////////////////////////
ZeroConfCallbacks_BDV::ZeroConfCallbacks_BDV(Clients* clientsPtr) :
   clientsPtr_(clientsPtr)
{
   auto requestLambda = [this](void)->void
   {
      processNotifRequests();
   };

   requestThread_ = thread(requestLambda);
}

///////////////////////////////////////////////////////////////////////////////
ZeroConfCallbacks_BDV::~ZeroConfCallbacks_BDV()
{
   requestQueue_.terminate();
   if (requestThread_.joinable()) {
      requestThread_.join();
   }
}

///////////////////////////////////////////////////////////////////////////////
std::set<BdvIdKey> ZeroConfCallbacks_BDV::hasScrAddr(
   const BinaryDataRef& addr) const
{
   //this is slow, needs improved
   std::set<BdvIdKey> result;

   auto bdvMap = clientsPtr_->BDVs_.get();
   for (const auto& bdv_pair : bdvMap) {
      if (bdv_pair.second->hasScrAddress(addr)) {
         result.emplace(bdv_pair.first);
      }
   }
   return result;
}

///////////////////////////////////////////////////////////////////////////////
void ZeroConfCallbacks_BDV::pushZcNotification(
   std::shared_ptr<MempoolSnapshot> ss,
   std::shared_ptr<KeyAddrMap> newZcKeys,
   std::map<BdvIdKey, ParsedZCData> flaggedBDVs,
   BdvIdKey bdvId,
   std::map<BinaryData, std::shared_ptr<WatcherTxBody>>& watcherMap)
{
   auto requestPtr = make_shared<
      ZeroConfCallbacks_BDV::ZcNotifRequest_Success>(
      bdvId,
      ss, newZcKeys, flaggedBDVs,
      watcherMap);
   requestQueue_.push_back(std::move(requestPtr));
}

///////////////////////////////////////////////////////////////////////////////
void ZeroConfCallbacks_BDV::pushZcError(
   BdvIdKey bdvID, const BinaryData& hash,
   ArmoryErrorCodes errCode, const string& verbose)
{
   auto requestPtr = make_shared<ZeroConfCallbacks_BDV::ZcNotifRequest_Error>(
      bdvID, hash, errCode, verbose);
   requestQueue_.push_back(move(requestPtr));
}

///////////////////////////////////////////////////////////////////////////////
void ZeroConfCallbacks_BDV::processNotifRequests()
{
   while (true) {
      std::shared_ptr<ZeroConfCallbacks_BDV::ZcNotifRequest> notifReqPtr;
      try {
         notifReqPtr = requestQueue_.pop_front();
      } catch (const Armory::Threading::StopBlockingLoop&) {
         break;
      }

      switch (notifReqPtr->type_)
      {
         case ZcNotifRequestType::Success:
         {
            auto reqPtr = dynamic_pointer_cast<
               ZeroConfCallbacks_BDV::ZcNotifRequest_Success>(notifReqPtr);
            if (reqPtr == nullptr) {
               LOGWARN << "zc notification request type mismatch";
               break;
            }

            //build notifications for each BDV
            for (auto& bdvObj : reqPtr->flaggedBDVs_) {
               //get bdv object
               auto bdvPtr = clientsPtr_->BDVs_.get(bdvObj.first);
               if (bdvPtr == nullptr) {
                  LOGWARN << "pushing zc notification with invalid bdvid";
                  continue;
               }

               //create notif packet
               ZcNotificationPacket notificationPacket(bdvObj.first);
               notificationPacket.ssPtr_ = reqPtr->ssPtr_;

               //set txio map
               for (auto& sa : bdvObj.second.scrAddrs_) {
                  auto txioKeys = reqPtr->ssPtr_->getTxioKeysForScrAddr(sa);
                  if (txioKeys.empty()) {
                     continue;
                  }

                  //copy the txiomap for this scrAddr over to the notification object
                  notificationPacket.scrAddrToTxioKeys_.emplace(sa, txioKeys);
               }

               //set invalidated keys
               if (!bdvObj.second.invalidatedKeys_.empty()) {
                  notificationPacket.purgePacket_ = std::make_shared<ZcPurgePacket>();
                  notificationPacket.purgePacket_->invalidatedZcKeys_ =
                     std::move(bdvObj.second.invalidatedKeys_);
               }

               //set the primary requestor if this is the caller bdv
               if (bdvObj.first == reqPtr->bdvId_) {
                  notificationPacket.primaryRequestor_ = reqPtr->bdvId_;
               }

               //set new zc keys
               notificationPacket.newKeysAndScrAddr_ = reqPtr->newZcKeys_;

               //create notif and push to bdv
               auto notifPacket = std::make_shared<BDV_Notification_Packet>();
               notifPacket->bdvPtr = bdvPtr;
               notifPacket->notifPtr =
                  std::make_shared<BDV_Notification_ZC>(notificationPacket);
               clientsPtr_->innerBDVNotifStack_.push_back(std::move(notifPacket));
            }

            //process duplicate broadcast requests
            for (auto& watcherObj : reqPtr->watcherMap_) {
               if (watcherObj.second->extraRequestors_.empty()) {
                  continue;
               }

               if (!reqPtr->ssPtr_->hasHash(watcherObj.first)) {
                  //tx was not added to mempool, skip
                  continue;
               }

               //tx was added to mempool, report already-in-mempool error to
               //duplicate requestors
               for (auto& extra : watcherObj.second->extraRequestors_) {
                  pushZcError(extra, watcherObj.first,
                     ArmoryErrorCodes::ZcBroadcast_AlreadyInMempool,
                     "Extra requestor broadcast error: Already in mempool");
               }
            }

            break;
         }

         case ZcNotifRequestType::Error:
         {
            auto reqPtr = dynamic_pointer_cast<
               ZeroConfCallbacks_BDV::ZcNotifRequest_Error>(notifReqPtr);
            if (reqPtr == nullptr) {
               LOGWARN << "zc notification request type mismatch";
               break;
            }

            auto bdvPtr = clientsPtr_->BDVs_.get(reqPtr->bdvId_);
            if (bdvPtr == nullptr) {
               LOGWARN << "pushed zc error with invalid bdvid";
               return;
            }

            auto notifPacket = std::make_shared<BDV_Notification_Packet>();
            notifPacket->bdvPtr = bdvPtr;
            notifPacket->notifPtr = std::make_shared<BDV_Notification_Error>(
               reqPtr->bdvId_,
               (int)reqPtr->errCode_, reqPtr->hash_, reqPtr->verbose_);
            clientsPtr_->innerBDVNotifStack_.push_back(move(notifPacket));

            break;
         }

         default:
            throw std::runtime_error("unexpected zc notification request type");
      }
   }
}

///////////////////////////////////////////////////////////////////////////////
ZeroConfCallbacks_BDV::ZcNotifRequest::~ZcNotifRequest()
{}
