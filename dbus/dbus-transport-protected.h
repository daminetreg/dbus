/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-transport-protected.h Used by subclasses of DBusTransport object (internal to D-BUS implementation)
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
#ifndef DBUS_TRANSPORT_PROTECTED_H
#define DBUS_TRANSPORT_PROTECTED_H

#include <dbus/dbus-internals.h>
#include <dbus/dbus-errors.h>
#include <dbus/dbus-transport.h>
#include <dbus/dbus-message-internal.h>
#include <dbus/dbus-auth.h>

DBUS_BEGIN_DECLS;

typedef struct DBusTransportVTable DBusTransportVTable;

struct DBusTransportVTable
{
  void (* finalize)           (DBusTransport *transport);
  /**< The finalize method must free the transport. */

  void (* handle_watch)       (DBusTransport *transport,
                               DBusWatch     *watch,
                               unsigned int   flags);
  /**< The handle_watch method handles reading/writing
   * data as indicated by the flags.
   */

  void (* disconnect)         (DBusTransport *transport);
  /**< Disconnect this transport. */

  void (* connection_set)     (DBusTransport *transport);
  /**< Called when transport->connection has been filled in */

  void (* messages_pending)   (DBusTransport *transport,
                               int            queue_length);
  /**< Called when the outgoing message queue goes from empty
   * to non-empty or vice versa.
   */

  void (* do_iteration)       (DBusTransport *transport,
                               unsigned int   flags,
                               int            timeout_milliseconds);
  /**< Called to do a single "iteration" (block on select/poll
   * followed by reading or writing data).
   */
};

struct DBusTransport
{
  int refcount;                               /**< Reference count. */

  const DBusTransportVTable *vtable;          /**< Virtual methods for this instance. */

  DBusConnection *connection;                 /**< Connection owning this transport. */

  DBusMessageLoader *loader;                  /**< Message-loading buffer. */

  DBusAuth *auth;                             /**< Authentication conversation */

  unsigned int disconnected : 1;              /**< #TRUE if we are disconnected. */
  unsigned int authenticated : 1;             /**< Cache of auth state; use _dbus_transport_get_is_authenticated() to query value */
  unsigned int messages_need_sending : 1;     /**< #TRUE if we need to write messages out */
};

dbus_bool_t _dbus_transport_init_base     (DBusTransport             *transport,
                                           const DBusTransportVTable *vtable,
                                           dbus_bool_t                server);
void        _dbus_transport_finalize_base (DBusTransport             *transport);



DBUS_END_DECLS;

#endif /* DBUS_TRANSPORT_PROTECTED_H */
