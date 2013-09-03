/**
 * @file quiescing_manager.cpp
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

// Common STL includes.
#include <cassert>

#include "log.h"
#include "utils.h"
#include "pjutils.h"
#include "quiescing_manager.h"


SynchronizedFSM::SynchronizedFSM() :
  _input_q(),
  _running(false)
{
  pthread_mutex_init(&_lock, NULL);
}

SynchronizedFSM::~SynchronizedFSM() :
{
  pthread_mutex_destroy(&_lock);
}

void SynchronizedFSM::send_input(int input)
{
  pthread_mutex_lock(&_lock);

  // Queue the new input, even if we can't process it immediately.
  _input_q.push(input);

  if (!_running)
  {
    // The FSM is not already running. Set a flag to show that it is running (so
    // now other threads will attempt to run it at the same time we are).
    _running = true;

    // Process all the inputs on the queue.  For each one remove it from the
    // queue and call process_input (which is implemented by the subclass).
    while (!_input_q.empty())
    {
      int next_input = _input_q.front();
      _input_q.pop();

      // Drop the lock when calling process_input.  This allows the FSM to be
      // re-entrant.
      pthread_mutex_unlock(&_lock);
      process_input(next_input);
      pthread_mutex_lock(&_lock);
    }

    _running = false;
  }

  pthread_mutex_unlock(&_lock);
}


QuiescingManager::QuiescingManager(bool edge_proxy,
                                   ConnectionTracker *connection_tracker) :
  SynchronizedFSM(),
  _conn_tracker(connection_tracker)
  _edge_proxy(edge_proxy)
{}


// Implement the state machine descibed in the comments for the QuiescingManager
// class.
void QuiescingManager::process_input(int input)
{
  // Check that we're in a valid state and have received a valid input.
  assert((input == INPUT_QUIESCE) ||
         (input == INPUT_FLOWS_GONE) ||
         (input == INPUT_CONNS_GONE) ||
         (input == INPUT_UNQUIESCE));
  assert((_state == STATE_ACTIVE) ||
         (_state == STATE_QUIESCING_FLOWS) ||
         (_state == STATE_QUIESCING_CONNS) ||
         (_state == STATE_QUIESCED));

  switch (_state)
  {
    case STATE_ACTIVE:

      switch (input)
      {
        case INPUT_QUIESCE:
          _state = QUIESCING_FLOWS;
          quiesce_untrusted_interface();
          break;

        case INPUT_FLOWS_GONE:
        case INPUT_CONNS_GONE:
          // No-op.
          break;

        case INPUT_UNQUIESCE:
          invalid_input(input, _state);
          break;
      }

    case STATE_QUIESCING_FLOWS:

      switch (input)
      {
        case INPUT_QUIESCE:
          invalid_input(input, _state);
          break;

        case INPUT_FLOWS_GONE:
          _state = QUIESCING_CONNS;
          quiesce_connections();
          break;

        case INPUT_CONNS_GONE:
          // No-op.
          break;

        case INPUT_UNQUIESCE:
          _state = ACTIVE;
          unquiesce_untrusted_interface();
          break;
      }
      break;

    case STATE_QUIESCING_CONNS:

      switch (input)
      {
        case INPUT_QUIESCE:
          invalid_input(input, _state);
          break;

        case INPUT_FLOWS_GONE:
          // No-op.
          break;

        case INPUT_CONNS_GONE:
          _state = QUIESCED;
          quiesce_complete();
          break;

        case INPUT_UNQUIESCE:
          _state = ACTIVE;
          unquiesce_connections();
          unquiesce_untrusted_interface();
          break;
      }
      break;

    case STATE_QUIESCED:

      switch (input)
      {
        case INPUT_QUIESCE:
        case INPUT_UNQUIESCE:
          // No-op.
          break;

        case INPUT_FLOWS_GONE:
        case INPUT_CONNS_GONE:
          invalid_input(input, _state);
          break;
      }
      break;
  }
}


void QuiescingManager::invalid_input(int input, int state)
{
  LOG_ERROR("The Quiescing Manager received an invalid input %s (%d) "
            "when in state %s (%d)",
            INPUT_NAMES[input], input,
            STATE_NAMES[state], state);
  assert(false);
}


void QuiescingManager::quiesce_untrusted_interface()
{
  if (_edge_proxy)
  {
    // Close the untructed listening port.  This prevents any new clinets from
    // connecting. TODO

    // Instruct the FlowTable to quiesce.  This waits until all flows have
    // expired, at which case it calls flows_gone(). TODO.
  }
  else
  {
    // We're not on an edge proxy so there aren't any flows.
    flows_gone();
  }
}


void QuiescingManager::quiesce_connections()
{
  // Close the trusted listening port.  This prevents any new connections from
  // being established (note that on an edge proxy we should already have closed
  // the untructed listening port).  TODO

  // Quiesce open connections.  This will close them when they no longer have
  // any outstanding transactions.  When this process has completed the
  // connection tracker will call connections_gone().
  _conn_tracker->quiesce();
}

void QuiescingManager::quiesce_complete()
{
  // Notify the stack module that quiescing is now complete. TODO
}

void QuiescingManager::unquiesce_connections()
{
  // Repoen the untrusted listening port. TODO

  _conn_tracker->unquiesce();
}

void QuiescingManager::unquiesce_untrusted_interface()
{
  if (_edge_proxy)
  {
    // Reopen untrusted listening port. TODO

    // Take the FlowTable out of quiescing mode.
  }
}

