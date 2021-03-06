/// @author Olivier Roulet-dubonnet 2014
/// @brief  SubcsriptionService implementation.
/// @license GNU LGPL
///
/// Distributed under the GNU LGPL License
/// (See accompanying file LICENSE or copy at
/// http://www.gnu.org/licenses/lgpl.html)
///

#include "subscription_service_internal.h"

#include <boost/thread/locks.hpp>

namespace
{
  OpcUa::ByteString GenerateEventId()
  {
    //stupid id generator
    OpcUa::ByteString str;
    for (int i = 0; i < 8; i++)
      {
        int32_t val = rand() % std::numeric_limits<int32_t>::max();
        str.Data.push_back(val);
      }
    return str;
  }
}

namespace OpcUa
{
  namespace Internal
  {

    SubscriptionServiceInternal::SubscriptionServiceInternal(Server::AddressSpace::SharedPtr addressspace, boost::asio::io_service& ioService, bool debug)
      : io(ioService)
      , AddressSpace(addressspace)
      , Debug(debug)
    {
    }

    SubscriptionServiceInternal::~SubscriptionServiceInternal()
    {
    }

    Server::AddressSpace& SubscriptionServiceInternal::GetAddressSpace()
    {
      return *AddressSpace;
    }

    boost::asio::io_service& SubscriptionServiceInternal::GetIOService()
    {
      return io;
    }

    void SubscriptionServiceInternal::DeleteAllSubscriptions()
    {
      if (Debug) std::cout << "SubscriptionService | Deleting all subscriptions." << std::endl;

      std::vector<uint32_t> ids(SubscriptionsMap.size());
      {
        boost::shared_lock<boost::shared_mutex> lock(DbMutex);
        std::transform(SubscriptionsMap.begin(), SubscriptionsMap.end(), ids.begin(), [](const SubscriptionsIdMap::value_type& i){return i.first;});
      }

      DeleteSubscriptions(ids);
    }

    std::vector<StatusCode> SubscriptionServiceInternal::DeleteSubscriptions(const std::vector<uint32_t>& subscriptions)
    {
      boost::unique_lock<boost::shared_mutex> lock(DbMutex);

      std::vector<StatusCode> result;
      for (const uint32_t& subid: subscriptions)
      {
        SubscriptionsIdMap::iterator itsub = SubscriptionsMap.find(subid);
        if ( itsub == SubscriptionsMap.end())
        {
          std::cout << "SubscriptionService | Error, got request to delete non existing Subscription: " << subid << std::endl;
          result.push_back(StatusCode::BadSubscriptionIdInvalid);
        }
        else
        {
          if (Debug) std::cout << "SubscriptionService | Deleting Subscription: " << subid << std::endl;
          itsub->second->Stop();
          SubscriptionsMap.erase(subid);
          result.push_back(StatusCode::Good);
        }
      }
      return result;
    }

    SubscriptionData SubscriptionServiceInternal::CreateSubscription(const CreateSubscriptionRequest& request, std::function<void (PublishResult)> callback)
    {
      boost::unique_lock<boost::shared_mutex> lock(DbMutex);

      SubscriptionData data;
      data.SubscriptionId = ++LastSubscriptionId;
      data.RevisedLifetimeCount = request.Parameters.RequestedLifetimeCount;
      data.RevisedPublishingInterval = request.Parameters.RequestedPublishingInterval;
      data.RevisedMaxKeepAliveCount = request.Parameters.RequestedMaxKeepAliveCount;
      if (Debug) std::cout << "SubscriptionService | Creating Subscription with Id: " << data.SubscriptionId << std::endl;

      std::shared_ptr<InternalSubscription> sub(new InternalSubscription(*this, data, request.Header.SessionAuthenticationToken, callback, Debug));
      sub->Start();
      SubscriptionsMap[data.SubscriptionId] = sub;
      return data;
    }

    std::vector<MonitoredItemCreateResult> SubscriptionServiceInternal::CreateMonitoredItems(const MonitoredItemsParameters& params)
    {
      boost::unique_lock<boost::shared_mutex> lock(DbMutex);

      std::vector<MonitoredItemCreateResult> data;

      SubscriptionsIdMap::iterator itsub = SubscriptionsMap.find(params.SubscriptionId);
      if ( itsub == SubscriptionsMap.end()) //SubscriptionId does not exist, return errors for all items
      {
        for (int j=0; j<(int)params.ItemsToCreate.size(); j++)
        {
          MonitoredItemCreateResult res;
          res.Status = StatusCode::BadSubscriptionIdInvalid;
          data.push_back(res);
        }
        return data;
      }

      for (const MonitoredItemCreateRequest& req: params.ItemsToCreate) //FIXME: loop could be in InternalSubscription
      {
        MonitoredItemCreateResult result = itsub->second->CreateMonitoredItem(req);
        data.push_back(result);
      }
      return data;

    }

    std::vector<StatusCode> SubscriptionServiceInternal::DeleteMonitoredItems(const DeleteMonitoredItemsParameters& params)
    {
      boost::unique_lock<boost::shared_mutex> lock(DbMutex);

      std::vector<StatusCode> results;

      SubscriptionsIdMap::iterator itsub = SubscriptionsMap.find(params.SubscriptionId);
      if ( itsub == SubscriptionsMap.end()) //SubscriptionId does not exist, return errors for all items
      {
        for (int j=0; j<(int)params.MonitoredItemIds.size(); j++)
        {
          results.push_back(StatusCode::BadSubscriptionIdInvalid);
        }
        return results;
      }

      results = itsub->second->DeleteMonitoredItemsIds(params.MonitoredItemIds);
      return results;
    }

    void SubscriptionServiceInternal::Publish(const PublishRequest& request)
    {
      boost::unique_lock<boost::shared_mutex> lock(DbMutex);

      if ( PublishRequestQueues[request.Header.SessionAuthenticationToken] < 100 )
      {
        PublishRequestQueues[request.Header.SessionAuthenticationToken] += 1;
      }
      //FIXME: else spec says we should return error to warn client

      for (SubscriptionAcknowledgement ack:  request.SubscriptionAcknowledgements)
      {
        SubscriptionsIdMap::iterator sub_it = SubscriptionsMap.find(ack.SubscriptionId);
        if ( sub_it != SubscriptionsMap.end())
        {
          sub_it->second->NewAcknowlegment(ack);
        }
      }
    }

    RepublishResponse SubscriptionServiceInternal::Republish(const RepublishParameters& params)
    {
      boost::shared_lock<boost::shared_mutex> lock(DbMutex);
      
      SubscriptionsIdMap::iterator sub_it = SubscriptionsMap.find(params.SubscriptionId);
      if ( sub_it == SubscriptionsMap.end())
      {
        RepublishResponse response;
        response.Header.ServiceResult = StatusCode::BadSubscriptionIdInvalid;
        return response;
      }
      return sub_it->second->Republish(params);
    }


    bool SubscriptionServiceInternal::PopPublishRequest(NodeId node)
    {
      std::map<NodeId, uint32_t>::iterator queue_it = PublishRequestQueues.find(node);
      if ( queue_it == PublishRequestQueues.end() )
      {
        std::cout << "SubscriptionService | Error request for publish queue for unknown session: " << node << " queue are available for: ";
        for ( auto i: PublishRequestQueues ){
          std::cout << "    " << i.first ;
        }
        std::cout << std::endl;
        return false;
      }
      else
      {
        if ( queue_it->second == 0 )
        {
          std::cout << "SubscriptionService | Missing publish request, cannot send response for session: " << node << std::endl;
          return false;
        }
        else
        {
          --queue_it->second;
          return true;
        }
      }
    }

    void SubscriptionServiceInternal::TriggerEvent(NodeId node, Event event)
    {
      boost::shared_lock<boost::shared_mutex> lock(DbMutex);

      //A new id must be generated every time we trigger an event, 
      //if user have not set it manually we force something
      if ( event.EventId.Data.empty() ) 
      {
        event.EventId = GenerateEventId();
      }

      for (auto sub : SubscriptionsMap)
      {
        sub.second->TriggerEvent(node, event);
      }

    }

  } // namespace Internal

  namespace Server
  {

    SubscriptionService::UniquePtr CreateSubscriptionService(std::shared_ptr<Server::AddressSpace> addressspace, boost::asio::io_service& io, bool debug)
    {
      return SubscriptionService::UniquePtr(new Internal::SubscriptionServiceInternal(addressspace, io, debug));
    }

  }
}

