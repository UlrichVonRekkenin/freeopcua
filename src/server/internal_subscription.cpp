#include "internal_subscription.h"

#include <boost/thread/locks.hpp>

namespace OpcUa
{
  namespace Internal
  {

    InternalSubscription::InternalSubscription(SubscriptionServiceInternal& service, const SubscriptionData& data, const NodeID& SessionAuthenticationToken, std::function<void (PublishResult)> callback, bool debug)
      : Service(service)
      , AddressSpace(Service.GetAddressSpace())
      , Data(data)
      , CurrentSession(SessionAuthenticationToken)
      , Callback(callback)
      , io(service.GetIOService())
      , Timer(io, boost::posix_time::milliseconds(data.RevisedPublishingInterval))
      , LifeTimeCount(data.RevisedLifetimeCount)
      , Debug(debug)
    {
      Timer.async_wait([&](const boost::system::error_code& error){ this->PublishResults(error); });
    }
    
    InternalSubscription::~InternalSubscription()
    {
      DeleteAllMonitoredItems();
      Stop();
    }

    void InternalSubscription::Stop()
    {
      if (!TimerStopped)
        Timer.cancel();
    }

    void InternalSubscription::DeleteAllMonitoredItems()
    {
      if (Debug) std::cout << "InternalSubscription | Deleting all monitoreditems" << std::endl; 
      std::vector<IntegerID> handles;
      for (auto pair : MonitoredItemsMap)
      {
        handles.push_back(pair.first);
      }
      DeleteMonitoredItemsIds(handles);
    }

    bool InternalSubscription::HasExpired()
    {
      bool expired = KeepAliveCount > LifeTimeCount ;
      if ( expired )
      {
        if (Debug) { std::cout << "InternalSubscription | Subscription has expired " << KeepAliveCount << "  " << LifeTimeCount << std::endl; }
      }
      return expired;
    }

    void InternalSubscription::PublishResults(const boost::system::error_code& error)
    {
      if ( error || HasExpired() )
      {
        TimerStopped = true;
        if (Debug) { std::cout << "InternalSubscription | boost::asio called us with an error code: " << error.value() << ", this probably means out timer has been deleted. Stopping subscription" << std::endl; }
        return; //It is very important to return, instance of InternalSubscription may have been deleted!
      }
      if ( HasPublishResult() && Service.PopPublishRequest(CurrentSession) ) //Check we received a publishrequest before sening respomse
      {

        std::vector<PublishResult> results = PopPublishResult();
        if (results.size() > 0 )
        {
          if (Debug) { std::cout << "InternalSubscription | Subscription has " << results.size() << " results, calling callback" << std::endl; }
          if ( Callback )
          {
            Callback(results[0]);
          }
          else
          {
            if (Debug) std::cout << "InternalSubcsription | No callback defined for this subscription" << std::endl;
          }
         }
      }
      TimerStopped = false;
      Timer.expires_at(Timer.expires_at() + boost::posix_time::milliseconds(Data.RevisedPublishingInterval));
      Timer.async_wait([&](const boost::system::error_code& error){ this->PublishResults(error); });
    }

    bool InternalSubscription::HasPublishResult()
    {
      boost::unique_lock<boost::shared_mutex> lock(DbMutex);
      
      if ( Startup || ! MonitoredItemsTriggered.empty() || ! EventTriggered.empty() ) 
      {
        return true;
      }
      if ( KeepAliveCount > Data.RevizedMaxKeepAliveCount ) //we need to send keepalive notification
      {
        if (Debug) std::cout << "InternalSubscription | KeepAliveCount " << KeepAliveCount << " is > than MaxKeepAliveCount " <<  Data.RevizedMaxKeepAliveCount << " sending publish event" << std::endl; 
        return true;
      }
      ++KeepAliveCount;
      return false;

    }

    std::vector<PublishResult> InternalSubscription::PopPublishResult()
    {
      boost::unique_lock<boost::shared_mutex> lock(DbMutex);

      //std::cout << "PopPublishresult for subscription: " << Data.ID << " with " << MonitoredItemsTriggered.size() << " triggered items in queue" << std::endl;
      PublishResult result;
      result.SubscriptionID = Data.ID;
      result.Message.PublishTime = CurrentDateTime();

      if ( ! MonitoredItemsTriggered.empty() )
      {
        NotificationData data = GetNotificationData();
        result.Message.Data.push_back(data);
        result.Statuses.push_back(StatusCode::Good);
      }
          
      if ( ! EventTriggered.empty() )
      {
        if (Debug) { std::cout << "InternalSubcsription | Subscription " << Data.ID << " has " << EventTriggered.size() << " events to send to client" << std::endl; }
        EventNotificationList notif;
        for ( EventFieldList ef: EventTriggered )
        {
          notif.Events.push_back(ef);
        }
        EventTriggered.clear();
        NotificationData data(notif);
        result.Message.Data.push_back(data);
        result.Statuses.push_back(StatusCode::Good);
      }


      // FIXME: also add statuschange notification since they can be send in same result
      
      KeepAliveCount = 0;
      Startup = false;

      result.Message.SequenceID = NotificationSequence;
      ++NotificationSequence;
      result.MoreNotifications = false;
      for (const PublishResult& res: NotAcknowledgedResults)
      {
        result.AvailableSequenceNumber.push_back(res.Message.SequenceID);
      }
      NotAcknowledgedResults.push_back(result);
      if (Debug) { std::cout << "InternalSubcsription | Sending Notification with " << result.Message.Data.size() << " notifications"  << std::endl; }
      std::vector<PublishResult> resultlist;
      resultlist.push_back(result);

      return resultlist;
    };

    NotificationData InternalSubscription::GetNotificationData()
    {
      DataChangeNotification notification;
      for ( const MonitoredItems& monitoreditem: MonitoredItemsTriggered)
      {
        notification.Notification.push_back(monitoreditem);
      }
      MonitoredItemsTriggered.clear();
      NotificationData data(notification);
      return data;
    }

    void InternalSubscription::NewAcknowlegment(const SubscriptionAcknowledgement& ack)
    {
      NotAcknowledgedResults.remove_if([&](PublishResult res){ return ack.SequenceNumber == res.Message.SequenceID; });
    }
    

    CreateMonitoredItemsResult InternalSubscription::CreateMonitoredItem(const MonitoredItemRequest& request)
    {
      if (Debug) std::cout << "SubscriptionService| Creating monitored item." << std::endl;
      boost::unique_lock<boost::shared_mutex> lock(DbMutex);

      CreateMonitoredItemsResult result;
      uint32_t callbackHandle = 0;
      result.MonitoredItemID = ++LastMonitoredItemID;
      if (request.ItemToMonitor.Attribute == AttributeID::EVENT_NOTIFIER )
      {
        if (Debug) std::cout << "SubscriptionService| Subscribed o event notifier " << std::endl;
        //client want to subscribe to events
        //FIXME: check attribute EVENT notifier is set for the node
        MonitoredEvents[request.ItemToMonitor.Node] = result.MonitoredItemID;
      }
      else
      {
        if (Debug) std::cout << "SubscriptionService| Subscribing to data chanes in the address space." << std::endl;
        IntegerID id = result.MonitoredItemID;
        callbackHandle = AddressSpace.AddDataChangeCallback(request.ItemToMonitor.Node, request.ItemToMonitor.Attribute, [this, id] (const OpcUa::NodeID& nodeId, OpcUa::AttributeID attr, const DataValue& value)
          {
            this->DataChangeCallback(id, value);
          });

        if (callbackHandle == 0)
        {
          if (Debug) std::cout << "SubscriptionService| ERROR: address returned zero handle." << std::endl;
          --LastMonitoredItemID; //revert increment 
          result.Status = OpcUa::StatusCode::BadNodeAttributesInvalid;
          return result;
        }
      }
      result.Status = OpcUa::StatusCode::Good;
      result.RevisedSamplingInterval = Data.RevisedPublishingInterval; //Force our own rate
      result.RevizedQueueSize = request.Parameters.QueueSize; // We should check that value, maybe set to a default...
      result.Filter = request.Parameters.Filter;
      //res.FilterResult = //We can omit that one if we do not change anything in filter
      DataMonitoredItems mdata;
      mdata.Parameters = result;
      mdata.Mode = request.Mode;
      mdata.ClientHandle = request.Parameters.ClientHandle;
      mdata.CallbackHandle = callbackHandle;
      MonitoredItemsMap[result.MonitoredItemID] = mdata;
      if (Debug) std::cout << "Created MonitoredItem with id: " << result.MonitoredItemID << " and client handle " << mdata.ClientHandle << std::endl;
      //Forcing event, 
      TriggerDataChangeEvent(mdata, request.ItemToMonitor);

      return result;
    }

    void InternalSubscription::TriggerDataChangeEvent(DataMonitoredItems monitoreditems, AttributeValueID attrval)
    {
      if (Debug) { std::cout << "InternalSubcsription | Manual Trigger of DataChangeEvent for sub: " << Data.ID << " and clienthandle: " << monitoreditems.ClientHandle << std::endl; }
      ReadParameters params;
      params.AttributesToRead.push_back(attrval);
      std::vector<DataValue> vals = AddressSpace.Read(params);

      MonitoredItems event;
      event.ClientHandle = monitoreditems.ClientHandle; 
      event.Value = vals[0];
      MonitoredItemsTriggered.push_back(event);
    }

    std::vector<StatusCode> InternalSubscription::DeleteMonitoredItemsIds(const std::vector<IntegerID>& monitoreditemsids)
    {
      std::vector<StatusCode> results;
      for (const IntegerID& handle: monitoreditemsids)
      {
        if (Debug) std::cout << "InternalSubcsription | Deleting Monitoreditemsid: " << handle << std::endl;
        for (auto pair : MonitoredEvents)
        {
          if ( pair.second == handle )
          {
            MonitoredEvents.erase(pair.first);
            break;
          }
        }
        
        MonitoredItemsMapType::iterator it = MonitoredItemsMap.find(handle);
        if ( it == MonitoredItemsMap.end() )
        {
          results.push_back(StatusCode::BadMonitoredItemIdInvalid);
        }
        else
        {
          if (it->second.CallbackHandle != 0){ //if 0 this monitoreditem did not use callbacks
            AddressSpace.DeleteDataChangeCallback(it->second.CallbackHandle);
          }
          MonitoredItemsMap.erase(handle);
          results.push_back(StatusCode::Good);
        }
      }
      return results;
    }



    void InternalSubscription::DataChangeCallback(const IntegerID& m_id, const DataValue& value)
    {
      boost::unique_lock<boost::shared_mutex> lock(DbMutex);

      MonitoredItems event;
      MonitoredItemsMapType::iterator it_monitoreditem = MonitoredItemsMap.find(m_id);
      if ( it_monitoreditem == MonitoredItemsMap.end()) 
      {
        std::cout << "InternalSubcsription | DataChangeCallback called for unknown item" << std::endl;
        return ;
      }

      event.ClientHandle = it_monitoreditem->second.ClientHandle; 
      event.Value = value;
      if (Debug) { std::cout << "InternalSubcsription | Enqueued DataChange triggered item for sub: " << Data.ID << " and clienthandle: " << event.ClientHandle << std::endl; }
      MonitoredItemsTriggered.push_back(event);
    }

    void InternalSubscription::TriggerEvent(NodeID node, Event event)
    {
      boost::shared_lock<boost::shared_mutex> lock(DbMutex);

      MonitoredEventsMap::iterator it = MonitoredEvents.find(node);
      if ( it == MonitoredEvents.end() )
      {
        if (Debug) std::cout << "InternalSubcsription | Subscription: " << Data.ID << " has no subcsription for this event" << std::endl;
        return;
      }
      lock.unlock();//Enqueue vill need to set a unique lock
      EnqueueEvent(it->second, event);
    }

    bool InternalSubscription::EnqueueEvent(IntegerID monitoreditemid, const Event& event)
    {
      boost::unique_lock<boost::shared_mutex> lock(DbMutex);
      if (Debug) { std::cout << "InternalSubcsription | Enqueing event to be send" << std::endl; }

      //Find monitoredItem 
      std::map<IntegerID, DataMonitoredItems>::iterator mii_it =  MonitoredItemsMap.find( monitoreditemid );
      if  (mii_it == MonitoredItemsMap.end() ) 
      {
        if (Debug) std::cout << "InternalSubcsription | monitoreditem " << monitoreditemid << " is already deleted" << std::endl; 
        return false;
      }
          
      //Check filter against event data and create EventFieldList to send
      //FIXME: Here we should also check event agains WhereClause of filter
      EventFieldList fieldlist;
      fieldlist.ClientHandle = mii_it->second.ClientHandle; 
      fieldlist.EventFields = GetEventFields(mii_it->second.Parameters.Filter.Event, event);
      EventTriggered.push_back(fieldlist);
      return true;
    }

    std::vector<Variant> InternalSubscription::GetEventFields(const EventFilter& filter, const Event& event)
    {
      //Go through filter and add value og matches as in spec
      std::vector<Variant> fields;
      if(Debug) std::cout << "InternalSubscription | InternalGetEventField " << filter.SelectClauses.size() << std::endl;
      for (SimpleAttributeOperand sattr : filter.SelectClauses)
      {
        if(Debug) std::cout << "InternalSubscription | BrowsePAth size " << sattr.BrowsePath.size() << std::endl;
        if ( sattr.BrowsePath.size() == 0 )
        {
          fields.push_back(event.GetValue(sattr.Attribute));
        }
        else
        {
          if(Debug) std::cout << "InternalSubscription | sending value for : " << sattr.BrowsePath[0] << std::endl;
          if ( sattr.BrowsePath[0] == QualifiedName("EventID", 0) )
          {
            fields.push_back(event.EventId);
          }
          else if ( sattr.BrowsePath[0] == QualifiedName("EventType", 0) )
          {
            fields.push_back(event.EventType);
          }
          else if ( sattr.BrowsePath[0] == QualifiedName("SourceNode", 0) )
          {
            fields.push_back(event.SourceNode);
          }
          else if ( sattr.BrowsePath[0] == QualifiedName("SourceName", 0) )
          {
            fields.push_back(event.SourceName);
          }
          else if ( sattr.BrowsePath[0] == QualifiedName("Message", 0) )
          {
            fields.push_back(event.Message);
          }
          else if ( sattr.BrowsePath[0] == QualifiedName("Severity", 0) )
          {
            fields.push_back(event.Severity);
          }
          else if ( sattr.BrowsePath[0] == QualifiedName("LocalTime", 0) )
          {
            fields.push_back(event.LocalTime);
          }
          else if ( sattr.BrowsePath[0] == QualifiedName("ReceiveTime", 0) )
          {
            fields.push_back(event.ReceiveTime);
          }
          else if ( sattr.BrowsePath[0] == QualifiedName("Time", 0) )
          {
            fields.push_back(event.Time);
          }
          else
          {
            fields.push_back(event.GetValue(sattr.BrowsePath));
          }
        }
      }
      return fields;
    }


  }
}

