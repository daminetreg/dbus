/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-transport.c DBusTransport object (internal to D-BUS implementation)
 *
 * Copyright (C) 2002  Red Hat Inc.
 *
 * Licensed under the Academic Free License version 1.2
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dbus-transport-protected.h"
#include "dbus-transport-unix.h"
#include "dbus-connection-internal.h"
#include "dbus-watch.h"
#include "dbus-auth.h"

/**
 * @defgroup DBusTransport DBusTransport object
 * @ingroup  DBusInternals
 * @brief "Backend" for a DBusConnection.
 *
 * Types and functions related to DBusTransport.  A transport is an
 * abstraction that can send and receive data via various kinds of
 * network connections or other IPC mechanisms.
 * 
 * @{
 */

/**
 * @typedef DBusTransport
 *
 * Opaque object representing a way message stream.
 * DBusTransport abstracts various kinds of actual
 * transport mechanism, such as different network protocols,
 * or encryption schemes.
 */

/**
 * Refs a transport and associated connection for reentrancy.
 *
 * @todo this macro reflects a design mistake, which is that the
 * transport has a pointer to its connection. Ownership should move in
 * only one direction; the connection should push/pull from the
 * transport, rather than vice versa. Then the connection would take
 * care of referencing itself when needed.
 */
#define DBUS_TRANSPORT_HOLD_REF(t) \
  _dbus_transport_ref (t); if ((t)->connection) dbus_connection_ref ((t)->connection)

/**
 * Inverse of DBUS_TRANSPORT_HOLD_REF().
 */
#define DBUS_TRANSPORT_RELEASE_REF(t) \
  if ((t)->connection) dbus_connection_unref ((t)->connection); _dbus_transport_unref (t)


/**
 * Initializes the base class members of DBusTransport.
 * Chained up to by subclasses in their constructor.
 *
 * @param transport the transport being created.
 * @param vtable the subclass vtable.
 * @param server #TRUE if this transport is on the server side of a connection
 * @returns #TRUE on success.
 */
dbus_bool_t
_dbus_transport_init_base (DBusTransport             *transport,
                           const DBusTransportVTable *vtable,
                           dbus_bool_t                server)
{
  DBusMessageLoader *loader;
  DBusAuth *auth;
  
  loader = _dbus_message_loader_new ();
  if (loader == NULL)
    return FALSE;

  if (server)
    auth = _dbus_auth_server_new ();
  else
    auth = _dbus_auth_client_new ();
  if (auth == NULL)
    {
      _dbus_message_loader_unref (loader);
      return FALSE;
    }
  
  transport->refcount = 1;
  transport->vtable = vtable;
  transport->loader = loader;
  transport->auth = auth;
  transport->authenticated = FALSE;
  transport->messages_need_sending = FALSE;
  transport->disconnected = FALSE;
  
  return TRUE;
}

/**
 * Finalizes base class members of DBusTransport.
 * Chained up to from subclass finalizers.
 *
 * @param transport the transport.
 */
void
_dbus_transport_finalize_base (DBusTransport *transport)
{
  if (!transport->disconnected)
    _dbus_transport_disconnect (transport);

  _dbus_message_loader_unref (transport->loader);
  _dbus_auth_unref (transport->auth);
}

/**
 * Opens a new transport for the given address.  (This opens a
 * client-side-of-the-connection transport.)
 *
 * @todo right now the address is just a Unix domain socket path.
 * 
 * @param address the address.
 * @param result location to store reason for failure.
 * @returns new transport of #NULL on failure.
 */
DBusTransport*
_dbus_transport_open (const char     *address,
                      DBusResultCode *result)
{
  DBusTransport *transport;
  
  /* FIXME parse the address - whatever format
   * we decide addresses are in - and find the
   * appropriate transport.
   */

  /* Pretend it's just a unix domain socket name for now */
  transport = _dbus_transport_new_for_domain_socket (address,
                                                     FALSE,
                                                     result);
  
  return transport;
}

/**
 * Increments the reference count for the transport.
 *
 * @param transport the transport.
 */
void
_dbus_transport_ref (DBusTransport *transport)
{
  transport->refcount += 1;
}

/**
 * Decrements the reference count for the transport.
 * Disconnects and finalizes the transport if
 * the reference count reaches zero.
 *
 * @param transport the transport.
 */
void
_dbus_transport_unref (DBusTransport *transport)
{
  _dbus_assert (transport != NULL);
  _dbus_assert (transport->refcount > 0);

  transport->refcount -= 1;
  if (transport->refcount == 0)
    {
      _dbus_assert (transport->vtable->finalize != NULL);
      
      (* transport->vtable->finalize) (transport);
    }
}

/**
 * Closes our end of the connection to a remote application. Further
 * attempts to use this transport will fail. Only the first call to
 * _dbus_transport_disconnect() will have an effect.
 *
 * @param transport the transport.
 * 
 */
void
_dbus_transport_disconnect (DBusTransport *transport)
{
  _dbus_assert (transport->vtable->disconnect != NULL);

  if (transport->disconnected)
    return;

  DBUS_TRANSPORT_HOLD_REF (transport);
  (* transport->vtable->disconnect) (transport);

  transport->disconnected = TRUE;
  DBUS_TRANSPORT_RELEASE_REF (transport);
}

/**
 * Returns #TRUE if the transport has not been disconnected.
 * Disconnection can result from _dbus_transport_disconnect()
 * or because the server drops its end of the connection.
 *
 * @param transport the transport.
 * @returns whether we're connected
 */
dbus_bool_t
_dbus_transport_get_is_connected (DBusTransport *transport)
{
  return !transport->disconnected;
}

/**
 * Returns #TRUE if we have been authenticated.  Will return #TRUE
 * even if the transport is disconnected.
 *
 * @param transport the transport
 * @returns whether we're authenticated
 */
dbus_bool_t
_dbus_transport_get_is_authenticated (DBusTransport *transport)
{  
  if (transport->authenticated)
    return TRUE;
  else
    {
      transport->authenticated =
        _dbus_auth_do_work (transport->auth) == DBUS_AUTH_STATE_AUTHENTICATED;

      return transport->authenticated;
    }
}

/**
 * Handles a watch by reading data, writing data, or disconnecting
 * the transport, as appropriate for the given condition.
 *
 * @param transport the transport.
 * @param watch the watch.
 * @param condition the current state of the watched file descriptor.
 */
void
_dbus_transport_handle_watch (DBusTransport           *transport,
                              DBusWatch               *watch,
                              unsigned int             condition)
{
  _dbus_assert (transport->vtable->handle_watch != NULL);

  if (transport->disconnected)
    {
      _dbus_connection_transport_error (transport->connection,
                                        DBUS_RESULT_DISCONNECTED);
      return;
    }

  if (dbus_watch_get_fd (watch) < 0)
    {
      _dbus_warn ("Tried to handle an invalidated watch; this watch should have been removed\n");
      return;
    }
  
  _dbus_watch_sanitize_condition (watch, &condition);

  DBUS_TRANSPORT_HOLD_REF (transport);
  _dbus_watch_ref (watch);
  (* transport->vtable->handle_watch) (transport, watch, condition);
  _dbus_watch_unref (watch);
  DBUS_TRANSPORT_RELEASE_REF (transport);
}

/**
 * Sets the connection using this transport. Allows the transport
 * to add watches to the connection, queue incoming messages,
 * and pull outgoing messages.
 *
 * @param transport the transport.
 * @param connection the connection.
 */
void
_dbus_transport_set_connection (DBusTransport  *transport,
                                DBusConnection *connection)
{
  _dbus_assert (transport->vtable->connection_set != NULL);
  _dbus_assert (transport->connection == NULL);
  
  transport->connection = connection;

  DBUS_TRANSPORT_HOLD_REF (transport);
  (* transport->vtable->connection_set) (transport);
  DBUS_TRANSPORT_RELEASE_REF (transport);
}

/**
 * Notifies the transport when the outgoing message queue goes from
 * empty to non-empty or vice versa. Typically causes the transport to
 * add or remove its DBUS_WATCH_WRITABLE watch.
 *
 * @param transport the transport.
 * @param queue_length the length of the outgoing message queue.
 *
 */
void
_dbus_transport_messages_pending (DBusTransport  *transport,
                                  int             queue_length)
{
  _dbus_assert (transport->vtable->messages_pending != NULL);

  if (transport->disconnected)
    {
      _dbus_connection_transport_error (transport->connection,
                                        DBUS_RESULT_DISCONNECTED);
      return;
    }

  transport->messages_need_sending = queue_length > 0;

  DBUS_TRANSPORT_HOLD_REF (transport);
  (* transport->vtable->messages_pending) (transport,
                                           queue_length);
  DBUS_TRANSPORT_RELEASE_REF (transport);
}

/**
 * Performs a single poll()/select() on the transport's file
 * descriptors and then reads/writes data as appropriate,
 * queueing incoming messages and sending outgoing messages.
 * This is the backend for _dbus_connection_do_iteration().
 * See _dbus_connection_do_iteration() for full details.
 *
 * @param transport the transport.
 * @param flags indicates whether to read or write, and whether to block.
 * @param timeout_milliseconds if blocking, timeout or -1 for no timeout.
 */
void
_dbus_transport_do_iteration (DBusTransport  *transport,
                              unsigned int    flags,
                              int             timeout_milliseconds)
{
  _dbus_assert (transport->vtable->do_iteration != NULL);

  if ((flags & (DBUS_ITERATION_DO_WRITING |
                DBUS_ITERATION_DO_READING)) == 0)
    return; /* Nothing to do */

  if (transport->disconnected)
    {
      _dbus_connection_transport_error (transport->connection,
                                        DBUS_RESULT_DISCONNECTED);
      return;
    }

  DBUS_TRANSPORT_HOLD_REF (transport);
  (* transport->vtable->do_iteration) (transport, flags,
                                       timeout_milliseconds);
  DBUS_TRANSPORT_RELEASE_REF (transport);
}

/** @} */
