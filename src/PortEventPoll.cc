/* <!-- copyright */
/*
 * aria2 - The high speed download utility
 *
 * Copyright (C) 2010 Tatsuhiro Tsujikawa
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 * You must obey the GNU General Public License in all respects
 * for all of the code used other than OpenSSL.  If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so.  If you
 * do not wish to do so, delete this exception statement from your
 * version.  If you delete this exception statement from all source
 * files in the program, then also delete it here.
 */
/* copyright --> */
#include "PortEventPoll.h"

#include <cstring>
#include <algorithm>
#include <numeric>

#include "Command.h"
#include "LogFactory.h"
#include "Logger.h"

namespace aria2 {

PortEventPoll::KSocketEntry::KSocketEntry(sock_t s):
  SocketEntry<KCommandEvent, KADNSEvent>(s) {}

int accumulateEvent(int events, const PortEventPoll::KEvent& event)
{
  return events|event.getEvents();
}

PortEventPoll::A2PortEvent PortEventPoll::KSocketEntry::getEvents()
{
  A2PortEvent portEvent;
  portEvent.socketEntry = this;
#ifdef ENABLE_ASYNC_DNS
  portEvent.events =
    std::accumulate(_adnsEvents.begin(),
                    _adnsEvents.end(),
                    std::accumulate(_commandEvents.begin(),
                                    _commandEvents.end(), 0, accumulateEvent),
                    accumulateEvent);
#else // !ENABLE_ASYNC_DNS
  portEvent.events =
    std::accumulate(_commandEvents.begin(), _commandEvents.end(), 0,
                    accumulateEvent);

#endif // !ENABLE_ASYNC_DNS
  return portEvent;
}

PortEventPoll::PortEventPoll():
  _portEventsSize(PORT_EVENTS_SIZE),
  _portEvents(new port_event_t[_portEventsSize]),
  _logger(LogFactory::getInstance())
{
  _port = port_create();
}

PortEventPoll::~PortEventPoll()
{
  if(_port != -1) {
    int r;
    while((r = close(_port)) == -1 && errno == EINTR);
    if(r == -1) {
      _logger->error("Error occurred while closing port %d: %s",
                     _port, strerror(errno));
    }
  }
  delete [] _portEvents;
}

bool PortEventPoll::good() const
{
  return _port != -1;
}

void PortEventPoll::poll(const struct timeval& tv)
{
  struct timespec timeout = { tv.tv_sec, tv.tv_usec*1000 };
  int res;
  uint_t nget = 1;
  // If port_getn was interrupted by signal, it can consume events but
  // not updat nget!. For this very annoying bug, we have to check
  // actually event is filled or not.
  _portEvents[0].portev_user = (void*)-1;
  res = port_getn(_port, _portEvents, _portEventsSize, &nget, &timeout);
  if(res == 0 ||
     (res == -1 && (errno == ETIME || errno == EINTR) &&
      _portEvents[0].portev_user != (void*)-1)) {
    if(_logger->debug()) {
      _logger->debug("nget=%u", nget);
    }
    for(uint_t i = 0; i < nget; ++i) {
      const port_event_t& pev = _portEvents[i];
      KSocketEntry* p = reinterpret_cast<KSocketEntry*>(pev.portev_user);
      p->processEvents(pev.portev_events);
      int r = port_associate(_port, PORT_SOURCE_FD, pev.portev_object,
                             p->getEvents().events, p);
      if(r == -1) {
        _logger->error("port_associate failed for file descriptor %d: cause %s",
                       pev.portev_object, strerror(errno));
      }
    }
  }
#ifdef ENABLE_ASYNC_DNS
  // It turns out that we have to call ares_process_fd before ares's
  // own timeout and ares may create new sockets or closes socket in
  // their API. So we call ares_process_fd for all ares_channel and
  // re-register their sockets.
  for(std::deque<SharedHandle<KAsyncNameResolverEntry> >::iterator i =
        _nameResolverEntries.begin(), eoi = _nameResolverEntries.end();
      i != eoi; ++i) {
    (*i)->processTimeout();
    (*i)->removeSocketEvents(this);
    (*i)->addSocketEvents(this);
  }
#endif // ENABLE_ASYNC_DNS

  // TODO timeout of name resolver is determined in Command(AbstractCommand,
  // DHTEntryPoint...Command)
}

static int translateEvents(EventPoll::EventType events)
{
  int newEvents = 0;
  if(EventPoll::EVENT_READ&events) {
    newEvents |= PortEventPoll::IEV_READ;
  }
  if(EventPoll::EVENT_WRITE&events) {
    newEvents |= PortEventPoll::IEV_WRITE;
  }
  if(EventPoll::EVENT_ERROR&events) {
    newEvents |= PortEventPoll::IEV_ERROR;
  }
  if(EventPoll::EVENT_HUP&events) {
    newEvents |= PortEventPoll::IEV_HUP;
  }
  return newEvents;
}

bool PortEventPoll::addEvents(sock_t socket,
                              const PortEventPoll::KEvent& event)
{
  SharedHandle<KSocketEntry> socketEntry(new KSocketEntry(socket));
  std::deque<SharedHandle<KSocketEntry> >::iterator i =
    std::lower_bound(_socketEntries.begin(), _socketEntries.end(), socketEntry);
  int r = 0;
  if(i != _socketEntries.end() && (*i) == socketEntry) {
    event.addSelf(*i);
    A2PortEvent pv = (*i)->getEvents();
    r = port_associate(_port, PORT_SOURCE_FD, (*i)->getSocket(),
                       pv.events, pv.socketEntry);
  } else {
    _socketEntries.insert(i, socketEntry);
    if(_socketEntries.size() > _portEventsSize) {
      _portEventsSize *= 2;
      delete [] _portEvents;
      _portEvents = new port_event_t[_portEventsSize];
    }
    event.addSelf(socketEntry);
    A2PortEvent pv = socketEntry->getEvents();
    r = port_associate(_port, PORT_SOURCE_FD, socketEntry->getSocket(),
                       pv.events, pv.socketEntry);
  }
  if(r == -1) {
    if(_logger->debug()) {
      _logger->debug("Failed to add socket event %d:%s",
                     socket, strerror(errno));
    }
    return false;
  } else {
    return true;
  }
}

bool PortEventPoll::addEvents(sock_t socket, Command* command,
                              EventPoll::EventType events)
{
  int portEvents = translateEvents(events);
  return addEvents(socket, KCommandEvent(command, portEvents));
}

#ifdef ENABLE_ASYNC_DNS
bool PortEventPoll::addEvents(sock_t socket, Command* command, int events,
                              const SharedHandle<AsyncNameResolver>& rs)
{
  return addEvents(socket, KADNSEvent(rs, command, socket, events));
}
#endif // ENABLE_ASYNC_DNS

bool PortEventPoll::deleteEvents(sock_t socket,
                                 const PortEventPoll::KEvent& event)
{
  SharedHandle<KSocketEntry> socketEntry(new KSocketEntry(socket));
  std::deque<SharedHandle<KSocketEntry> >::iterator i =
    std::lower_bound(_socketEntries.begin(), _socketEntries.end(), socketEntry);
  if(i != _socketEntries.end() && (*i) == socketEntry) {
    event.removeSelf(*i);
    int r = 0;
    if((*i)->eventEmpty()) {
      r = port_dissociate(_port, PORT_SOURCE_FD, (*i)->getSocket());
      _socketEntries.erase(i);
    } else {
      A2PortEvent pv = (*i)->getEvents();
      r = port_associate(_port, PORT_SOURCE_FD, (*i)->getSocket(),
                         pv.events, pv.socketEntry);
    }
    if(r == -1) {
      if(_logger->debug()) {
        _logger->debug("Failed to delete socket event:%s", strerror(errno));
      }
      return false;
    } else {
      return true;
    }
  } else {
    if(_logger->debug()) {
      _logger->debug("Socket %d is not found in SocketEntries.", socket);
    }
    return false;
  }
}

#ifdef ENABLE_ASYNC_DNS
bool PortEventPoll::deleteEvents(sock_t socket, Command* command,
                                 const SharedHandle<AsyncNameResolver>& rs)
{
  return deleteEvents(socket, KADNSEvent(rs, command, socket, 0));
}
#endif // ENABLE_ASYNC_DNS

bool PortEventPoll::deleteEvents(sock_t socket, Command* command,
                                 EventPoll::EventType events)
{
  int portEvents = translateEvents(events);
  return deleteEvents(socket, KCommandEvent(command, portEvents));
}

#ifdef ENABLE_ASYNC_DNS
bool PortEventPoll::addNameResolver
(const SharedHandle<AsyncNameResolver>& resolver, Command* command)
{
  SharedHandle<KAsyncNameResolverEntry> entry
    (new KAsyncNameResolverEntry(resolver, command));
  std::deque<SharedHandle<KAsyncNameResolverEntry> >::iterator itr =
    std::find(_nameResolverEntries.begin(), _nameResolverEntries.end(), entry);
  if(itr == _nameResolverEntries.end()) {
    _nameResolverEntries.push_back(entry);
    entry->addSocketEvents(this);
    return true;
  } else {
    return false;
  }
}

bool PortEventPoll::deleteNameResolver
(const SharedHandle<AsyncNameResolver>& resolver, Command* command)
{
  SharedHandle<KAsyncNameResolverEntry> entry
    (new KAsyncNameResolverEntry(resolver, command));
  std::deque<SharedHandle<KAsyncNameResolverEntry> >::iterator itr =
    std::find(_nameResolverEntries.begin(), _nameResolverEntries.end(), entry);
  if(itr == _nameResolverEntries.end()) {
    return false;
  } else {
    (*itr)->removeSocketEvents(this);
    _nameResolverEntries.erase(itr);
    return true;
  }
}
#endif // ENABLE_ASYNC_DNS

} // namespace aria2